#pragma once
// ===========================================================================
// MCLA GPU renderer -- a car draw's triangles out of guest memory (shared by the car dump and the car frame)
// ===========================================================================
// car_dump.h (a one-frame debug dump) and car_frame.h (the player car's draws every frame) read the same things out of the guest: the
// vehicle the local player drives, what a vertex shader's microcode reads (and so how a draw's vertices reach the world,
// car_dump_rules.h), and a draw's triangles: the index range and the position stream of its declaration, endian-swapped and expanded
// to a triangle list with the renderer's own conversions, and for a skinned draw the blend of its bones. This is that code, in one
// place; the numbers it works on are car_dump_rules.h's (pure) and car_frame_rules.h's.
//
// Everything that reads guest memory (ReadTriangles, ReadNormals, SkinPositions) is for the render thread, inside the draw's own call
// (the guest is held back then: geometry.cpp reads the same ranges for the same draw). The ShaderReadsCache and ReadPlayerVehicle read
// guest memory too, the player's vehicle once a frame, the microcode once per shader.
// ===========================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "car_dump_rules.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "native_gfx/vertex_declaration.h"
#include "records.h"
#include "shaders.h"
#include "topology.h"

namespace mcla::gpu {

class GuestMemory;

namespace car_mesh {

// D3DDECLUSAGE (native_gfx/vertex_declaration.h).
inline constexpr uint32_t kUsagePosition = 0;
inline constexpr uint32_t kUsageBlendWeight = 1;
inline constexpr uint32_t kUsageBlendIndices = 2;
inline constexpr uint32_t kUsageNormal = 3;
// A draw with more indices than this is not a car's.
inline constexpr uint32_t kMaxIndices = 8u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// the player's vehicle
// ---------------------------------------------------------------------------
// The vehicle the local player drives, in the game's memory (mc_engine/squash_stretch.cpp, read off the game's code): the global
// dword_8288DCF8 holds the vehicle, vehicle + 0x14 its entity, entity + 0x90 the entity's world Matrix44 (row vectors, floats, big
// endian; row 3, at +0x30, is the translation).
inline constexpr uint32_t kPlayerVehicleGlobal = 0x8288DCF8u;
inline constexpr uint32_t kVehicleEntityOffset = 0x14;
inline constexpr uint32_t kEntityMatrixOffset = 0x90;
inline constexpr uint32_t kMatrixTranslationOffset = 0x30;

struct PlayerVehicle {
  bool valid = false;       // the position was read
  uint32_t vehicle = 0;     // dword_8288DCF8: the vehicle the local player drives
  uint32_t entity = 0;      // vehicle + 0x14
  double position[3] = {};  // row 3 of the entity's world matrix (entity + 0x90 + 0x30)
  std::string note;         // what was found, for the log (only when asked for)
};

// Reads the player's vehicle (every pointer is checked with IsGuestRangeReadable first). `with_note` makes the text of `note` (it
// allocates; the car frame asks for it only when it logs).
void ReadPlayerVehicle(PlayerVehicle* out, bool with_note);

// ---------------------------------------------------------------------------
// what a vertex shader reads
// ---------------------------------------------------------------------------

// What the microcode of a vertex shader was found to read (cached by shader identity, verified microcode only).
struct ShaderReads {
  car_dump::ConstantReads reads;
  bool verified = false;  // the microcode was readable and hashes to the shader's identity
};

// What the vertex shader of `record` reads, from its microcode: the draw record names the microcode in guest memory (a virtual address,
// as the capture reads it), and a read counts only when it hashes to the draw's shader identity (the guest may have loaded another
// shader there since). Only verified scans are kept.
class ShaderReadsCache {
 public:
  // `retry_frames` 0: a shader that could not be verified is tried again on every call (the car dump). Otherwise it is not tried
  // again for that many `frame`s (the car frame, which asks for every draw of every frame).
  const ShaderReads& Of(const DrawRecord& record, uint64_t frame = 0, uint32_t retry_frames = 0);
  void Clear() {
    reads_.clear();
    failed_.clear();
  }

