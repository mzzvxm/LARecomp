#pragma once
// ===========================================================================
// MCLA GPU renderer -- the SDK overlays (Dear ImGui dialogs, F3, toasts)
// ===========================================================================
// rexglue 0.11 draws its overlays through IUISystem::DrawOverlays(IOverlayRenderer&): the UI thread
// builds the ImGui frame, and the renderer's RenderOverlays(ImDrawData&) draws it on the thread that
// called. The SDK's own present thread does that for its Xenia drivers; this renderer presents on a
// render thread of its own (the "larecomp" driver's BeginPresent says "no image"), so it asks.
//
// MCLA keeps F3 and the achievement toasts up during gameplay. A DrawOverlays call made on the render
// thread waits for the UI thread to build the frame (RunSynchronous), which would put a UI-thread wake-up
// into every presented frame, and a UI thread that is busy (the save wait of a close, a nested loop) would
// stall the picture. So the call is made on the UI thread instead, and the two sides only exchange CPU data:
//
//   render thread                            UI thread
//   OverlayHost::RequestPass                 OverlayHost::RunPass (a PostTask)
//     posts one pass, never waits              IUISystem::DrawOverlays(OverlayCapture)
//   OverlayRenderer::Prepare / Draw              OverlayCapture::RenderOverlays copies the ImDrawData
//     takes texture uploads, deaths and          into an OverlayFrame in CPU memory, turns the
//     the newest OverlayFrame from the           ImTextureData requests (the font atlas, icons the dialogs
//     channel; creates textures, frees           register) into queued pixels, and publishes both
//     dead ones, draws the frame it
//     retains over the back buffer
//
// One frame of overlay latency; the render thread never waits on the UI, and the render thread stays the
// only thread that records GPU commands. DrawOverlays itself holds the SDK's frame lock and returns at
// once on the UI thread (RunSynchronous runs inline there).
//
// Textures: ImGui 1.92 owns them. Every ImTextureData in ImDrawData::Textures (the font atlas, and each
// texture a dialog made with ImGui::RegisterUserTexture, ui/overlay_texture.h) with a status other than OK
// is created, replaced or destroyed here, and gets its TexID (a channel id) before the draw commands that
// name it are read. A texture that leaves the list is destroyed too.
//
// Lifetime: the channel is shared (shared_ptr) by the capture, the renderer and the module. The host is
// shared by the module and every pass posted to the UI thread, and is switched off on the UI thread before
// the UI system stops (gpu::Shutdown runs from LarecompApp::OnStop).
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <rex/overlay_renderer.h>

#include "hdr.h"

struct ImDrawData;
struct ImTextureData;

