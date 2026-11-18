// MCLA GPU renderer -- the car dump (a debug prototype). See car_dump.h.

#include "car_dump.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "device.h"
#include "frame.h"
#include "guest_memory.h"
#include "host_passes.h"
#include "native_gfx/vertex_declaration.h"
#include "vehicle_motion.h"

REXCVAR_DEFINE_UINT64(
    mcla_gpu_car_dump, 0, "MCLA/GPU",
    "Debug: takes the triangles of every car the game draws in ONE frame out of the game's memory, puts them in world space with "
    "each draw's own matrices and writes them to car_dump_<frame>.obj next to the executable (one group per draw, for the car "
    "chosen by mcla_gpu_car_dump_id), with a census of the car draws (how many draws, vertices and triangles each car has and "
    "which shaders draw it) in the log under [car dump]. 0 (the default) is off. A number N dumps the first frame at or after the "
    "game's frame N that has a car in it. With the car self-reflections on (mcla_gpu_car_selfrefl_test=1) it also checks the "
    "triangles against the picture of the cars the self-reflections keep, and logs how much of the car they cover and how close "
    "their distances are. Park the car, switch the upscaler off and use the game's own view (not the garage or the pause menu). A "
    "new value dumps again. It costs nothing while it is off and one slow frame when it dumps.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_car_dump_id, 0, "MCLA/GPU",
    "Debug: which car mcla_gpu_car_dump writes to its OBJ file and checks first, by the number the game stamps into the stencil "
    "for it (1 to 23, handed out in drawing order each frame: the player's car is 1 when it is drawn first). 0 (the default) "
    "takes the car with the most draws in the frame. Live.")
    .debug_only()
    .range(0, 23)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

