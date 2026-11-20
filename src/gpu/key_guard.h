#pragma once
// ===========================================================================
// Guards for the structs the renderer uses as cache keys.
//
// WHY. A key that is hashed (XXH3 over &key) or compared (memcmp) as raw bytes reads every byte of the
// object. Two things make two identical keys differ in those bytes:
//   - PADDING: bytes between members and at the tail that no member owns. A key built on the stack never
//     writes them, so they hold whatever the stack held.
//   - A MEMBER no constructor sets (no default member initializer, no `= {}`): the same, a whole field of it.
// The headlight shadow replay's pipeline key had four bytes of tail padding and was compared and hashed as
// bytes: a night street listed 98 759 pipelines for the background builder on one run, 543 441 on another and
// 230 on the next, and every draw waited for a pipeline that was never the one it was looking for
// (HEADLIGHT-FIX-2, 2026-11-20; fixed on feat/headlights 5a04fe16 with a field-wise == and hash).
//
// WHAT. The two compile-time checks below, put next to every struct that is hashed or compared as bytes
// (pipelines.h PipelineKey, textures.h TextureKey, edram.h EdramKey and FramebufferKey), so a member added the
// wrong way fails the build instead of the player's frame time:
//   static_assert(key_guard::kNoPadding<T>);                          // no padding (floats: SizeIsMemberSum)
//   static_assert(key_guard::AllMembersSet<T>());                     // every member has an initializer
// key_guard_test.cpp (given the src/gpu folder) also lists every place in src/gpu that hashes or compares a
// struct as bytes and fails when a new one appears that nobody audited.
//
// A key may instead be compared and hashed FIELD BY FIELD (EdramKey, InputLayouts::Key, PsoKey, local shadow's
// PipeKey after 5a04fe16): then padding never matters and none of this is needed.
//
// Floats in a key: -0.0 and +0.0 are equal as numbers and different as bytes, and a NaN is never equal to
// itself as a number. CanonicalZero gives the two zeros one bit pattern (the one float a key can
// legitimately hold as either); no key holds a NaN.
// ===========================================================================

#include <cstddef>
#include <type_traits>

namespace mcla::gpu::key_guard {

// True when every byte of T is the value of one of its members: no padding anywhere. The standard's own
// trait, which is false for a type with a float member (a float has two zeros): such a key states the
// bytes its members add up to instead, SizeIsMemberSum.
template <class T>
inline constexpr bool kNoPadding = std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>;

// True when T is as big as its members added up (`member_bytes`, written out at the struct): any padding, at the
// tail or between two members, makes the struct bigger than the sum.
template <class T>
consteval bool SizeIsMemberSum(std::size_t member_bytes) {
  return std::is_trivially_copyable_v<T> && sizeof(T) == member_bytes;
}

// True when a default-constructed T has every member set. It is only ever true or a compile error: the
// evaluation copies a default-constructed T inside a constant expression, and clang (like gcc and MSVC)
// refuses to read a member that was never initialized ("subobject 'x' is not initialized"). Use it as
// static_assert(AllMembersSet<T>()) after the struct; not inside the class that declares T as a nested struct
// (its default member initializers are not parsed until that class is complete: put the assert in a member
// function body there).
template <class T>
consteval bool AllMembersSet() {
  T made;
  T copy = made;
  (void)copy;
  return true;
}

// The one bit pattern a key holds for a float that is zero (-0.0f becomes +0.0f); any other value as it is.
constexpr float CanonicalZero(float value) {
  return value == 0.0f ? 0.0f : value;
}

}  // namespace mcla::gpu::key_guard
