#pragma once
// ===========================================================================
// MCLA GPU renderer -- the stencil reference of a guest draw
// ===========================================================================
// The guest changes the stencil reference from draw to draw (a pass writes 1, 2, 3 ... as object ids, a lit pass tests against them, and
// a draw sets 128 to 191 one after the other). The host used to bake it into the pipeline, so every value was a pipeline of its own:
// 1 348 of the 3 280 records of the shipped starter list differ from an earlier one only by the reference. Plume can now set it on the
// command list (RenderCommandList::setStencilReference; Vulkan's dynamic state, D3D12's OMSetStencilRef) and every pipeline of
// Pipelines is made with that dynamic, so the reference is not in PipelineKey any more and the draw puts it on the list.
//
// PipelineKey::stencil_masks: read mask in bits 0-7, write mask in bits 8-15, bits 16-23 the reference (always 0 now: kept in the word
// so the cache file's record stays what it was, 104 bytes), the back face's depth fail operation in bits 24-27. A record an older build
// wrote has its draw's reference there; Pipelines reads such a file with WithoutReference, as it zeroes a static depth bias.
//
// This header is the part that needs no renderer (stencil_ref_test.cpp checks it).
#include <cstdint>

namespace mcla::gpu::stencil_ref {

inline constexpr uint32_t kReferenceShift = 16;
inline constexpr uint32_t kReferenceMask = 0xFFu << kReferenceShift;

// The masks word of a pipeline key: the guest's read mask and write mask (8 bits each) and the back face's depth fail operation.
// No reference: draws that differ only by it are the same pipeline.
constexpr uint32_t PackMasks(uint32_t read_mask, uint32_t write_mask, uint32_t back_depth_fail_op) {
  return (read_mask & 0xFFu) | ((write_mask & 0xFFu) << 8) | ((back_depth_fail_op & 0xFu) << 24);
}

// The masks word of an older build's record, as a key made now has it.
constexpr uint32_t WithoutReference(uint32_t masks) {
  return masks & ~kReferenceMask;
}

// The reference a draw needs on the command list. A draw that tests or writes the stencil takes the guest's (8 bits). One that does
// not has no use for it, but its pipeline still has the dynamic state, which Vulkan wants set before the first draw of a command
// buffer: it keeps what the list holds (0 when the list holds nothing yet), so a draw without stencil between two with the same
// reference costs no extra call.
constexpr uint32_t ReferenceFor(bool stencil_enabled, uint32_t guest_reference, bool held_known, uint32_t held) {
  if (stencil_enabled) {
    return guest_reference & 0xFFu;
  }
  return held_known ? held : 0u;
}

// What the open command list holds (Scene::ListState; forgotten with it: a new list starts unknown).
struct Held {
  bool known = false;
  uint32_t value = 0;

  // Sets *reference to what the draw needs and returns true when the list must be told (the first draw of a list, or a change).
  bool Need(bool stencil_enabled, uint32_t guest_reference, uint32_t* reference) {
    *reference = ReferenceFor(stencil_enabled, guest_reference, known, value);
    if (known && value == *reference) {
      return false;
    }
    known = true;
    value = *reference;
    return true;
  }
};

}  // namespace mcla::gpu::stencil_ref
