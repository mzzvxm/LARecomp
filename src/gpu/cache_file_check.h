#pragma once
// ===========================================================================
// MCLA GPU renderer -- what a pipeline cache file has to be before its counts are believed
// ===========================================================================
// The pipeline cache and the starter list are read by every game that starts, while other games may be writing them (a test batch's parallel runs share the cache
// folder). A file is written under a temporary name and replaced in one step (os/file_replace.h), so a reader never sees a half-written one; but a file that IS damaged
// (a disk that filled up, a bad copy, a file of another tool) must be left out, not crash the start. The reader (Pipelines::LoadFile) checks the magic, the version, the key
// size and the backend, reads every layout and record with bounds checks, and keeps nothing until all of it has read. The one count it used to trust is the record count
// in the header: a damaged one asked for a vector of billions of records before the first record was read. This is the check that comes first. Pure, so that a test
// (src/os/file_replace_test.cpp) needs no renderer.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu::cache_file {

// Whether `count` records of `record_bytes` each fit in the `remaining` bytes of the file that are left after the header and the layouts.
inline constexpr bool RecordsFit(uint64_t count, uint64_t record_bytes, uint64_t remaining) {
  return record_bytes != 0 && count <= remaining / record_bytes;
}

}  // namespace mcla::gpu::cache_file
