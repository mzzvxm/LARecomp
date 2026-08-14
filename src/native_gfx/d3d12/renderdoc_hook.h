#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — programmatic RenderDoc capture
// ===========================================================================
// Triggering a capture by time or keypress cannot catch this runtime: the
// whole draw batch is recorded and submitted in well under a second, roughly
// 26 s after launch, and RenderDoc's TriggerCapture only acts on the next
// Present. A capture aimed that way lands on a later frame containing nothing
// but the presenter's blit (measured: 1 draw, 1 Present, no native work).
//
// StartFrameCapture/EndFrameCapture bracket exactly the work we record, so the
// capture contains our command lists by construction, with no timing window.
//
// The SDK does wrap this (rex::ui::RenderDocAPI), but its header pulls in
// <renderdoc_app.h>, which lives only inside the FidelityFX third-party tree
// and is not part of the installed includes. Only three entry points are
// needed, so they are declared here against RenderDoc's public and
// version-stable 1.0.0 layout instead of dragging in that dependency.
//
// Everything is a no-op when the process was not launched under RenderDoc.
// ===========================================================================

#include <cstdint>

namespace mcla::native_gfx {

// True if renderdoc.dll is present in this process and the API was obtained.
bool RenderDocAvailable();

// Brackets a capture. `device` is the ID3D12Device*. Safe to call when
// RenderDoc is absent; both return false then.
// RenderDoc holds one capture at a time, so Begin refuses to nest and End
// refuses to close what was never opened; both report false in those cases.
bool RenderDocBeginCapture(void* device);
bool RenderDocEndCapture(void* device);

// True while a capture is open, from RenderDoc's own state rather than a local
// flag -- the two diverged once already, which is how a capture stayed open for
// a whole session and made RenderDoc's capture key do nothing.
bool RenderDocIsCapturing();

}  // namespace mcla::native_gfx