namespace {

constexpr const char* kPrefix = "[car dump]";
// How far a draw's pixels may be from the image's distance, relative, to count as the same surface (the half float image is
// within 0.05% of the distance; 1% is 5 cm at 5 m).
constexpr double kFitTolerance = 0.01;
// A draw with more indices than this is not a car's.
constexpr uint32_t kMaxIndices = 8u * 1024u * 1024u;
// D3DDECLUSAGE_BLENDINDICES (native_gfx/vertex_declaration.h).
constexpr uint32_t kUsageBlendIndices = 2;
constexpr uint32_t kUsagePosition = 0;

// The key of one group of the census.
using GroupKey = std::tuple<uint32_t, uint64_t, uint64_t>;  // ID, vertex shader, pixel shader

std::string PrimitiveText(uint32_t mask) {
  std::string text;
  for (uint32_t p = 0; p < 32; ++p) {
    if (mask & (1u << p)) {
      text += std::format("{}{}", text.empty() ? "" : "/", PrimitiveName(p));
    }
  }
  return text.empty() ? "none" : text;
}

std::string FormatText(uint32_t mask) {
  std::string text;
  for (uint32_t f = 0; f < 8; ++f) {
    if (mask & (1u << f)) {
      text += std::format("{}{}", text.empty() ? "" : "/", car_dump::PositionFormatName(car_dump::PositionFormat(f)));
    }
  }
  return text.empty() ? "none" : text;
}

double Distance3(const double a[3], const double b[3]) {
  const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

double Relative(double a, double b) { return std::fabs(a - b) / std::max(1.0, std::fabs(b)); }

}  // namespace

const char* CarDump::ProblemName(Problem problem) {
  switch (problem) {
    case Problem::kNone: return "none";
    case Problem::kComputedFetch: return "computed vertex fetch";
    case Problem::kPrimitive: return "primitive not drawn as triangles";
    case Problem::kNotTriangles: return "not triangles";
    case Problem::kNoPosition: return "no POSITION0";
    case Problem::kPositionFormat: return "position format";
    case Problem::kFetch: return "no vertex fetch";
    case Problem::kUnreadable: return "unreadable";
    case Problem::kNoTriangles: return "no triangles";
  }
  return "?";
}

CarDump::~CarDump() = default;

void CarDump::Initialize(Device& device, FrameRing& frames, GuestMemory& memory) {
  device_ = &device;
  frames_ = &frames;
  memory_ = &memory;
  Refresh();
}

void CarDump::Shutdown() {
  readback_.reset();
  readback_bytes_ = 0;
  Release();
  wants_ = false;
  device_ = nullptr;
  frames_ = nullptr;
  memory_ = nullptr;
}

void CarDump::Refresh() {
  const uint64_t target = REXCVAR_GET(mcla_gpu_car_dump);
  if (target != target_) {
    target_ = target;
    done_ = false;  // a new request dumps again
  }
  wants_ = memory_ != nullptr && target != 0 && !done_ && frame_ >= target;
}

void CarDump::Release() {
  draws_.clear();
  draws_.shrink_to_fit();
  history_ = History{};
  finished_ = false;
  camera_valid_ = false;
}

// ---------------------------------------------------------------------------
// the frame's draws
// ---------------------------------------------------------------------------

void CarDump::Open() {
  Release();
  open_ = true;
  dump_frame_ = frame_;
  chosen_ = 0;
  REXLOG_INFO("{} frame {}: the first draw that writes a vehicle ID comes after {} main-view draws that write none; dumping this "
              "frame",
              kPrefix, dump_frame_, world_draws_);
}

void CarDump::OnMainDraw(const MainDraw& draw) {
  const uint32_t mask = draw.stencil_ids & ~1u & ((1u << temporal::kVehicleIds) - 1u);
  if (mask == 0) {
    ++world_draws_;
    return;
  }
  if (!open_) {
    Open();
  }
  draws_.emplace_back();
  Draw& out = draws_.back();
  const DrawRecord& r = draw.view->record();
  out.id_mask = mask;
  out.id = uint32_t(std::countr_zero(mask));
  out.vs = r.vs_identity;
  out.ps = r.ps_identity;
  out.world_before = world_draws_;
  Expand(draw, &out);
}

void CarDump::Expand(const MainDraw& d, Draw* out) {
  const DrawView& view = *d.view;
  const DrawRecord& r = view.record();
  out->primitive = r.primitive;
  out->kind = r.kind;
  out->count = r.count;
  out->matrices = car_dump::DecodeCarMatrices(d.banks->vs.data());
  out->damage = (d.state->bool_words[0] & 1u) != 0;
  if (d.shaders && d.vs) {
    d.shaders->GetVertexElements(*d.vs, elements_);
    for (const ShaderStore::VertexElement& e : elements_) {
      out->skinned |= e.usage == kUsageBlendIndices;
    }
  }

  // What the renderer draws: lists and strips as they are, quads and fans as lists of triangles (topology.h).
  const PrimitiveConversion conversion = d.primitive.conversion;
  const plume::RenderPrimitiveTopology topology = d.primitive.topology;
  if ((r.flags & kDrawVsComputedFetchIndex) != 0 || (d.vs && (d.vs->flags & ShaderStore::kComputedFetchIndex) != 0)) {
    out->problem = Problem::kComputedFetch;
    return;
  }
  if (conversion == PrimitiveConversion::kUnsupported || conversion == PrimitiveConversion::kRectList) {
    out->problem = Problem::kPrimitive;
    return;
  }
  const bool strip = topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  if (topology != plume::RenderPrimitiveTopology::TRIANGLE_LIST && !strip) {
    out->problem = Problem::kNotTriangles;
    return;
  }
  out->strip = strip;
  if (r.count > kMaxIndices) {
    out->problem = Problem::kUnreadable;
    return;
  }

  // The POSITION element of the declaration: the first one with usage 0 and index 0, as the input layout takes it.
  nfx::DeclarationElement position;
  bool found = false;
  for (uint32_t i = 0; i < r.decl_count && i < guest::kMaxDeclElements; ++i) {
    const nfx::DeclarationElement e = nfx::ReadDeclarationElement(view.decl_records(), i);
    if (e.stream == 0xFF) {
      break;
    }
    if (e.usage == kUsagePosition && e.usage_index == 0) {
      position = e;
      found = true;
      break;
    }
  }
  if (!found) {
    out->problem = Problem::kNoPosition;
    return;
  }
  out->format = car_dump::PositionFormatOf(position.type);
  out->stream = position.stream;
  out->offset = position.offset;

  // Its stream: a vertex fetch constant (the address, the size in bytes, the endian) and the device's stride, or the inline vertices.
  const bool inline_draw = r.kind == DrawKind::kInline;
  uint32_t address = 0;
  uint32_t size = 0;
  uint32_t stride = 0;
  const uint8_t* inline_base = nullptr;
  if (inline_draw) {
    stride = r.inline_stride;
    size = r.inline_bytes;
    inline_base = view.inline_data();
  } else {
    const uint32_t slot = nfx::FetchSlotForStream(position.stream);
    if (slot >= guest::kFetchGroupCount * 3 || position.stream >= guest::kStreamCount) {
      out->problem = Problem::kFetch;
      return;
    }
    uint32_t d0 = 0, d1 = 0;
    view.VertexFetch(slot, &d0, &d1);
    if ((d0 & 3u) != 3u || (d1 & 3u) != car_dump::kEndian8in32) {  // a vertex fetch, endian 8in32 (geometry.cpp StreamSource)
      out->problem = Problem::kFetch;
      return;
    }
    address = d0 & 0xFFFFFFFCu;
    size = d1 & 0x03FFFFFCu;
    stride = uint32_t(r.stream_strides[position.stream]) * 4u;
  }
  out->stride = stride;
  const uint32_t bytes = car_dump::PositionBytes(out->format);
  if (bytes == 0 || stride == 0 || position.offset + bytes > stride) {
    out->problem = Problem::kPositionFormat;
    return;
  }
  const uint32_t available = size / stride;

  // The indices, expanded to a triangle list by the renderer's own functions (geometry.cpp PrepareDraw does the same).
  const uint32_t count = r.count;
  int64_t vertex_base = 0;
  bool reset = false;
  uint32_t reset_index = 0;
  if (r.kind == DrawKind::kIndexed) {
    const bool index32 = (r.index_dword0 & 0x80000000u) != 0;
    const uint32_t endian = (r.index_dword0 >> 29) & 3u;
    const uint32_t index_size = index32 ? 4u : 2u;
    const uint32_t index_address = GuestMemory::ToPhysical(r.index_address) + r.start * index_size;
    const uint8_t* raw = memory_->Read(index_address, count * index_size);
    if (!raw) {
      out->problem = Problem::kUnreadable;
      return;
    }
    out->index32 = index32;
    car_dump::DecodeIndices(raw, count, index32, endian, &guest_indices_);
    vertex_base = r.base_vertex;
    reset = d.state->sc_mode.multi_prim_ib_ena != 0;
    reset_index = d.state->reset_index & (index32 ? 0xFFFFFFFFu : 0xFFFFu);
  } else {
    guest_indices_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      guest_indices_[i] = i;
    }
    vertex_base = r.kind == DrawKind::kAuto ? int64_t(r.start) : 0;
  }
  list_.clear();
  if (conversion == PrimitiveConversion::kNone) {
    if (strip) {
      list_.resize(size_t(count) * 3);
      const uint32_t n = StripToList(true, guest_indices_.data(), count, reset, reset_index, list_.data());
      list_.resize(n);
    } else {
      list_ = guest_indices_;
    }
  } else {
    const uint32_t n = ConvertedIndexCount(conversion, count);
    list_.resize(n);
    if (n != 0) {
      ConvertIndices(conversion, guest_indices_.data(), count, list_.data());
    }
  }
  car_dump::TriangleSet set;
  car_dump::CollectTriangles(list_.data(), uint32_t(list_.size()), vertex_base, available, &set);
  out->degenerate = set.degenerate;
  out->out_of_range = set.out_of_range;
  if (set.triangles() == 0) {
    out->problem = Problem::kNoTriangles;
    return;
  }

