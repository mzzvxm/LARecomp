// Check of HostImage's release order (host_passes.h): the view and the framebuffer read their texture when they are destroyed, so the
// texture must go LAST, by the destructor and by move assignment. A device is not needed: stand-ins for plume's texture, view and
// framebuffer (plume's headers only) record the order they are destroyed in and whether a view or framebuffer was destroyed after its
// texture. Exit code 0 = passed.
//
//   cmake --build <build dir> --target larecomp_host_image_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_host_image_test
//
// The bug this guards (AddressSanitizer heap-use-after-free in GiField::Shutdown on Vulkan, 2026-11-25): the defaulted move assignment
// assigned the members in declaration order, so `image = HostImage{}` (83 places) freed the old texture BEFORE the old view, whose
// destructor (VulkanTextureView) reads the texture. The check "the old defaulted assignment gets it wrong" below runs a copy of that
// layout with a defaulted assignment through the same harness, so the harness is not vacuous: it does see the bug when it is there.
//
// What it proves, and what not: for HostImage as it is declared, assignment and destruction release framebuffer, view, texture, and the
// target takes over every member of the source. It does not look at the other users of plume's objects, and a member added to HostImage
// later is covered only by its move constructor (defaulted: it moves every member) -- the new member's own destruction order is the
// author's to place in the declaration.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "host_passes.h"

using mcla::gpu::HostImage;

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

// Kept OUTSIDE the stand-ins, so reading it after the stand-in died is defined (the real view reads the texture object itself).
struct Life {
  bool alive = true;
};

struct Record {
  std::string order;      // T, V, F in the order destroyed
  int dead_reads = 0;     // a view or framebuffer destroyed when its texture already was
};

struct StubTexture : plume::RenderTexture {
  StubTexture(Record* r, Life* l) : rec(r), life(l) {}
  ~StubTexture() override {
    life->alive = false;
    rec->order += 'T';
  }
  std::unique_ptr<plume::RenderTextureView> createTextureView(const plume::RenderTextureViewDesc&) const override { return nullptr; }
  void setName(const std::string&) override {}
  Record* rec;
  Life* life;
};

struct StubView : plume::RenderTextureView {
  StubView(Record* r, Life* l) : rec(r), texture(l) {}
  ~StubView() override {
    if (!texture->alive) ++rec->dead_reads;
    rec->order += 'V';
  }
  Record* rec;
  Life* texture;
};

struct StubFramebuffer : plume::RenderFramebuffer {
  StubFramebuffer(Record* r, Life* l) : rec(r), texture(l) {}
  ~StubFramebuffer() override {
    if (!texture->alive) ++rec->dead_reads;
    rec->order += 'F';
  }
  uint32_t getWidth() const override { return 0; }
  uint32_t getHeight() const override { return 0; }
  Record* rec;
  Life* texture;
};

// One image's three objects and the life flag of its texture.
struct Parts {
  Life life;
};

