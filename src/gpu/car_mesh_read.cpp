// MCLA GPU renderer -- a car draw's triangles out of guest memory. See car_mesh_read.h.

#include "car_mesh_read.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "car_frame_rules.h"
#include "guest_memory.h"
#include "native_gfx/guest/guest_resources.h"
#include "native_gfx/shader_identity.h"

namespace mcla::gpu::car_mesh {

namespace nfx = mcla::native_gfx;

namespace {

const uint8_t* GuestBase() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->virtual_membase() : nullptr;
}

}  // namespace

const char* ProblemName(Problem problem) {
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

// ---------------------------------------------------------------------------
// the player's vehicle
// ---------------------------------------------------------------------------

// The player's vehicle in the game's own memory: dword_8288DCF8 -> vehicle, +0x14 -> entity, +0x90 its world matrix (see the constants).
// The guest runs on while the render thread works, so this is the state of a frame or a few after the draws (a car at 25 m/s moves 0.4 m
// a frame; car_dump_rules.h kPlayerMatchMetres takes up to 5 m).
void ReadPlayerVehicle(PlayerVehicle* out, bool with_note) {
  out->valid = false;
  out->vehicle = 0;
  out->entity = 0;
  out->position[0] = out->position[1] = out->position[2] = 0.0;
  if (with_note) {
    out->note.clear();
  }
  const uint8_t* base = GuestBase();
  if (!base) {
    if (with_note) {
      out->note = "no guest memory";
    }
    return;
  }
  const auto bytes = [&](uint32_t address, uint32_t size) -> const uint8_t* {
    if (address < 0x1000u || (address & 3u) != 0 || !nfx::IsGuestRangeReadable(address, size)) {
      return nullptr;
    }
    return rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
  };
  const uint8_t* global = bytes(kPlayerVehicleGlobal, 4);
  if (!global) {
    if (with_note) {
      out->note = std::format("the global {:#010x} is not readable", kPlayerVehicleGlobal);
    }
    return;
  }
  out->vehicle = car_dump::LoadBe32(global);
  const uint8_t* vehicle = bytes(out->vehicle + kVehicleEntityOffset, 4);
  if (out->vehicle == 0 || !vehicle) {
    if (with_note) {
      out->note = std::format("the global {:#010x} holds {:#010x}: no vehicle", kPlayerVehicleGlobal, out->vehicle);
    }
    return;
  }
  out->entity = car_dump::LoadBe32(vehicle);
  const uint8_t* matrix = bytes(out->entity + kEntityMatrixOffset, 64);
  if (out->entity == 0 || !matrix) {
    if (with_note) {
      out->note = std::format("vehicle {:#010x} holds entity {:#010x}: no matrix", out->vehicle, out->entity);
    }
    return;
  }
  for (uint32_t c = 0; c < 3; ++c) {
    const uint32_t bits = car_dump::LoadBe32(matrix + kMatrixTranslationOffset + 4 * c);
    float value;
    std::memcpy(&value, &bits, 4);
    out->position[c] = double(value);
  }
  out->valid = std::isfinite(out->position[0]) && std::isfinite(out->position[1]) && std::isfinite(out->position[2]);
  if (with_note) {
    out->note = out->valid ? std::format("vehicle {:#010x}, entity {:#010x}, world position ({:.2f}, {:.2f}, {:.2f})", out->vehicle,
                                         out->entity, out->position[0], out->position[1], out->position[2])
                           : std::format("vehicle {:#010x}, entity {:#010x}: its matrix has no finite translation", out->vehicle,
                                         out->entity);
  }
}

// ---------------------------------------------------------------------------
// what a vertex shader reads
// ---------------------------------------------------------------------------

const ShaderReads& ShaderReadsCache::Of(const DrawRecord& record, uint64_t frame, uint32_t retry_frames) {
  const auto found = reads_.find(record.vs_identity);
  if (found != reads_.end()) {
    return found->second;
  }
  static const ShaderReads kNone;
  if (retry_frames != 0) {
    const auto failed = failed_.find(record.vs_identity);
    if (failed != failed_.end() && frame >= failed->second && frame - failed->second < retry_frames) {
      return kNone;
    }
  }
  const auto fail = [&]() -> const ShaderReads& {
    if (retry_frames != 0 && record.vs_identity != 0) {
      failed_[record.vs_identity] = frame;
    }
    return kNone;
  };
  const uint8_t* base = GuestBase();
  const uint32_t address = record.vs_ucode_address;
  const uint32_t size = record.vs_ucode_size;
  if (!base || record.vs_identity == 0 || size < 12 || size > nfx::kMaxUcodeBytes || (address & 3u) != 0 ||
      !nfx::IsGuestRangeReadable(address, size)) {
    return fail();
  }
  const uint8_t* ucode = rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
  if (nfx::ShaderIdentity(ucode, size) != record.vs_identity) {
    return fail();
  }
  ShaderReads out;
  out.reads = car_dump::ScanConstantReads(ucode, size);
  out.verified = out.reads.valid;
  if (!out.verified) {
    return fail();
  }
  return reads_.emplace(record.vs_identity, out).first->second;
}

// ---------------------------------------------------------------------------
// streams and elements
// ---------------------------------------------------------------------------

StreamRef ResolveStream(const DrawView& view, const DrawRecord& r, uint32_t stream) {
  StreamRef out;
  if (r.kind == DrawKind::kInline) {
    out.inline_data = true;
    out.stride = r.inline_stride;
    out.size = r.inline_bytes;
    out.inline_base = view.inline_data();
    out.ok = true;
    return out;
  }
  const uint32_t slot = nfx::FetchSlotForStream(stream);
  if (slot >= guest::kFetchGroupCount * 3 || stream >= guest::kStreamCount) {
    return out;
  }
  uint32_t d0 = 0, d1 = 0;
  view.VertexFetch(slot, &d0, &d1);
  if ((d0 & 3u) != 3u || (d1 & 3u) != car_dump::kEndian8in32) {
    return out;
  }
  out.address = d0 & 0xFFFFFFFCu;
  out.size = d1 & 0x03FFFFFCu;
  out.stride = uint32_t(r.stream_strides[stream]) * 4u;
  out.ok = true;
  return out;
}

bool FindElement(const DrawView& view, const DrawRecord& r, uint32_t usage, nfx::DeclarationElement* out) {
  for (uint32_t i = 0; i < r.decl_count && i < guest::kMaxDeclElements; ++i) {
    const nfx::DeclarationElement e = nfx::ReadDeclarationElement(view.decl_records(), i);
    if (e.stream == 0xFF) {
      break;
    }
    if (e.usage == usage && e.usage_index == 0) {
      *out = e;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// a draw's triangles
// ---------------------------------------------------------------------------

void ReadTriangles(const GuestMemory& memory, const DrawInputs& in, Scratch& scratch, Triangles* out) {
  const DrawView& view = *in.view;
  const DrawRecord& r = view.record();
  out->Reset();

  // What the renderer draws: lists and strips as they are, quads and fans as lists of triangles (topology.h).
  const PrimitiveConversion conversion = in.primitive.conversion;
  const plume::RenderPrimitiveTopology topology = in.primitive.topology;
  if ((r.flags & kDrawVsComputedFetchIndex) != 0 || (in.vs && (in.vs->flags & ShaderStore::kComputedFetchIndex) != 0)) {
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
  if (!FindElement(view, r, kUsagePosition, &position)) {
    out->problem = Problem::kNoPosition;
    return;
  }
  out->format = car_dump::PositionFormatOf(position.type);
  out->stream = position.stream;
  out->offset = position.offset;

  // Its stream: a vertex fetch constant (the address, the size in bytes, the endian) and the device's stride, or the inline vertices.
  const StreamRef source = ResolveStream(view, r, position.stream);
  if (!source.ok) {
    out->problem = Problem::kFetch;
    return;
  }
  const uint32_t stride = source.stride;
  out->stride = stride;
  const uint32_t bytes = car_dump::PositionBytes(out->format);
  if (bytes == 0 || stride == 0 || position.offset + bytes > stride) {
    out->problem = Problem::kPositionFormat;
    return;
  }
  const uint32_t available = source.size / stride;

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
    const uint8_t* raw = memory.Read(index_address, count * index_size);
    if (!raw) {
      out->problem = Problem::kUnreadable;
      return;
    }
    out->index32 = index32;
    car_dump::DecodeIndices(raw, count, index32, endian, &scratch.guest_indices);
    vertex_base = r.base_vertex;
    reset = in.state->sc_mode.multi_prim_ib_ena != 0;
    reset_index = in.state->reset_index & (index32 ? 0xFFFFFFFFu : 0xFFFFu);
  } else {
    scratch.guest_indices.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      scratch.guest_indices[i] = i;
    }
    vertex_base = r.kind == DrawKind::kAuto ? int64_t(r.start) : 0;
  }
  scratch.list.clear();
  if (conversion == PrimitiveConversion::kNone) {
    if (strip) {
      scratch.list.resize(size_t(count) * 3);
      const uint32_t n = StripToList(true, scratch.guest_indices.data(), count, reset, reset_index, scratch.list.data());
      scratch.list.resize(n);
    } else {
      scratch.list = scratch.guest_indices;
    }
  } else {
    const uint32_t n = ConvertedIndexCount(conversion, count);
    scratch.list.resize(n);
    if (n != 0) {
      ConvertIndices(conversion, scratch.guest_indices.data(), count, scratch.list.data());
    }
  }
  car_dump::TriangleSet set;
  car_dump::CollectTriangles(scratch.list.data(), uint32_t(scratch.list.size()), vertex_base, available, &set);
  out->degenerate = set.degenerate;
  out->out_of_range = set.out_of_range;
  if (set.triangles() == 0) {
    out->problem = Problem::kNoTriangles;
    return;
  }

  // The positions of the vertices the triangles name (one range of the stream).
  const uint32_t first = set.first_vertex;
  const uint32_t span = set.span();
  const uint8_t* base = source.inline_data ? source.inline_base + size_t(first) * stride
                                           : memory.Read(source.address + first * stride, span * stride);
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
  scratch.seen.assign(span, 0);
  for (uint32_t& n : out->tri) {
    n -= first;
    scratch.seen[n] = 1;
  }
  out->triangles = uint32_t(out->tri.size() / 3);
  out->vertices = uint32_t(std::count(scratch.seen.begin(), scratch.seen.end(), uint8_t(1)));
}

bool SkinPositions(const GuestMemory& memory, const DrawView& view, const uint8_t* vs_bank, uint32_t first, uint32_t span,
                   const std::vector<float>& local, std::vector<float>* blended, car_dump::SkinStats* stats, std::string* note,
                   const std::vector<float>* directions_in, std::vector<float>* directions_out) {
  const DrawRecord& r = view.record();
  nfx::DeclarationElement indices, weights;
  if (!FindElement(view, r, kUsageBlendIndices, &indices) || !FindElement(view, r, kUsageBlendWeight, &weights)) {
    *note = "the declaration has no BLENDINDICES / BLENDWEIGHT";
    return false;
  }
  const car_dump::BlendFormat index_format = car_dump::BlendFormatOf(indices.type);
  const car_dump::BlendFormat weight_format = car_dump::BlendFormatOf(weights.type);
  if (index_format == car_dump::BlendFormat::kNone || weight_format == car_dump::BlendFormat::kNone) {
    *note = std::format("blend formats not handled (BLENDINDICES decltype {:08X}, BLENDWEIGHT {:08X})", uint32_t(indices.type),
                        uint32_t(weights.type));
    return false;
  }
  const uint32_t index_bytes = car_dump::BlendBytes(index_format);
  const uint32_t weight_bytes = car_dump::BlendBytes(weight_format);
  const StreamRef index_stream = ResolveStream(view, r, indices.stream);
  const StreamRef weight_stream = ResolveStream(view, r, weights.stream);
  if (!index_stream.ok || !weight_stream.ok || index_stream.stride == 0 || weight_stream.stride == 0 ||
      indices.offset + index_bytes > index_stream.stride || weights.offset + weight_bytes > weight_stream.stride) {
    *note = "the blend elements do not fit their streams";
    return false;
  }
  if (uint64_t(first + span) * index_stream.stride > index_stream.size ||
      uint64_t(first + span) * weight_stream.stride > weight_stream.size) {
    *note = "the blend streams are shorter than the position stream";
    return false;
  }
  const auto window = [&](const StreamRef& stream) -> const uint8_t* {
    return stream.inline_data ? stream.inline_base + size_t(first) * stream.stride
                              : memory.Read(stream.address + first * stream.stride, span * stream.stride);
  };
  const uint8_t* index_base = window(index_stream);
  const uint8_t* weight_base = window(weight_stream);
  if (!index_base || !weight_base) {
    *note = "the blend streams are not readable";
    return false;
  }
  car_dump::SkinPalette palette;
  car_dump::LoadPalette(vs_bank, &palette);
  blended->resize(size_t(span) * 3);
  const bool directions = directions_in && directions_out && directions_in->size() == size_t(span) * 3;
  if (directions) {
    directions_out->resize(size_t(span) * 3);
  }
  *stats = car_dump::SkinStats{};
  for (uint32_t v = 0; v < span; ++v) {
    const car_dump::SkinInfluence influence =
        car_dump::ReadInfluence(index_base + size_t(v) * index_stream.stride + indices.offset, index_format,
                                weight_base + size_t(v) * weight_stream.stride + weights.offset, weight_format);
    car_dump::SkinBlend(palette, influence, &local[size_t(v) * 3], &(*blended)[size_t(v) * 3], stats);
    if (directions) {
      car_frame_rules::SkinBlendDirection(palette, influence, &(*directions_in)[size_t(v) * 3], &(*directions_out)[size_t(v) * 3]);
    }
  }
  return true;
}

bool ReadNormals(const GuestMemory& memory, const DrawView& view, uint32_t first, uint32_t span, std::vector<float>* out) {
  out->clear();
  const DrawRecord& r = view.record();
  nfx::DeclarationElement normal;
  if (!FindElement(view, r, kUsageNormal, &normal)) {
    return false;
  }
  const car_frame_rules::NormalFormat format = car_frame_rules::NormalFormatOf(normal.type);
  const uint32_t bytes = car_frame_rules::NormalBytes(format);
  const StreamRef stream = ResolveStream(view, r, normal.stream);
  if (bytes == 0 || !stream.ok || stream.stride == 0 || normal.offset + bytes > stream.stride ||
      uint64_t(first + span) * stream.stride > stream.size) {
    return false;
  }
  const uint8_t* base = stream.inline_data ? stream.inline_base + size_t(first) * stream.stride
                                           : memory.Read(stream.address + first * stream.stride, span * stream.stride);
  if (!base) {
    return false;
  }
  out->resize(size_t(span) * 3);
  for (uint32_t v = 0; v < span; ++v) {
    car_frame_rules::DecodeNormal(base + size_t(v) * stream.stride + normal.offset, format, &(*out)[size_t(v) * 3]);
  }
  return true;
}

}  // namespace mcla::gpu::car_mesh