  // The positions of the vertices the triangles name (one range of the stream).
  const uint32_t first = set.first_vertex;
  const uint32_t span = set.span();
  const uint8_t* base = nullptr;
  if (inline_draw) {
    base = inline_base + size_t(first) * stride;
  } else {
    base = memory_->Read(address + first * stride, span * stride);
  }
  if (!base) {
    out->problem = Problem::kUnreadable;
    return;
  }
  out->first_vertex = first;
  out->local.resize(size_t(span) * 3);
  for (uint32_t v = 0; v < span; ++v) {
    car_dump::DecodePosition(base + size_t(v) * stride + position.offset, out->format, &out->local[size_t(v) * 3]);
  }
  out->tri = std::move(set.tri);
  std::vector<uint8_t> seen(span, 0);
  for (uint32_t& n : out->tri) {
    n -= first;
    seen[n] = 1;
  }
  out->triangles = uint32_t(out->tri.size() / 3);
  out->vertices = uint32_t(std::count(seen.begin(), seen.end(), uint8_t(1)));
}

void CarDump::OnAnchor(const ConstantBanks::Banks& banks) {
  if (!open_) {
    return;
  }
  // The frame's camera, as car_selfrefl.cpp OnAnchor reads it (the frame size in c62, the view, view-projection and view inverse in
  // c4-c15 of the anchor's pixel bank).
  const uint8_t* ps = banks.ps.data();
  const float width = car_dump::BankFloat(ps, 62, 0);
  const float height = car_dump::BankFloat(ps, 62, 1);
  camera_valid_ = false;
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    return;
  }
  camera_valid_ = temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), dump_frame_, &camera_) ==
                  temporal::Reject::kNone;
}

// ---------------------------------------------------------------------------
// the history image, read back
// ---------------------------------------------------------------------------