 private:
  std::unordered_map<uint64_t, ShaderReads> reads_;
  std::unordered_map<uint64_t, uint64_t> failed_;  // identity -> the frame its read failed in
};

// ---------------------------------------------------------------------------
// a draw's streams and elements
// ---------------------------------------------------------------------------

// A vertex stream of a draw: where its bytes are and how they are laid out.
struct StreamRef {
  bool ok = false;
  bool inline_data = false;      // the draw's own inline vertices
  uint32_t address = 0;          // physical, from the stream's vertex fetch constant
  uint32_t size = 0;             // bytes
  uint32_t stride = 0;           // bytes
  const uint8_t* inline_base = nullptr;
};

// The stream `stream` of a draw: a vertex fetch constant (the address, the size in bytes, the endian 8in32 that geometry.cpp
// StreamSource accepts) and the device's stride, or the inline vertices. Not ok when the constant is no vertex fetch.
StreamRef ResolveStream(const DrawView& view, const DrawRecord& r, uint32_t stream);

// The element `usage` / index 0 of the draw's declaration (the first, as the input layout takes it); false when there is none.
bool FindElement(const DrawView& view, const DrawRecord& r, uint32_t usage, native_gfx::DeclarationElement* out);

// ---------------------------------------------------------------------------
// a draw's triangles
// ---------------------------------------------------------------------------

enum class Problem : uint8_t {
  kNone,
  kComputedFetch,  // the vertex shader's fetch index is computed (the rain's quads): no triangles of a car
  kPrimitive,      // a primitive the renderer does not draw as triangles (rectangle list, unsupported)
  kNotTriangles,   // lines
  kNoPosition,     // the declaration has no POSITION0
  kPositionFormat, // a position that does not fit its stride or has no format here
  kFetch,          // the stream's fetch constant is no vertex fetch, or has no stride
  kUnreadable,     // a range is not committed guest memory
  kNoTriangles,    // nothing left after the index expansion
};
const char* ProblemName(Problem problem);

// One draw's triangles in model space (the positions as the guest has them).
struct Triangles {
  Problem problem = Problem::kNone;
  bool strip = false;      // the host topology is a strip (expanded here)
  bool index32 = false;
  car_dump::PositionFormat format = car_dump::PositionFormat::kNone;
  uint32_t stream = 0;
  uint32_t offset = 0;
  uint32_t stride = 0;
  uint32_t degenerate = 0;
  uint32_t out_of_range = 0;
  uint32_t first_vertex = 0;   // `local` starts at this vertex of the stream
  uint32_t triangles = 0;
  uint32_t vertices = 0;       // distinct vertices the triangles name
  std::vector<float> local;    // model-space positions of the vertices first_vertex..first_vertex + span, 3 floats each
  std::vector<uint32_t> tri;   // 3 numbers a triangle, relative to first_vertex

  // Back to nothing, keeping the vectors' memory.
  void Reset() {
    problem = Problem::kNone;
    strip = false;
    index32 = false;
    format = car_dump::PositionFormat::kNone;
    stream = offset = stride = 0;
    degenerate = out_of_range = 0;
    first_vertex = 0;
    triangles = vertices = 0;
    local.clear();
    tri.clear();
  }
};

// What a draw's triangles are read from: the prepared draw (Scene::NoteAmbientOcclusion).
struct DrawInputs {
  const DrawView* view = nullptr;
  const XenosState* state = nullptr;
  const ShaderStore::Entry* vs = nullptr;
  HostPrimitive primitive;
};

// Scratch of ReadTriangles, reused from call to call.
struct Scratch {
  std::vector<uint32_t> guest_indices;
  std::vector<uint32_t> list;
  std::vector<uint8_t> seen;
};

// The triangles of the draw: the indices expanded to a triangle list by the renderer's own functions (geometry.cpp PrepareDraw does the
// same), the positions of the vertices they name (one range of the stream) decoded. `out->problem` says why a draw has none.
void ReadTriangles(const GuestMemory& memory, const DrawInputs& in, Scratch& scratch, Triangles* out);

// The blended positions of a skinned draw (car_dump_rules.h SkinBlend): the BLENDINDICES and BLENDWEIGHT elements of its declaration over
// the vertices `first`.. `first + span` of their streams, the palette of the draw's bank `vs_bank`; `local` the model-space positions
// of those vertices (Triangles::local). False, with `*note` saying why, when it cannot be done. `directions_in` (null: none; else x y z
// a vertex) are blended too, into `directions_out` (the rotation part of the same bones, not normalised).
bool SkinPositions(const GuestMemory& memory, const DrawView& view, const uint8_t* vs_bank, uint32_t first, uint32_t span,
                   const std::vector<float>& local, std::vector<float>* blended, car_dump::SkinStats* stats, std::string* note,
                   const std::vector<float>* directions_in = nullptr, std::vector<float>* directions_out = nullptr);

// The NORMAL element of the draw's declaration for the vertices `first`.. `first + span`, as x y z a vertex (the guest's numbers, not
// normalised). False (and `out` empty) when the draw has no NORMAL, a format that is not decoded, or a stream that is not readable.
bool ReadNormals(const GuestMemory& memory, const DrawView& view, uint32_t first, uint32_t span, std::vector<float>* out);

}  // namespace car_mesh
}  // namespace mcla::gpu
