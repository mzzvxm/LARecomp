// XXH3 for the renderer: XXH3_64bits and XXH3_64bits_withSeed, header only.
//
// rexglue 0.10's rex/hash.h pulled xxhash.h in for every includer. In 0.11 it does not (rex::HashXXH3 is a
// function of the SDK library, and there is no seeded form), so the renderer includes xxHash directly,
// as the SDK did: XXH_INLINE_ALL, nothing to link. xxhash.h comes from the folder src/gpu/CMakeLists.txt
// finds (LARECOMP_XXHASH_INCLUDE_DIR, else LAR-XenosRecomp's thirdparty/xxHash, the same copy
// src/shaderpack uses). XXH3's output does not depend on the xxHash version, so the hashes (texture
// contents, pipeline keys) are the ones 0.10 computed.
//
// XXH_X86DISPATCH stays off, as in the SDK: the hashes run on several threads, and the dispatch writes its
// function pointers on the first call without any synchronisation.

#pragma once

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif

#include <xxhash.h>