void CarDump::OnHistoryImage(plume::RenderCommandList* list, HostImage& image, uint32_t divisor, const temporal::Mat4& lens) {
  if (!open_ || history_.requested || !list || !device_ || !frames_ || !image.texture) {
    return;
  }
  if (image.format != plume::RenderFormat::R16G16B16A16_FLOAT || image.width == 0 || image.height == 0) {
    return;
  }
  const uint32_t width = image.width;
  const uint32_t height = image.height;
  const uint32_t row_bytes = (width * 8 + 255) / 256 * 256;  // rows 256-aligned
  const uint64_t bytes = uint64_t(row_bytes) * height;
  if (!readback_ || readback_bytes_ < bytes) {
    readback_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(bytes), "car dump readback");
    readback_bytes_ = readback_ ? bytes : 0;
    if (!readback_) {
      REXLOG_ERROR("{} the history image's readback buffer could not be made: no check", kPrefix);
      return;
    }
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::COPY_SOURCE));
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(readback_.get(), plume::RenderFormat::R16G16B16A16_FLOAT, width, height, 1,
                                                        row_bytes / 8, 0),
      plume::RenderTextureCopyLocation::Subresource(image.texture.get()));
  // The next frame's paint draws read the image as a texture: back to where the history pass left it.
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
  history_.requested = true;
  history_.width = width;
  history_.height = height;
  history_.divisor = divisor;
  history_.lens = lens;
  const uint64_t frame = dump_frame_;
  frames_->RetireAction([this, frame, width, height, row_bytes] { ProcessHistory(frame, width, height, row_bytes); });
}

void CarDump::ProcessHistory(uint64_t frame, uint32_t width, uint32_t height, uint32_t row_bytes) {
  if (frame != dump_frame_ || !history_.requested || history_.ready || !readback_) {
    return;
  }
  const void* mapped = readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} the history image of frame {} could not be mapped: no check", kPrefix, frame);
    return;
  }
  const uint8_t* source = static_cast<const uint8_t*>(mapped);
  history_.distance.assign(size_t(width) * height, 0.0f);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = source + size_t(y) * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      uint16_t half;
      std::memcpy(&half, row + size_t(x) * 8 + 6, 2);  // the alpha: the vehicle's view distance
      const float value = car_dump::HalfToFloat(half);
      history_.distance[size_t(y) * width + x] = (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
    }
  }
  readback_->unmap();
  history_.ready = true;
  TryCheck();
}

void CarDump::TryCheck() {
  if (!finished_ || !history_.ready) {
    return;
  }
  RunCheck();
  Release();
}

// ---------------------------------------------------------------------------
// the end of the frame: the census and the OBJ
// ---------------------------------------------------------------------------

void CarDump::OnPresent() {
  if (open_) {
    Finish();
    open_ = false;
    done_ = true;
  }
  world_draws_ = 0;
  ++frame_;
  Refresh();
}

uint32_t CarDump::ChosenId() const {
  const int32_t wanted = REXCVAR_GET(mcla_gpu_car_dump_id);
  uint32_t draws[temporal::kVehicleIds] = {};
  for (const Draw& d : draws_) {
    ++draws[d.id];
  }
  if (wanted > 0 && uint32_t(wanted) < temporal::kVehicleIds && draws[wanted] != 0) {
    return uint32_t(wanted);
  }
  if (wanted > 0) {
    REXLOG_WARN("{} mcla_gpu_car_dump_id = {} has no draw in this frame: taking the ID with the most draws", kPrefix, wanted);
  }
  uint32_t best = 0;
  for (uint32_t id = 1; id < temporal::kVehicleIds; ++id) {
    if (draws[id] > draws[best]) {
      best = id;
    }
  }
  return best;
}

void CarDump::Finish() {
  chosen_ = ChosenId();
  Census(chosen_);
  WriteObj(chosen_);
  finished_ = true;
  if (!history_.requested) {
    REXLOG_WARN("{} frame {}: the car history image was not made in this frame, so there is no check against it (the car "
                "self-reflections must be on: --mcla_gpu_car_selfrefl_test=1; they run in the game's view with a camera for the "
                "frame before and a paint draw through the variant in this frame)",
                kPrefix, dump_frame_);
    Release();
  } else {
    TryCheck();  // (the image may have been read before the present)
  }
}