namespace rex::ui {
class IUISystem;
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

// ImDrawVert as the pipeline reads it: position, texture coordinate, RGBA8 colour.
struct OverlayVertex {
  float x = 0.0f;
  float y = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
  uint32_t color = 0;
};
static_assert(sizeof(OverlayVertex) == 20);

struct OverlayDraw {
  // `count` indices from `first_index` (16-bit, relative to the draw's vertices), added to `base_vertex`.
  uint32_t count = 0;
  uint32_t first_index = 0;
  int32_t base_vertex = 0;
  uint32_t texture_id = kOverlayNoTexture;
  // Scissor in the frame's coordinate space (converted to back-buffer pixels
  // when drawn, so a frame recorded at another size still clips right).
  float scissor_left = 0.0f;
  float scissor_top = 0.0f;
  float scissor_right = 0.0f;
  float scissor_bottom = 0.0f;
};

struct OverlayFrame {
  uint64_t sequence = 0;
  float width = 0.0f;   // coordinate space (ImDrawData::DisplaySize)
  float height = 0.0f;
  std::vector<OverlayVertex> vertices;
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
  // New pixels (and size) for an id that exists: the render thread makes a new GPU texture under the same
  // id and retires the old one, so a frame still in flight keeps reading what it was recorded with.
  void ReplaceTexture(uint32_t id, uint32_t width, uint32_t height, bool linear, bool repeat,
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
  // still handed out, so a texture the UI thread holds stays a valid object.
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

// The IOverlayRenderer the UI system draws into: copies one ImDrawData into a frame of the channel and
// serves its texture requests. UI thread only (RenderOverlays runs on the thread that called
// IUISystem::DrawOverlays).
class OverlayCapture final : public rex::IOverlayRenderer {
 public:
  explicit OverlayCapture(std::shared_ptr<OverlayChannel> channel) : channel_(std::move(channel)) {}

  void RenderOverlays(ImDrawData& draw_data) override;

  // Around one DrawOverlays call: it returns without a RenderOverlays when no dialog is open, and then the
  // frame the last pass left must be replaced by an empty one (PublishEmpty), or a closed dialog stays on.
  void Begin() { called_ = false; }
  bool called() const { return called_; }
  void PublishEmpty();

 private:
  void ServeTextures(ImDrawData& draw_data);

  std::shared_ptr<OverlayChannel> channel_;
  // The ImGui-owned textures that have a channel id, and the pass that last saw each.
  struct Tracked {
    uint32_t id = 0;
    uint64_t seen = 0;
  };
  std::unordered_map<ImTextureData*, Tracked> tracked_;
  uint64_t pass_ = 0;
  bool called_ = false;
};

// Asks the UI thread for the next overlay frame. Never waits for it.
class OverlayHost : public std::enable_shared_from_this<OverlayHost> {
 public:
  OverlayHost(rex::ui::IUISystem& ui, std::shared_ptr<OverlayChannel> channel);

  // Any thread. Posts one pass to the UI thread unless one is pending.
  void RequestPass();
  // After this no pass touches the UI system again. Called on the UI thread
  // before the UI system stops.
  void Disconnect();

 private:
  void RunPass();  // UI thread

  // Read on the UI thread only, and only while connected_ (PostTask itself is any-thread).
  rex::ui::IUISystem* ui_;
  OverlayCapture capture_;
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

  // `hdr`: the target is an HDR10 back buffer (SwapChain::hdr()), `target_format` its format. The
  // SDK's colours are then placed at paper white and PQ encoded by the pixel shader, and blended
  // in those code values.
  bool Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, UploadRing& upload,
                  std::shared_ptr<OverlayChannel> channel, plume::RenderFormat target_format,
                  bool hdr);
  // The GPU must be idle and the FrameRing drained. Closes the channel.
  void Shutdown();

  // The pixel shader and the pipelines come in two sets, for an SDR and for an HDR10 back buffer
  // (mcla_gpu_hdr is live: Renderer::SwitchHdr). Initialize made the one it was given and uses it.
  // BuildTarget makes the other, once and without using it; false (logged) when it could not be
  // made, with nothing changed. UseTarget then draws with it, for the back buffer the swap chain
  // has made from now on; nothing is destroyed. Render thread, at a frame boundary.
  bool BuildTarget(plume::RenderFormat target_format, bool hdr);
  void UseTarget(bool hdr);
  bool has_target(bool hdr) const { return targets_[hdr ? 1 : 0].built; }

  // Whether a present would show something the window does not show yet: a
  // newer frame with content, or one that empties a retained frame with
  // content. Used to decide idle presents.
  bool HasNewContent();

  // Outside a render pass: creates (or replaces) the textures queued since the last call
  // (uploads recorded on `list`), frees dead ones the retained frame no longer
  // uses, and retains the newest frame.
  void Prepare(plume::RenderCommandList* list, uint32_t slot);
  // Inside the back buffer's framebuffer. Returns the draws recorded.
  uint32_t Draw(plume::RenderCommandList* list, uint32_t slot, uint32_t target_width,
                uint32_t target_height);

  // HDR10 only: the levels Draw puts in its constants, each frame before it.
  void SetHdrLevels(const HdrLevels& levels) { hdr_levels_ = levels; }

  size_t texture_count() const { return textures_.size(); }

 private:
  struct GpuTexture {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t slot = 0;
    uint32_t sampler = 0;
  };

  // The layout and the vertex shader, shared by both sets.
  bool CreateShared();
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
  // The pixel shader and the pipelines for an SDR ([0]) and an HDR10 ([1]) back buffer.
  struct Target {
    bool built = false;
    std::unique_ptr<plume::RenderShader> pixel_shader;
    std::unique_ptr<plume::RenderPipeline> triangle_pipeline;
  };
  Target targets_[2];
  // The set in use (owned by targets_).
  plume::RenderPipeline* triangle_pipeline_ = nullptr;
  bool hdr_ = false;  // the target is HDR10: the pixel shader is overlay_hdr_ps
  HdrLevels hdr_levels_;

  std::unordered_map<uint32_t, GpuTexture> textures_;
  std::unordered_set<uint32_t> failed_textures_;    // creation failed; logged once
  std::unordered_set<uint32_t> reported_missing_;   // drawn but unknown; logged once
  std::vector<uint32_t> pending_deaths_;
  OverlayIntake intake_;
  std::unique_ptr<OverlayFrame> retained_;
  bool reported_first_frame_ = false;
};

}  // namespace mcla::gpu
