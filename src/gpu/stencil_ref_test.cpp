// Check of the stencil reference of a guest draw (stencil_ref.h): it is not in the pipeline key, an older cache's records lose it
// when they are read, and the draw puts it on the command list exactly when the list does not hold it. A program of its own: it
// needs no renderer, no ReXGlue, no Plume and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_stencil_ref_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_stencil_ref_test [<src/gpu dir>]
// or by hand: clang++ -std=c++20 -I src/gpu src/gpu/stencil_ref_test.cpp -o stencil_ref_test
//
// What it covers (STENCIL-DYN, 2026-11-24: 1 348 of the 3 280 records of the starter list differed from an earlier one only by the
// reference; one state had a record for each of the references 128 to 191):
//   - the masks word of a key (PackMasks) has the reference in no bit, whatever the guest's: two draws that differ only by it get the
//     same key and so the same pipeline; the other fields stay where Pipelines::BuildDesc and sync_build_log.h read them;
//   - a record an older build wrote (the reference in bits 16-23) is the same key as the new one once WithoutReference has cleared it,
//     so Pipelines::LoadFile folds such records into one (the 64 references of a sequence become one record);
//   - what the list is told (stencil_ref::Held): the first draw of a list always (Vulkan wants the dynamic state set before the first
//     draw of a command buffer, with the stencil test off too), then only a change, never for a draw that does not use the stencil, and
//     again after the list state was forgotten (Scene::ResetListState);
//   - given the src/gpu folder: the places that must keep it that way still do (BuildKey packs with PackMasks and no reference,
//     BuildDesc makes every pipeline with the dynamic state and never reads a reference from the key, LoadFile clears it, and Scene sets
//     it for each of its three kinds of draw: the game's, the HDR scene's second draw and the upscaler mask's).

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

#include "stencil_ref.h"

namespace sr = mcla::gpu::stencil_ref;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

// The key before this change: the guest's reference in bits 16-23 (Pipelines::BuildKey of 430b6968).
uint32_t LegacyMasks(uint32_t read_mask, uint32_t write_mask, uint32_t reference, uint32_t back_depth_fail_op) {
  return read_mask | (write_mask << 8) | (reference << 16) | (back_depth_fail_op << 24);
}

void TestKeyHasNoReference() {
  // PackMasks takes no reference, and its word has none of bits 16-23, for any mask and operation.
  bool clean = true;
  for (uint32_t read = 0; read < 256; read += 5) {
    for (uint32_t write = 0; write < 256; write += 7) {
      for (uint32_t op = 0; op < 16; ++op) {
        clean = clean && (sr::PackMasks(read, write, op) & sr::kReferenceMask) == 0;
      }
    }
  }
  Check(clean, "no packed masks word has a bit of the reference");
  // Where the other fields are: BuildDesc reads read mask & 0xFF, write mask >> 8 & 0xFF, the back depth fail operation >> 24 & 0xF;
  // sync_build_log.h StencilWrites reads the write mask at bit 8 and the operation at 24.
  Check(sr::PackMasks(0xFF, 0x0F, 3) == 0x03000FFFu, "read mask in bits 0-7, write mask in 8-15, back depth fail operation in 24-27");
  Check(sr::PackMasks(0x00, 0xFF, 0) == 0x0000FF00u, "a write mask alone is bits 8-15");
  Check(sr::PackMasks(0x00, 0x00, 0xF) == 0x0F000000u, "an operation alone is bits 24-27");
  // Draws that differ only by the guest's reference are one key: the reference is not an input.
  const uint32_t a = sr::PackMasks(0xFF, 0xFF, 3);
  const uint32_t b = sr::PackMasks(0xFF, 0xFF, 3);
  Check(a == b, "the same masks are the same word");
  Check(sr::kReferenceMask == 0x00FF0000u && sr::kReferenceShift == 16, "the reference's place in a legacy word is bits 16-23");
}

void TestLegacyRecordsFold() {
  // The sequence of one state: references 128 to 191, six times each in the starter list.
  std::set<uint32_t> legacy, folded;
  for (uint32_t ref = 128; ref < 192; ++ref) {
    const uint32_t old_word = LegacyMasks(0xFF, 0xFF, ref, 3);
    legacy.insert(old_word);
    folded.insert(sr::WithoutReference(old_word));
  }
  Check(legacy.size() == 64, "64 references were 64 keys");
  Check(folded.size() == 1, "and are one key once the reference is cleared");
  Check(*folded.begin() == sr::PackMasks(0xFF, 0xFF, 3), "the folded key is the one a draw makes now");
  // Reference 0 and no stencil (a zero word) are unchanged; clearing twice changes nothing; the other fields are not touched.
  Check(sr::WithoutReference(0) == 0, "a word with nothing stays 0");
  Check(sr::WithoutReference(sr::WithoutReference(0x7F123456u)) == sr::WithoutReference(0x7F123456u), "clearing is idempotent");
  Check(sr::WithoutReference(0xAB123456u) == 0xAB003456u, "only bits 16-23 are cleared");
  bool all = true;
  for (uint32_t ref = 0; ref < 256; ++ref) {
    all = all && sr::WithoutReference(LegacyMasks(0x3C, 0xC3, ref, 5)) == sr::PackMasks(0x3C, 0xC3, 5);
  }
  Check(all, "every one of the 256 references clears to the key without it");
}

