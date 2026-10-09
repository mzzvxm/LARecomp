#pragma once
// ===========================================================================
// MCLA GPU renderer -- the SDK overlays (Dear ImGui dialogs, F3, toasts)
// ===========================================================================
// The SDK draws its overlays through a rex::ui::ImmediateDrawer the app
// provides in detached mode (ReXApp::OnCreateImmediateDrawer). reblue and
// reeot record that drawer's commands from inside their Present, with the
// render (or guest) thread blocked while the UI thread fills its list. MCLA
// keeps F3 and the achievement toasts up during gameplay, so that would stall
// every frame with an overlay on the UI thread waking up, and the guest waits
// in Present behind it. Here the two sides only exchange CPU data:
//
//   UI thread                                render thread
//   OverlayHost::RunPass                     OverlayRenderer::Prepare / Draw
//     ImGuiDrawer::Draw(AppUIDrawContext)      takes texture uploads, deaths and
//       OverlayDrawer records an                the newest OverlayFrame from the
//       OverlayFrame in CPU memory              channel; creates textures, frees
//       and publishes it to the channel         dead ones, draws the frame it
//   OverlayDrawer::CreateTexture                retains over the back buffer
//     returns at once; pixels queued          OverlayHost::RequestPass
//   ~OverlayTexture: id queued as dead          asks the UI thread for the next
//                                               frame (never waits for it)
//
// One frame of overlay latency; the render thread never waits on the UI, and
// the render thread stays the only thread that records GPU commands.
//
// Lifetime: the channel is shared (shared_ptr) by the drawer, which the SDK
// owns and creates before the renderer exists, the renderer, and the module.
// The host is shared by the module and every pass posted to the UI thread,
// and is switched off on the UI thread before the SDK destroys its ImGui
// drawer (gpu::Shutdown runs from OnWindowCloseRequested / OnShutdown).
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui {
class ImGuiDrawer;
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;
class UploadRing;

inline constexpr uint32_t kOverlayNoTexture = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// CPU side
// ---------------------------------------------------------------------------

struct OverlayDraw {
  // Indexed: `count` indices from `first_index`, added to `base_vertex`.
  // Not indexed: `count` vertices from `base_vertex`.
  uint32_t count = 0;
  uint32_t first_index = 0;
  int32_t base_vertex = 0;
  uint32_t texture_id = kOverlayNoTexture;
  bool indexed = false;
  bool lines = false;
  bool scissor = false;
  // Scissor in the frame's coordinate space (converted to back-buffer pixels
  // when drawn, so a frame recorded at another size still clips right).
  float scissor_left = 0.0f;
  float scissor_top = 0.0f;
  float scissor_right = 0.0f;
  float scissor_bottom = 0.0f;
};

struct OverlayFrame {
  uint64_t sequence = 0;
  float width = 0.0f;   // coordinate space
  float height = 0.0f;
  std::vector<rex::ui::ImmediateVertex> vertices;
  std::vector<uint16_t> indices;  // relative to each draw's base_vertex
  std::vector<OverlayDraw> draws;

  bool empty() const { return draws.empty(); }
  bool References(uint32_t texture_id) const;
  void Clear();
};

struct OverlayTextureUpload {
  uint32_t id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool linear = true;
  bool repeat = false;
  std::vector<uint8_t> rgba;  // width * height * 4, tightly packed
};

// What the render thread takes from the channel in one go.
struct OverlayIntake {
  std::vector<OverlayTextureUpload> uploads;
  std::vector<uint32_t> deaths;
  std::unique_ptr<OverlayFrame> frame;  // null when nothing new was published
};

class OverlayChannel {
 public:
  // UI thread.
  uint32_t CreateTexture(uint32_t width, uint32_t height, bool linear, bool repeat,
                         const uint8_t* rgba);
  void DestroyTexture(uint32_t id);
  std::unique_ptr<OverlayFrame> AcquireFrame();
  void Publish(std::unique_ptr<OverlayFrame> frame);

  // Render thread. Moves everything pending into `out` (whose vectors keep
  // their capacity across calls).
  void Take(OverlayIntake& out);
  // Render thread. Whether a frame newer than the last Take is waiting, and if
  // so whether it has draws.
  bool PeekNewFrame(bool* has_draws);
  void Recycle(std::unique_ptr<OverlayFrame> frame);
  // The renderer is gone: drop queued pixels and stop queueing more. Ids are
  // still handed out, so the SDK's textures stay valid objects.
  void Close();

 private:
  std::mutex mutex_;
  std::unique_ptr<OverlayFrame> latest_;
  std::vector<std::unique_ptr<OverlayFrame>> free_frames_;
  std::vector<OverlayTextureUpload> uploads_;
  std::vector<uint32_t> deaths_;
  uint64_t published_ = 0;
  uint32_t next_texture_id_ = 1;
  bool closed_ = false;
};

// The ImmediateTexture the SDK holds: an id into the channel.
class OverlayTexture final : public rex::ui::ImmediateTexture {
 public:
  OverlayTexture(uint32_t width, uint32_t height, uint32_t id,
                 std::shared_ptr<OverlayChannel> channel)
      : ImmediateTexture(width, height), id_(id), channel_(std::move(channel)) {}
  ~OverlayTexture() override { channel_->DestroyTexture(id_); }
  uint32_t id() const { return id_; }

 private:
  uint32_t id_;
  std::shared_ptr<OverlayChannel> channel_;
};

// The drawer handed to the SDK. Records frames; never touches the GPU.
class OverlayDrawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit OverlayDrawer(std::shared_ptr<OverlayChannel> channel);

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter filter,
                                                           bool is_repeated,
                                                           const uint8_t* data) override;
  void Begin(rex::ui::UIDrawContext& ui_draw_context, float space_width,
             float space_height) override;
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override;
  void Draw(const rex::ui::ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

  // Publishes a frame with nothing in it, for a UI pass in which the ImGui
  // drawer drew nothing (it returns before Begin when it has no dialog).
  void PublishEmpty(float width, float height);
  uint64_t ended_passes() const { return ended_passes_; }

 private:
  std::shared_ptr<OverlayChannel> channel_;
  std::unique_ptr<OverlayFrame> frame_;  // between Begin and End
  uint32_t batch_vertex_base_ = 0;
  uint32_t batch_index_base_ = 0;
  uint32_t batch_vertex_count_ = 0;
  uint32_t batch_index_count_ = 0;
  bool batch_open_ = false;
  bool batch_indexed_ = false;
  uint64_t ended_passes_ = 0;
  bool reported_bad_draw_ = false;
};

