#include "citypack/guest_hash.h"

// Header only, as the renderer's own copy (src/gpu/xxhash_inline.h): no dispatch (XXH_X86DISPATCH stays off, as in the SDK), nothing to link.
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace larecomp::citypack {

uint64_t HashGuestBytes(const void* data, size_t size) { return uint64_t(XXH3_64bits(data, size)); }

}  // namespace larecomp::citypack