void CarDump::Census(uint32_t chosen) {
  struct Group {
    uint32_t draws = 0;
    uint32_t primitives = 0;   // bit per xenos primitive type seen
    uint32_t formats = 0;      // bit per PositionFormat seen
    uint32_t indexed = 0, index16 = 0, index32 = 0, autos = 0, inlines = 0;
    uint32_t stride = 0, offset = 0, stream = 0;
    bool stride_varies = false;
    uint32_t bone_min = 0xFFFFFFFFu, bone_max = 0, no_bone = 0;
    uint32_t damage = 0, skinned = 0;
    uint32_t before = 0, after = 0;
    uint32_t triangles = 0, vertices = 0, degenerate = 0, out_of_range = 0;
    uint32_t problems[16] = {};
    bool have_t = false;
    double t[3] = {};
    double dist_min = 1e300, dist_max = -1.0;
    uint32_t wvp_ok = 0, wvp_checked = 0;
    double wvp_worst = 0.0;
  };
  struct IdTotals {
    uint32_t draws = 0, vertices = 0, triangles = 0, degenerate = 0, out_of_range = 0, no_triangles = 0;
    double nearest = 1e300;
  };
  std::map<GroupKey, Group> groups;
  IdTotals ids[temporal::kVehicleIds];
  uint32_t before = 0, after = 0, multi = 0;
  for (const Draw& d : draws_) {
    Group& g = groups[GroupKey{d.id, d.vs, d.ps}];
    IdTotals& total = ids[d.id];
    ++g.draws;
    ++total.draws;
    before += d.world_before == 0;
    after += d.world_before != 0;
    multi += (d.id_mask & (d.id_mask - 1)) != 0;
    g.primitives |= 1u << (d.primitive & 31u);
    g.formats |= 1u << uint32_t(d.format);
    g.indexed += d.kind == DrawKind::kIndexed;
    g.index16 += d.kind == DrawKind::kIndexed && !d.index32;
    g.index32 += d.kind == DrawKind::kIndexed && d.index32;
    g.autos += d.kind == DrawKind::kAuto;
    g.inlines += d.kind == DrawKind::kInline;
    if (g.draws == 1) {
      g.stride = d.stride;
      g.offset = d.offset;
      g.stream = d.stream;
    } else if (g.stride != d.stride || g.offset != d.offset || g.stream != d.stream) {
      g.stride_varies = true;
    }
    if (d.matrices.bone_valid) {
      g.bone_min = std::min(g.bone_min, d.matrices.bone);
      g.bone_max = std::max(g.bone_max, d.matrices.bone);
    } else {
      ++g.no_bone;
    }
    g.damage += d.damage;
    g.skinned += d.skinned;
    (d.world_before == 0 ? g.before : g.after) += 1;
    g.triangles += d.triangles;
    g.vertices += d.vertices;
    g.degenerate += d.degenerate;
    g.out_of_range += d.out_of_range;
    ++g.problems[uint32_t(d.problem)];
    total.vertices += d.vertices;
    total.triangles += d.triangles;
    total.degenerate += d.degenerate;
    total.out_of_range += d.out_of_range;
    total.no_triangles += d.problem != Problem::kNone;
    // gWorld's translation (c3), its distance to the camera and whether gWorld times the camera's view-projection is the draw's
    // gWorldViewProj.
    const double t[3] = {d.matrices.world.m[3][0], d.matrices.world.m[3][1], d.matrices.world.m[3][2]};
    if (!g.have_t) {
      g.have_t = true;
      std::memcpy(g.t, t, sizeof(t));
    }
    if (camera_valid_ && d.matrices.finite) {
      const double dist = Distance3(t, camera_.position);
      g.dist_min = std::min(g.dist_min, dist);
      g.dist_max = std::max(g.dist_max, dist);
      total.nearest = std::min(total.nearest, dist);
      const double error = car_dump::WvpError(d.matrices.world, camera_.view_projection, d.matrices.wvp);
      ++g.wvp_checked;
      g.wvp_ok += error <= car_dump::kWvpTolerance;
      g.wvp_worst = std::max(g.wvp_worst, error);
    }
  }
  REXLOG_INFO("{} frame {}: {} main-view draws write a vehicle ID ({} before the first world draw, {} after it; {} write more "
              "than one ID), the frame's {} main-view draws before the first vehicle draw wrote none; camera {}",
              kPrefix, dump_frame_, draws_.size(), before, after, multi, draws_.empty() ? 0u : draws_.front().world_before,
              camera_valid_ ? std::format("at ({:.2f}, {:.2f}, {:.2f})", camera_.position[0], camera_.position[1], camera_.position[2])
                            : std::string("not read (the anchor draw was not seen, or its constants were not a camera): no distances, no gWorld * VP check"));
  for (uint32_t id = 1; id < temporal::kVehicleIds; ++id) {
    const IdTotals& total = ids[id];
    if (total.draws == 0) {
      continue;
    }
    REXLOG_INFO("{} id {}{}: {} draws, {} vertices, {} triangles (+ {} degenerate, {} out of range), {} draws with no triangles, "
                "nearest gWorld {}",
                kPrefix, id, id == chosen ? " (chosen)" : "", total.draws, total.vertices, total.triangles, total.degenerate,
                total.out_of_range, total.no_triangles,
                total.nearest < 1e299 ? std::format("{:.2f} m from the camera", total.nearest) : std::string("n/a"));
  }
  for (const auto& [key, g] : groups) {
    std::string bones;
    if (g.bone_max >= g.bone_min && g.bone_min != 0xFFFFFFFFu) {
      bones = g.bone_min == g.bone_max ? std::format("bone {}", g.bone_min) : std::format("bones {}-{}", g.bone_min, g.bone_max);
    } else {
      bones = "no valid bone";
    }
    if (g.no_bone != 0 && g.no_bone != g.draws) {
      bones += std::format(" ({} draws with an invalid c204.x)", g.no_bone);
    } else if (g.no_bone == g.draws && g.draws != 0 && g.bone_min != 0xFFFFFFFFu) {
      bones += " (invalid)";
    }
    std::string problems;
    for (uint32_t p = 1; p < 16; ++p) {
      if (g.problems[p]) {
        problems += std::format("{}{} x {}", problems.empty() ? "" : ", ", ProblemName(Problem(p)), g.problems[p]);
      }
    }
    std::string wvp = "gWorld * VP vs gWorldViewProj: no camera";
    std::string where = std::format("gWorld t = ({:.2f}, {:.2f}, {:.2f})", g.t[0], g.t[1], g.t[2]);
    if (g.wvp_checked != 0) {
      wvp = std::format("gWorld * VP == gWorldViewProj in {}/{} draws (worst relative error {:.1e})", g.wvp_ok, g.wvp_checked,
                        g.wvp_worst);
      where += std::format(", {:.2f}-{:.2f} m from the camera", g.dist_min, g.dist_max);
    }
    REXLOG_INFO("{} id {} vs {:016x} ps {:016x}: {} draws ({} indexed: {} 16-bit, {} 32-bit; {} auto; {} inline), {}, position {} at "
                "offset {} of stream {}, stride {}{}, {}, bool b0 (damage) on in {}, skinned (BLENDINDICES) in {}, {} before / {} after "
                "the first world draw, {} triangles over {} vertices (+ {} degenerate, {} out of range), {}; {}; problems: {}",
                kPrefix, std::get<0>(key), std::get<1>(key), std::get<2>(key), g.draws, g.indexed, g.index16, g.index32, g.autos,
                g.inlines, PrimitiveText(g.primitives), FormatText(g.formats), g.offset, g.stream, g.stride,
                g.stride_varies ? " (varies)" : "", bones, g.damage, g.skinned, g.before, g.after, g.triangles, g.vertices,
                g.degenerate, g.out_of_range, where, wvp, problems.empty() ? "none" : problems);
  }
}

