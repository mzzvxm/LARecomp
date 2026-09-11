#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — multi-draw frame capture
// ===========================================================================
// first_draw.* proved a single draw end to end. This scales that to N draws
// accumulated into one render target, which is what exposes the failures a
// single draw cannot: stale descriptors, resources left in the wrong state,
// PSO/vertex-layout mismatches between draws, texture slots bleeding from one
// draw into the next, and depth/MSAA interactions.
//
// Draws are grouped by render target configuration: the first accepted draw
// fixes the viewport, formats and sample count, and later draws are only
// accumulated while they still match. That is what isolates the main scene
// pass from the reflection / shadow / cube passes around it without needing
// to know anything about the engine's frame structure.
//
// Each draw is submitted on its own command list rather than batching them.
// It is slower, but the render target persists across submissions, so the
// accumulated image is identical, and a fault can be attributed to the exact
// draw that caused it.
// ===========================================================================

#include <cstdint>

namespace rex::ui {
class Presenter;
}

namespace mcla::native_gfx {

class D3D12Context;
class ShaderDatabase;
class BufferCache;
class TextureCache;
class TextureBinder;
class PipelineCache;
class RenderTargetPool;
class PresenterOutput;
class BlitPass;

// True once the requested number of draws has been captured and written out.
bool FrameCaptureDone();

// Continuous mode: draws accumulate exactly as in capture, but the frame is
// never latched and never written to a TGA. Instead PresentContinuousFrame
// blits the display target to the presenter and ResetContinuousFrame clears the
// per-frame state while keeping every cache. Enabled by SetContinuousMode.
void SetContinuousMode(bool on);
bool ContinuousMode();

// Blits the last display-shaped colour-writing pass to the presenter output.
// Called from the guest frame-boundary hook. No-op if no frame was recorded.
bool PresentContinuousFrame(D3D12Context& context, rex::ui::Presenter* presenter,
                            PresenterOutput& output, BlitPass& blit,
                            RenderTargetPool& render_targets);

// Clears per-frame capture state (counters, anchor, readback selection) so the
// next frame starts fresh. Caches (targets, textures, buffers, PSOs) persist,
// which is what lets the exposure adaptation converge across frames.
void ResetContinuousFrame(RenderTargetPool& render_targets);

// Guest thread, at the frame boundary: submits the frame's draws, transitions
// the display target to a shader-readable state, and publishes it for the
// command processor's swap callback (which runs on a different thread) to blit
// into the guest output. Returns false if there was no frame to present.
bool PrepareContinuousDisplay(D3D12Context& context, RenderTargetPool& render_targets);

// Command-processor thread, from the native guest-output callback: the display
// resource published by PrepareContinuousDisplay, its SRV format and size.
// Returns null if none is ready.
void* GetContinuousDisplayResource(uint32_t* srv_format, uint32_t* width, uint32_t* height);

// Offers one guest draw to the capture. Ignores draws that do not match the
// established render target configuration.
void CaptureDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                 uint32_t element_count, uint32_t start_element, int32_t base_vertex, bool indexed,
                 uint32_t draw_limit, D3D12Context& context, ShaderDatabase& shaders,
                 BufferCache& buffers, TextureCache& textures, TextureBinder& binder,
                 PipelineCache& pipelines, RenderTargetPool& render_targets,
                 uint32_t aux_stage);

// Arms the capture at a frame boundary so the passes that run BEFORE the
// main scene -- the shadow map above all -- are offered too. Without this
// the capture begins at the first main-scene draw and those passes are only
// ever counted, never rendered.
void ArmFrameCapture();

// Enables the (expensive) dump of resolve destinations.
void SetFrameCaptureDumpTargets(bool dump);

// Records a guest resolve for the capture report. The destination address is
// the one a later texture fetch samples, so this is what proves a render
// target and a texture refer to the same image.
void NoteFrameCaptureResolve(uint32_t dest_address, uint32_t width, uint32_t height,
                             bool from_depth);

// TEMP INSTRUMENTATION: per-frame tally of the guest draw entry points.
void NoteFrameCaptureGuestDraw(int kind);  // 0 = indexed, 1 = non-indexed, 2 = UP

// An inline-geometry draw (Begin/EndVertices): the vertices live in a
// command-buffer region at `address` rather than in a bound stream.
void CaptureInlineDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                       uint32_t vertex_count, uint32_t stride, uint32_t address,
                       uint32_t draw_limit, D3D12Context& context, ShaderDatabase& shaders,
                       BufferCache& buffers, TextureCache& textures, TextureBinder& binder,
                       PipelineCache& pipelines, RenderTargetPool& render_targets,
                       uint32_t aux_stage);

// TEMP INSTRUMENTATION: a resolve whose SOURCE pass was never rendered, so the
// destination could not be bridged and every later fetch of it falls back.
// Writes the resolve-miss table (passes whose source was never rendered
// natively, so their resolve was dropped) to native_gfx_diag.txt. Continuous
// mode has no other route to it: the capture report is CLOGF, a no-op there.
void DumpResolveMisses();

void NoteFrameCaptureResolveMiss(uint32_t dest_address, uint32_t dest_width,
                                 uint32_t dest_height, uint32_t src_width,
                                 uint32_t src_height, uint32_t rt_format,
                                 uint32_t ds_format);

// The sample count the pooled target for this shape will have. Anything that
// builds a RenderTargetKey outside the pool has to use it, or its lookup
// silently misses an MSAA target (see mcla_native_gfx_msaa).
uint32_t PooledSampleCountForShape(uint32_t rt_format, uint32_t ds_format, uint32_t width);

}  // namespace mcla::native_gfx
