#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — guest texture identity
// ===========================================================================
// Step one of resource ownership, in OBSERVE mode.
//
// grcTextureXenon::Init (sub_821841B0) is the funnel every texture passes
// through, both the placed road (header built over memory the streamed
// resource already owns) and the allocated road
// (grcTextureFactoryXenon::CreateTexture -> D3DDevice_CreateTexture). See
// guest/d3d_structs.h for the full map.
//
// Hooking it after the original runs costs nothing and answers a question the
// native runtime cannot answer today: WHICH texture a given guest address is.
// The texture cache keys purely on address ranges, so a diagnostic can say
// "0x06ACD000 decoded as 1280x720 BC1" but never "that is hud_glow_ring". The
// name is right there in the constructor argument.
//
// It is also the exact hook site ownership will use: when the runtime starts
// handing back its own objects, this becomes the place that allocates one out
// of HostResourceHeap instead of letting the guest allocate 52 bytes.
// ===========================================================================

#include <cstdint>
#include <string>

namespace mcla::native_gfx {

// Which of the two roads produced this texture. Recorded because they have
// very different implications for ownership: a placed texture's pixels belong
// to a streamed resource the runtime must not free, an allocated one's do not.
enum class TextureOrigin : uint8_t {
  kUnknown = 0,
  kPlaced = 1,     // Init's placed road: header built over image data it was given
  kAllocated = 2,  // Init's allocated road, via D3DDevice_CreateTexture (refcounted)
  kResource = 3,   // deserialized: the header came inside the streamed resource
};

struct TextureIdentity {
  uint32_t d3d_texture_va = 0;  // the D3DTexture object
  uint32_t grc_texture_va = 0;  // the owning grcTextureXenon
  uint32_t base_address = 0;    // fetch constant base, physical
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 0;
  uint32_t format = 0;  // raw Xenos TextureFormat
  uint32_t total_bytes = 0;
  TextureOrigin origin = TextureOrigin::kUnknown;
  std::string name;
};

// Called from the grcTextureXenon_Init hook, AFTER the original has run: the
// D3DTexture does not exist until then. Cheap and non-throwing; silently does
// nothing when the registry is off or the object looks wrong.
void NoteTextureCreated(const uint8_t* base, uint32_t grc_texture_va, uint32_t name_va);

// Called from the grcTextureXenon_ResourceCtor hook (sub_82184458), AFTER the
// original: the pointer fixups are what make the object readable at all.
//
// This is where the streamed textures are, and there are far more of them than
// Init ever sees. Init only builds the handful that exist at boot -- render
// targets and dynamically created surfaces -- which is why a registry hooked
// on Init alone froze at 111 entries for a whole session while the game
// streamed the city in and out.
//
// The name is not an argument here: it is a fixed-up char* at grc+24, which
// the constructor patches just above the D3DTexture pointer.
void NoteTextureFromResource(const uint8_t* base, uint32_t grc_texture_va);

// Called from the grcTextureXenon_dtor hook, BEFORE the original: the
// D3DTexture pointer is still readable at that point.
void NoteTextureDestroyed(const uint8_t* base, uint32_t grc_texture_va);

// Identity of the texture whose fetch constant points at this base address, or
// nullptr. Addresses are the physical ones fetch constants carry.
const TextureIdentity* LookupByBaseAddress(uint32_t base_address);

// Identity behind a D3DTexture object pointer, or nullptr.
const TextureIdentity* LookupByD3DTexture(uint32_t d3d_texture_va);

// Writes mcla_native_gfx_textures.txt: every live texture with its name,
// dimensions, format and origin. Called at a frame boundary while the cvar is
// on, once.
void DumpTextureRegistry();

// Live count, for diagnostics.
uint64_t TextureRegistryLiveCount();

}  // namespace mcla::native_gfx