void CarDump::WriteObj(uint32_t chosen) const {
  if (chosen == 0) {
    REXLOG_WARN("{} no car to write", kPrefix);
    return;
  }
  const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / std::format("car_dump_{}.obj", dump_frame_);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    REXLOG_ERROR("{} could not open {}", kPrefix, path.string());
    return;
  }
  std::string buffer;
  const auto flush = [&] {
    file.write(buffer.data(), std::streamsize(buffer.size()));
    buffer.clear();
  };
  std::format_to(std::back_inserter(buffer),
                 "# car dump of guest frame {}: stencil ID {}, world space, one group per draw (id_vsHash_psHash_bone). Every draw is "
                 "taken through its bone of the palette and then gWorld (the tyres' shader has no palette: the log's fit lines say which "
                 "recipe a shader follows); a skinned draw (BLENDINDICES) and the damage branch are not done.\n",
                 dump_frame_, chosen);
  uint64_t vertex_base = 0;
  uint64_t vertices = 0;
  uint64_t triangles = 0;
  uint32_t groups = 0;
  uint32_t ordinal = 0;
  for (const Draw& d : draws_) {
    ++ordinal;
    if (d.id != chosen || d.triangles == 0) {
      continue;
    }
    std::format_to(std::back_inserter(buffer), "# draw {}{}{}\n", ordinal - 1, d.skinned ? " skinned" : "", d.damage ? " damage" : "");
    std::format_to(std::back_inserter(buffer), "g {}_{:016x}_{:016x}_{}\n", d.id, d.vs, d.ps,
                   d.matrices.bone_valid ? std::to_string(d.matrices.bone) : std::string("x"));
    const size_t count = d.local.size() / 3;
    for (size_t v = 0; v < count; ++v) {
      const double p[3] = {double(d.local[v * 3]), double(d.local[v * 3 + 1]), double(d.local[v * 3 + 2])};
      double w[4];
      car_dump::TransformPoint(d.matrices.model, p, w);
      const double inverse = std::fabs(w[3]) > 1e-12 ? 1.0 / w[3] : 1.0;
      std::format_to(std::back_inserter(buffer), "v {:.5f} {:.5f} {:.5f}\n", w[0] * inverse, w[1] * inverse, w[2] * inverse);
      if (buffer.size() > (1u << 20)) {
        flush();
      }
    }
    for (size_t t = 0; t + 2 < d.tri.size(); t += 3) {
      std::format_to(std::back_inserter(buffer), "f {} {} {}\n", vertex_base + d.tri[t] + 1, vertex_base + d.tri[t + 1] + 1,
                     vertex_base + d.tri[t + 2] + 1);
      if (buffer.size() > (1u << 20)) {
        flush();
      }
    }
    vertex_base += count;
    vertices += count;
    triangles += d.triangles;
    ++groups;
  }
  flush();
  file.close();
  REXLOG_INFO("{} wrote {}: stencil ID {}, {} draws as groups, {} vertices, {} triangles", kPrefix, path.string(), chosen, groups,
              vertices, triangles);
}