// Runs the SDK's ImGui drawer on the UI thread when the render thread asks.
class OverlayHost : public std::enable_shared_from_this<OverlayHost> {
 public:
  OverlayHost(rex::ui::WindowedAppContext& app_context, rex::ui::Window& window,
              rex::ui::ImGuiDrawer& imgui_drawer, OverlayDrawer& drawer);

  // Any thread. Posts one pass to the UI thread unless one is pending.
  void RequestPass();
  // After this no pass touches the SDK objects again. Called on the UI thread
  // before the SDK destroys them.
  void Disconnect();

 private:
  void RunPass();  // UI thread

  rex::ui::WindowedAppContext& app_context_;
  // Read on the UI thread only, and only while connected_.
  rex::ui::Window* window_;
  rex::ui::ImGuiDrawer* imgui_drawer_;
  OverlayDrawer* drawer_;
  std::atomic<bool> pass_pending_{false};
  std::atomic<bool> connected_{true};
};

// ---------------------------------------------------------------------------
// GPU side (render thread)
// ---------------------------------------------------------------------------

class OverlayRenderer {
 public:
  OverlayRenderer() = default;
  ~OverlayRenderer();
  OverlayRenderer(const OverlayRenderer&) = delete;
  OverlayRenderer& operator=(const OverlayRenderer&) = delete;

  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload,
                  std::shared_ptr<OverlayChannel> channel, plume::RenderFormat target_format);
  // The GPU must be idle and the FrameRing drained. Closes the channel.
  void Shutdown();

  // Whether a present would show something the window does not show yet: a
  // newer frame with content, or one that empties a retained frame with
  // content. Used to decide idle presents.
  bool HasNewContent();

  // Outside a render pass: creates the textures queued since the last call
  // (uploads recorded on `list`), frees dead ones the retained frame no longer
  // uses, and retains the newest frame.
  void Prepare(plume::RenderCommandList* list, uint32_t slot);
  // Inside the back buffer's framebuffer. Returns the draws recorded.
  uint32_t Draw(plume::RenderCommandList* list, uint32_t slot, uint32_t target_width,
                uint32_t target_height);

  size_t texture_count() const { return textures_.size(); }

 private:
  struct GpuTexture {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = 0;
    uint32_t sampler = 0;
  };

  bool CreatePipelines(plume::RenderFormat target_format);
  void CreateTexture(plume::RenderCommandList* list, uint32_t slot,
                     const OverlayTextureUpload& upload);
  void ReleaseTexture(GpuTexture& texture);
  void ReleaseDeadTextures();

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  UploadRing* upload_ = nullptr;
  std::shared_ptr<OverlayChannel> channel_;

  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> vertex_shader_;
  std::unique_ptr<plume::RenderShader> pixel_shader_;
  std::unique_ptr<plume::RenderPipeline> triangle_pipeline_;
  std::unique_ptr<plume::RenderPipeline> line_pipeline_;

  std::unordered_map<uint32_t, GpuTexture> textures_;
  std::unordered_set<uint32_t> failed_textures_;    // creation failed; logged once
  std::unordered_set<uint32_t> reported_missing_;   // drawn but unknown; logged once
  std::vector<uint32_t> pending_deaths_;
  OverlayIntake intake_;
  std::unique_ptr<OverlayFrame> retained_;
  bool reported_first_frame_ = false;
};

}  // namespace mcla::gpu
