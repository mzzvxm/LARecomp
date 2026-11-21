#pragma once
// ===========================================================================
// LARecomp city pack -- the hash a texture is keyed by
// ===========================================================================
// The key of a texture (HitTexture::key, HitMaterial::key words 5 and 6) is the XXH3-64 of its base level's storage as the guest holds it: the number the renderer's texture dumps
// name a texture by (`<name>__<hash>.dds`), the texture mods and GB-F2's `hash:<hex>` rows. XXH3's output does not depend on the xxHash version (gpu/xxhash_inline.h). The rest of this
// library stands alone and keeps FNV-1a (bytes.h); this one function takes xxhash.h (header only, XXH_INLINE_ALL, included by guest_hash.cpp alone).
// ===========================================================================

#include <cstddef>
#include <cstdint>

namespace larecomp::citypack {

uint64_t HashGuestBytes(const void* data, size_t size);

}  // namespace larecomp::citypack