struct FakeList {
  int calls = 0;
  uint32_t value = 0xDEADBEEFu;  // what the command list holds

  // What Scene::Record does for a draw.
  void Draw(sr::Held& held, bool stencil, uint32_t guest_reference) {
    uint32_t want = 0;
    if (held.Need(stencil, guest_reference, &want)) {
      value = want;
      ++calls;
    }
  }
};

void TestListIsToldOnlyOnChange() {
  {
    // A new list: the first draw sets the reference, with the stencil off too (Vulkan); the next draw of the same value does not.
    FakeList list;
    sr::Held held;
    list.Draw(held, false, 77);
    Check(list.calls == 1 && list.value == 0, "a stencil-off draw first on a list sets 0, not the guest's reference");
    list.Draw(held, false, 12);
    list.Draw(held, false, 0);
    Check(list.calls == 1, "more draws without the stencil cost no call");
  }
  {
    // Stencil draws: set on a change only; a draw without the stencil in between keeps the list's value.
    FakeList list;
    sr::Held held;
    list.Draw(held, true, 5);
    Check(list.calls == 1 && list.value == 5, "the first stencil draw sets its reference");
    list.Draw(held, true, 5);
    Check(list.calls == 1, "the same reference again costs no call");
    list.Draw(held, false, 99);
    Check(list.calls == 1 && list.value == 5, "a draw without the stencil leaves the list's reference alone");
    list.Draw(held, true, 5);
    Check(list.calls == 1, "the same reference after it costs no call (not 5 -> 0 -> 5)");
    list.Draw(held, true, 6);
    Check(list.calls == 2 && list.value == 6, "another reference is set");
    for (uint32_t ref = 128; ref < 192; ++ref) {
      list.Draw(held, true, ref);
    }
    Check(list.calls == 2 + 64 && list.value == 191, "the sequence 128..191 is one call per draw, no more");
    list.Draw(held, true, 0x1A5);
    Check(list.value == 0xA5, "the guest's 9th bit is dropped: the reference is 8 bits");
  }
  {
    // A new list state (Scene::ResetListState): the list is told again, even for the value it held.
    FakeList list;
    sr::Held held;
    list.Draw(held, true, 5);
    held = sr::Held{};
    list.Draw(held, true, 5);
    Check(list.calls == 2, "a forgotten list state asks again");
    held = sr::Held{};
    list.Draw(held, false, 5);
    Check(list.calls == 3 && list.value == 0, "and a stencil-off draw first after it sets 0");
  }
  Check(sr::ReferenceFor(true, 300, true, 7) == (300 & 0xFF), "ReferenceFor: the stencil draw's reference is 8 bits");
  Check(sr::ReferenceFor(false, 300, true, 7) == 7 && sr::ReferenceFor(false, 300, false, 7) == 0, "ReferenceFor: no stencil keeps the list's, or 0");
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int Count(const std::string& text, const std::string& what) {
  int n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
    ++n;
  }
  return n;
}

void TestSources(const std::filesystem::path& dir) {
  const std::string pipelines = ReadFile(dir / "pipelines.cpp");
  const std::string scene = ReadFile(dir / "scene.cpp");
  if (pipelines.empty() || scene.empty()) {
    std::printf("the sources of %s could not be read: the source checks are skipped\n", dir.string().c_str());
    return;
  }
  Check(Count(pipelines, "key.stencil_masks = stencil_ref::PackMasks(") == 1, "BuildKey packs the masks with PackMasks");
  Check(Count(pipelines, "stencilref) << 16") == 0 && Count(pipelines, "stencilref << 16") == 0, "BuildKey puts no reference in the key");
  Check(Count(pipelines, "desc.dynamicStencilReferenceEnabled = true;") == 1, "BuildDesc makes every pipeline with the dynamic stencil reference");
  Check(Count(pipelines, "desc.stencilReference") == 0, "BuildDesc reads no reference from the key");
  Check(Count(pipelines, "stencil_ref::WithoutReference(r.key.stencil_masks)") == 1, "LoadFile clears the reference of an older record");
  // The draws that bind a pipeline of Pipelines: the game's (Record, through the list's held value), the mirror view's copy of it (RecordMirrorDraw, the same held value) and the two second draws.
  Check(Count(scene, "bound_.stencil_reference.Need(") == 2, "Record and the mirror view's draw tell the list the reference through the held value");
  Check(Count(scene, "list->setStencilReference(") == 4, "the game's draw, the mirror view's draw, the HDR second draw and the mask draw each set the reference");
  Check(Count(scene, "list->setPipeline(") == 4, "Scene binds a pipeline of Pipelines in four places (a fifth needs the reference too)");
}

}  // namespace

int main(int argc, char** argv) {
  TestKeyHasNoReference();
  TestLegacyRecordsFold();
  TestListIsToldOnlyOnChange();
  if (argc > 1) {
    TestSources(argv[1]);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  std::printf(g_failed == 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
  return g_failed == 0 ? 0 : 1;
}