void Fill(HostImage& image, Record& rec, Parts& parts, uint32_t width) {
  image.texture = std::make_unique<StubTexture>(&rec, &parts.life);
  image.view = std::make_unique<StubView>(&rec, &parts.life);
  image.framebuffer = std::make_unique<StubFramebuffer>(&rec, &parts.life);
  image.slot = 7;
  image.width = width;
  image.height = width / 2;
  image.samples = 4;
  image.serial = 1000 + width;
  image.format = plume::RenderFormat::R16G16B16A16_FLOAT;
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

// The layout HostImage had before the fix, assignment defaulted: the members assigned in declaration order.
struct NaiveImage {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
};

static_assert(std::is_nothrow_move_constructible_v<HostImage>, "HostImage moves without throwing");
static_assert(std::is_nothrow_move_assignable_v<HostImage>, "HostImage move-assigns without throwing");
static_assert(!std::is_copy_constructible_v<HostImage> && !std::is_copy_assignable_v<HostImage>, "HostImage owns its objects: no copies");

void TestDestructor() {
  Record rec;
  Parts parts;
  {
    HostImage image;
    Fill(image, rec, parts, 64);
  }
  Check(rec.order == "FVT", "the destructor releases framebuffer, view, texture (got " + rec.order + ")");
  Check(rec.dead_reads == 0, "no view or framebuffer is destroyed after its texture, by the destructor");
}

void TestAssignEmpty() {
  // `image = HostImage{}`: the 83 sites.
  Record rec;
  Parts parts;
  HostImage image;
  Fill(image, rec, parts, 64);
  image = HostImage{};
  Check(rec.order == "FVT", "`image = HostImage{}` releases framebuffer, view, texture (got " + rec.order + ")");
  Check(rec.dead_reads == 0, "no view or framebuffer is destroyed after its texture, by `image = HostImage{}`");
  Check(!image.texture && !image.view && !image.framebuffer, "the image is empty after `image = HostImage{}`");
  Check(image.slot == 0xFFFFFFFFu && image.width == 0 && image.height == 0 && image.samples == 1 && image.serial == 0 &&
            image.format == plume::RenderFormat::UNKNOWN && image.layout == plume::RenderTextureLayout::UNKNOWN,
        "the scalar members are back at their defaults after `image = HostImage{}`");
}

void TestAssignFull() {
  Record rec_a, rec_b;
  Parts parts_a, parts_b;
  HostImage a, b;
  Fill(a, rec_a, parts_a, 64);
  Fill(b, rec_b, parts_b, 128);
  const plume::RenderTexture* b_texture = b.texture.get();
  const plume::RenderTextureView* b_view = b.view.get();
  const plume::RenderFramebuffer* b_framebuffer = b.framebuffer.get();
  a = std::move(b);
  Check(rec_a.order == "FVT", "a move assignment releases the target's framebuffer, view, texture (got " + rec_a.order + ")");
  Check(rec_a.dead_reads == 0, "no view or framebuffer of the target is destroyed after its texture");
  Check(rec_b.order.empty(), "the source's objects are not destroyed by being moved");
  Check(a.texture.get() == b_texture && a.view.get() == b_view && a.framebuffer.get() == b_framebuffer,
        "the target owns the source's texture, view and framebuffer");
  Check(a.slot == 7 && a.width == 128 && a.height == 64 && a.samples == 4 && a.serial == 1128 &&
            a.format == plume::RenderFormat::R16G16B16A16_FLOAT && a.layout == plume::RenderTextureLayout::SHADER_READ,
        "the target took over the source's slot, size, samples, serial, format and layout");
  Check(!b.texture && !b.view && !b.framebuffer, "the source is left without objects");
  a = HostImage{};
  Check(rec_b.order == "FVT" && rec_b.dead_reads == 0, "the objects that came over are released in order too (got " + rec_b.order + ")");
}

void TestSelfAssign() {
  Record rec;
  Parts parts;
  HostImage image;
  Fill(image, rec, parts, 64);
  HostImage& same = image;
  image = std::move(same);
  Check(rec.order.empty(), "assigning an image to itself destroys nothing");
  Check(image.texture && image.view && image.framebuffer && image.width == 64, "and leaves it as it was");
}

void TestMoveConstruct() {
  Record rec;
  Parts parts;
  HostImage a;
  Fill(a, rec, parts, 64);
  HostImage b(std::move(a));
  Check(rec.order.empty(), "move construction destroys nothing");
  Check(b.texture && b.view && b.framebuffer && b.width == 64 && b.slot == 7 && !a.texture && !a.view && !a.framebuffer,
        "the new image owns the objects and the old one is empty");
}

void TestHarnessSeesTheOldBug() {
  Record rec;
  Parts parts;
  NaiveImage image;
  image.texture = std::make_unique<StubTexture>(&rec, &parts.life);
  image.view = std::make_unique<StubView>(&rec, &parts.life);
  image.framebuffer = std::make_unique<StubFramebuffer>(&rec, &parts.life);
  image = NaiveImage{};
  Check(rec.order == "TVF", "the old defaulted assignment released texture, view, framebuffer (got " + rec.order + ")");
  Check(rec.dead_reads == 2, "and the harness sees the view and the framebuffer destroyed after their texture");
}

}  // namespace

int main() {
  TestDestructor();
  TestAssignEmpty();
  TestAssignFull();
  TestSelfAssign();
  TestMoveConstruct();
  TestHarnessSeesTheOldBug();
  if (g_failed != 0) {
    std::printf("host_image_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("host_image_test: all %d checks passed\n", g_checks);
  return 0;
}