// ---------------------------------------------------------------------------
// the check against the history image
// ---------------------------------------------------------------------------

void CarDump::RunCheck() {
  const std::vector<float>& image = history_.distance;
  if (!camera_valid_) {
    REXLOG_WARN("{} check of frame {}: no camera for the frame (the anchor draw's constants), so the triangles cannot be projected",
                kPrefix, dump_frame_);
    return;
  }
  const uint32_t width = history_.width;
  const uint32_t height = history_.height;
  if (image.size() != size_t(width) * height || width == 0 || height == 0) {
    return;
  }
  uint64_t vehicle_pixels = 0;
  for (float v : image) {
    vehicle_pixels += v > 0.0f;
  }
  // The distance of the image is the lens' linearisation of the stored depth with the PREVIOUS frame's projection; the projection's
  // clip w is that distance only for P23 = -1 and P33 = 0.
  const temporal::Mat4& p = camera_.projection;
  const double lens_difference = std::max({Relative(history_.lens.m[2][2], p.m[2][2]), Relative(history_.lens.m[2][3], p.m[2][3]),
                              Relative(history_.lens.m[3][2], p.m[3][2]), Relative(history_.lens.m[3][3], p.m[3][3])});
  REXLOG_INFO("{} check of frame {}: history image {}x{} (the render size over {}), {} vehicle pixels ({:.2f}% of the image); the "
              "image was linearised with a lens {:.2f}% different from this frame's (largest relative difference of P22, P23, P32, P33); "
              "this frame's P23 = {:.4f}, P33 = {:.4f} (w is the view distance for -1 and 0)",
              kPrefix, dump_frame_, width, height, history_.divisor, vehicle_pixels,
              100.0 * double(vehicle_pixels) / double(size_t(width) * height), 100.0 * lens_difference, p.m[2][3], p.m[3][3]);
  if (vehicle_pixels == 0) {
    REXLOG_WARN("{} check of frame {}: the image has no vehicle pixel (no car in the view, or the history is for another frame): "
                "nothing to compare", kPrefix, dump_frame_);
    return;
  }

  car_dump::Image im;
  im.width = width;
  im.height = height;
  im.sample = car_dump::HistorySample(history_.divisor);
  car_dump::DepthGrid all, chosen;
  all.Reset(width, height);
  chosen.Reset(width, height);
  std::map<GroupKey, std::pair<car_dump::FitCount, car_dump::FitCount>> fits;  // palette recipe, gWorld alone
  uint64_t triangles = 0, off_image = 0, behind_camera = 0, chosen_triangles = 0, chosen_off = 0, chosen_behind = 0;
  std::vector<car_dump::Vec4> clip_a, clip_b;
  for (const Draw& d : draws_) {
    if (d.triangles == 0 || !d.matrices.finite) {
      continue;
    }
    const temporal::Mat4 to_clip_a = temporal::Mul(d.matrices.model, camera_.view_projection);
    const temporal::Mat4 to_clip_b = temporal::Mul(d.matrices.world, camera_.view_projection);
    const size_t vertex_count = d.local.size() / 3;
    clip_a.resize(vertex_count);
    clip_b.resize(vertex_count);
    for (size_t v = 0; v < vertex_count; ++v) {
      const double position[3] = {double(d.local[v * 3]), double(d.local[v * 3 + 1]), double(d.local[v * 3 + 2])};
      clip_a[v] = car_dump::Project(to_clip_a, position);
      clip_b[v] = car_dump::Project(to_clip_b, position);
    }
    auto& fit = fits[GroupKey{d.id, d.vs, d.ps}];
    const bool is_chosen = d.id == chosen_;
    for (size_t t = 0; t + 2 < d.tri.size(); t += 3) {
      const car_dump::Vec4 a[3] = {clip_a[d.tri[t]], clip_a[d.tri[t + 1]], clip_a[d.tri[t + 2]]};
      const car_dump::Vec4 b[3] = {clip_b[d.tri[t]], clip_b[d.tri[t + 1]], clip_b[d.tri[t + 2]]};
      ++triangles;
      const bool off = car_dump::OffImage(a, im);
      const bool behind = car_dump::BehindCamera(a);
      off_image += off;
      behind_camera += behind;
      if (is_chosen) {
        ++chosen_triangles;
        chosen_off += off;
        chosen_behind += behind;
      }
      car_dump::RasterizeClip(im, a, [&](uint32_t x, uint32_t y, double w) {
        fit.first.Add(w, image[size_t(y) * width + x], kFitTolerance);
        all.Splat(x, y, w);
        if (is_chosen) {
          chosen.Splat(x, y, w);
        }
      });
      car_dump::RasterizeClip(im, b, [&](uint32_t x, uint32_t y, double w) {
        fit.second.Add(w, image[size_t(y) * width + x], kFitTolerance);
      });
    }
  }
  const auto report = [&](const char* what, const car_dump::DepthGrid& grid, uint64_t tris, uint64_t off, uint64_t behind) {
    const car_dump::CheckStats s = car_dump::CompareToDistance(image, grid, kFitTolerance);
    REXLOG_INFO("{} check ({}): {:.2f}% of the {} vehicle pixels are covered by a triangle; depth error over the covered pixels: "
                "mean {:.3f}% / {:.4f} m, median {:.3f}%, 95th percentile {:.3f}% / {:.4f} m, worst {:.1f}%; {:.1f}% within 1%, {:.1f}% "
                "within 2%; the triangle is nearer than the image by more than {:.0f}% on {:.2f}% of them and farther on {:.2f}%; "
                "{} pixels have a triangle and no vehicle (hidden behind the world, or not the car); triangles: {} projected, {} with "
                "all three vertices outside the image ({} of them behind the camera)",
                kPrefix, what, 100.0 * s.coverage, s.vehicle, 100.0 * s.mean_rel, s.mean_abs, 100.0 * s.p50_rel,
                100.0 * s.p95_rel, s.p95_abs, 100.0 * s.max_rel,
                s.covered ? 100.0 * double(s.within_1pct) / double(s.covered) : 0.0,
                s.covered ? 100.0 * double(s.within_2pct) / double(s.covered) : 0.0, 100.0 * kFitTolerance,
                s.covered ? 100.0 * double(s.triangle_nearer) / double(s.covered) : 0.0,
                s.covered ? 100.0 * double(s.triangle_farther) / double(s.covered) : 0.0, s.raster_only, tris, off, behind);
  };
  report(std::format("ID {} alone", chosen_).c_str(), chosen, chosen_triangles, chosen_off, chosen_behind);
  report("every ID", all, triangles, off_image, behind_camera);
  for (const auto& [key, pair] : fits) {
    const car_dump::FitCount& a = pair.first;
    const car_dump::FitCount& b = pair.second;
    if (a.samples + a.off + b.samples + b.off == 0) {
      continue;
    }
    REXLOG_INFO("{} fit id {} vs {:016x} ps {:016x}: pixels of the draws' own triangles on a vehicle pixel / off one: palette then "
                "gWorld {} / {} (match {:.1f}%, in front {:.1f}%, behind {:.1f}%), gWorld alone {} / {} (match {:.1f}%, in front "
                "{:.1f}%, behind {:.1f}%); a right recipe matches where the draw is the visible surface and is next to never in front",
                kPrefix, std::get<0>(key), std::get<1>(key), std::get<2>(key), a.samples, a.off, 100.0 * a.Share(a.match),
                100.0 * a.Share(a.front), 100.0 * a.Share(a.behind), b.samples, b.off, 100.0 * b.Share(b.match),
                100.0 * b.Share(b.front), 100.0 * b.Share(b.behind));
  }
}

}  // namespace mcla::gpu
