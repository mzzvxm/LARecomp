#include "rsc5.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "lzx_encode.h"

namespace mc::modloader {

namespace {

constexpr uint32_t kRsc5Magic = 0x05435352u;
constexpr uint32_t kXCompressMagic = 0x0FF512EFu;
constexpr uint32_t kVirtualBase = 0x50000000u;
constexpr uint32_t kPhysicalBase = 0x60000000u;

constexpr uint32_t kPrimTriangleList = 3;

// A submesh's vertex layout, read from the declaration rather than assumed.
//
// Most character submeshes are position, blend weights, blend indices, normal,
// colour, UV and tangent in 36 bytes, but not all: some drop the tangent and
// pack into 32, and a single character can hold both. Requiring one layout
// meant a whole model was refused over a submesh that differed by one field.
//
// The declaration is a 16-bit mask of which semantics are present, a stride, and
// four bits of component type per semantic. Elements follow in semantic order,
// each starting where the last one ended.
struct VertexLayout {
    uint32_t stride = 0;
    int offset[16];  // byte offset of each semantic, -1 when absent

    VertexLayout() {
        for (int& value : offset) value = -1;
    }
    bool has(int semantic) const { return offset[semantic] >= 0; }
};

// Semantics, in the order they are laid out.
constexpr int kSemPosition = 0;
constexpr int kSemBlendWeights = 1;
constexpr int kSemBlendIndices = 2;
constexpr int kSemNormal = 3;
constexpr int kSemColour = 4;
constexpr int kSemTexcoord0 = 6;
constexpr int kSemTexcoord1 = 7;
constexpr int kSemTangent = 14;

// Size of each component type. Only the ones character meshes use are listed;
// anything else makes the submesh unwritable, which is safer than guessing a
// width and shifting every field after it. The two shipped layouts add up to
// exactly their declared stride under this table, 36 and 32, which is what says
// it is right.
uint32_t ComponentSize(uint32_t type) {
    switch (type) {
        case 1: return 4;   // Half2, the UV
        case 6: return 12;  // Float3, the position
        case 9: return 4;   // D3DCOLOR: weights, indices, colour
        case 10: return 4;  // Dec3N: normal, tangent
        default: return 0;
    }
}

uint32_t LoadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void StoreBE32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

void StoreBE16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

float LoadBEFloat(const uint8_t* p) {
    const uint32_t bits = LoadBE32(p);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

void StoreBEFloat(uint8_t* p, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    StoreBE32(p, bits);
}

uint16_t FloatToHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);

    const uint32_t sign = (bits >> 16) & 0x8000u;
    int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent <= 0) return static_cast<uint16_t>(sign);
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) |
                                 (mantissa >> 13));
}

// Dec3N in the plain order: bits 0-9 hold X, bits 10-19 Y, bits 20-29 Z.
//
// There is no axis rotation. This used to write Y, Z, X, and every normal and
// tangent the loader shipped reached the shader turned a third of a turn --
// the facets and the wrong reflections on the BMW's shell, and the highlight
// that sweeps round a replaced wheel as it spins. Measured on the game's own
// drawables by fitting all 48 axis orders against normals rebuilt from the
// triangles: X-low wins at a median dot of 0.991 on whl_am_bbs_ch, 0.996 on
// vp_chv_impala_96's body and 0.995 on drv_mp_01_set, and the native
// renderer's unpacker reads int3(v, v >> 10, v >> 20) and draws those right.
uint32_t PackDec3N(float x, float y, float z) {
    auto pack = [](float v) -> uint32_t {
        const float clamped = std::max(-1.0f, std::min(1.0f, v));
        int quantised = static_cast<int>(std::lround(clamped * 511.0f));
        quantised = std::max(-511, std::min(511, quantised));
        return static_cast<uint32_t>(quantised) & 0x3FFu;
    };
    return pack(x) | (pack(y) << 10) | (pack(z) << 20);
}

void UnpackDec3N(uint32_t packed, float& x, float& y, float& z) {
    auto unpack = [](uint32_t field) -> float {
        field &= 0x3FFu;
        const int signed_value = (field & 0x200u) ? static_cast<int>(field) - 1024 : static_cast<int>(field);
        return static_cast<float>(signed_value) / 511.0f;
    };
    x = unpack(packed);
    y = unpack(packed >> 10);
    z = unpack(packed >> 20);
}

// Address translation for the two RSC5 pointer spaces.
class Rsc5View {
 public:
    Rsc5View(std::vector<uint8_t>& data, uint32_t virtual_size)
        : data_(data), virtual_size_(virtual_size) {}

    bool Offset(uint32_t address, size_t need, size_t& out) const {
        size_t offset;
        if ((address & kVirtualBase) == kVirtualBase && (address & kPhysicalBase) != kPhysicalBase) {
            offset = address & 0x0FFFFFFFu;
        } else if ((address & kPhysicalBase) == kPhysicalBase) {
            offset = static_cast<size_t>(address & 0x1FFFFFFFu) + virtual_size_;
        } else {
            return false;
        }
        if (offset + need > data_.size()) return false;
        out = offset;
        return true;
    }

    bool U32(uint32_t address, uint32_t& out) const {
        size_t offset;
        if (!Offset(address, 4, offset)) return false;
        out = LoadBE32(data_.data() + offset);
        return true;
    }

    bool U8(uint32_t address, uint8_t& out) const {
        size_t offset;
        if (!Offset(address, 1, offset)) return false;
        out = data_[offset];
        return true;
    }

    bool U16(uint32_t address, uint16_t& out) const {
        size_t offset;
        if (!Offset(address, 2, offset)) return false;
        out = static_cast<uint16_t>((data_[offset] << 8) | data_[offset + 1]);
        return true;
    }

    bool String(uint32_t address, std::string& out) const {
        size_t offset;
        if (address == 0 || !Offset(address, 1, offset)) return false;
        out.clear();
        while (offset < data_.size() && data_[offset] != 0) out.push_back(
            static_cast<char>(data_[offset++]));
        return true;
    }

    bool Float(uint32_t address, float& out) const {
        size_t offset;
        if (!Offset(address, 4, offset)) return false;
        out = LoadBEFloat(data_.data() + offset);
        return true;
    }

    bool SetU32(uint32_t address, uint32_t value) {
        size_t offset;
        if (!Offset(address, 4, offset)) return false;
        StoreBE32(data_.data() + offset, value);
        return true;
    }

    bool SetU16(uint32_t address, uint16_t value) {
        size_t offset;
        if (!Offset(address, 2, offset)) return false;
        StoreBE16(data_.data() + offset, value);
        return true;
    }

 private:
    std::vector<uint8_t>& data_;
    uint32_t virtual_size_ = 0;
};

// Where a resource keeps its drawable, and where the drawable keeps everything
// hanging off it.
//
// Two shapes ship, and only the root differs between them -- LOD, model,
// geometry, vertex buffer and index buffer are byte for byte the same, which is
// why describing the root is enough to reuse the whole rewrite.
//
// A character (`<name>.xrsc`, resource type 6) wraps the drawable in a resource
// object: the drawable's address is at virtual+16, and inside it the shader
// group sits at +8, the skeleton at +12, the bounding box at +32 and +48, and
// four LOD slots at +64.
//
// A wheel or a vehicle body (`body_lod_N.xrsc`, resource type 63) is its own
// root at the start of the virtual segment. It carries three LOD slots at +8,
// the skeleton at +36, and no inline bounding box at all -- the bounds are
// taken from the template's own vertices instead. Its shader group is a
// different class as well: thirty-two byte shaders picked by vtable, with no
// name pointer and no embedded textures, because a wheel's textures live in the
// sibling `<name>.xtp` (resource type 83). The group is therefore not offered
// here at all -- see ReplaceDictionaryTexture, which rewrites that resource
// separately, for where a wheel's paint actually comes from.
struct DrawableLayout {
    uint32_t drawable = 0;
    uint32_t lod_field = 64;      // first LOD slot, offset within the drawable
    uint32_t lod_slots = 4;
    uint32_t skeleton_field = 12;
    bool inline_bounds = true;    // drawable+32 is min, drawable+48 is max
    bool shader_group = true;     // the group is one this code can read
};

constexpr uint32_t kTypeCharacterDrawable = 6;
constexpr uint32_t kTypeBodyDrawable = 63;

bool ResolveDrawable(const Rsc5View& view, uint32_t type, DrawableLayout& out) {
    if (type == kTypeBodyDrawable) {
        out.drawable = kVirtualBase;
        out.lod_field = 8;
        out.lod_slots = 3;
        out.skeleton_field = 36;
        out.inline_bounds = false;
        out.shader_group = false;
        return true;
    }
    return view.U32(kVirtualBase + 16, out.drawable) && out.drawable != 0;
}

// Reads a submesh's declaration. Fails when a field is of a type this cannot
// place, so the caller can leave that submesh out rather than write nonsense.
bool ReadVertexLayout(const Rsc5View& view, uint32_t vertex_buffer, VertexLayout& out,
                     bool require_uv = true) {
    uint32_t declaration = 0, fvf = 0, stride = 0, low = 0, high = 0;
    if (!view.U32(vertex_buffer + 16, declaration) || declaration == 0 ||
        !view.U32(declaration, fvf) || !view.U32(vertex_buffer + 12, stride) || stride == 0 ||
        !view.U32(declaration + 8, high) || !view.U32(declaration + 12, low)) {
        return false;
    }

    const uint64_t types = (static_cast<uint64_t>(high) << 32) | low;
    uint32_t offset = 0;
    for (int semantic = 0; semantic < 16; ++semantic) {
        if (((fvf >> semantic) & 1) == 0) continue;
        const uint32_t size = ComponentSize((types >> (semantic * 4)) & 0xF);
        if (size == 0) return false;
        out.offset[semantic] = static_cast<int>(offset);
        offset += size;
    }
    // The elements have to account for the stride exactly; if they do not, the
    // table above is wrong about something and every offset past it is suspect.
    if (offset != stride) return false;

    out.stride = stride;
    // A UV is demanded by default because a character's every submesh samples a
    // skin, so one without a texture coordinate is a misread declaration rather
    // than a real slot. A car's BlackMatte submeshes are real and carry nothing
    // but a position, so the caller can lift the requirement.
    if (!out.has(kSemPosition)) return false;
    return require_uv ? out.has(kSemTexcoord0) : true;
}

struct GeometryRef {
    uint32_t address = 0;
    uint32_t vertex_buffer = 0;
    uint32_t index_buffer = 0;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t bone_palette = 0;
    uint32_t bone_count = 0;
    uint32_t shader = 0;       // index into the drawable's shader group
    uint32_t shader_map = 0;   // the owning model's per-geometry shader array
    uint16_t slot_in_model = 0;
    uint16_t model = 0;        // which of the LOD's models owns it
    VertexLayout layout;
};

// Both buffer objects carry an embedded GPU resource header -- the vertex one
// at +28, the index one at +12 -- and that header, not the software pointer, is
// what the draw actually fetches from. Word 6 holds the address (the vertex
// flavour ORs in an endian field of 3), word 7 the size: for vertices the count
// in dwords shifted left by two inside a 0x10000002 frame, for indices a plain
// byte count. Verified against all fifteen submeshes of the shipped driver.
// Leaving the size stale is what makes a swapped mesh render as spikes: the GPU
// keeps reading the original vertices while the draw uses the new index count.
// Word 6 is never written -- every buffer stays exactly where it was.
constexpr uint32_t kGpuHeaderSizeWord = 7 * 4;
constexpr uint32_t kGpuHeaderAddressWord = 6 * 4;

// Where each bone of the driver's skeleton sits in bind pose, in model space.
//
// The skeleton stores parent indices at skeleton+0x04, 64-byte per-bone
// transforms at skeleton+0x10 (three rows of basis, translation in the last) and
// the bone count in the first half of skeleton+0x14.
//
// The translations are parent-relative but expressed along the MODEL's axes, not
// along the parent's rotated frame, so accumulating a position means adding the
// translations down the chain and nothing else. Composing the rotations the way
// a normal hierarchy would is wrong here and not subtly: the upper-arm bone
// carries a forty-five degree roll about Z, and letting it turn its children
// swings the whole arm out to x = +/-0.56 and up to y = 1.07 -- an A-pose that
// exists nowhere in the game. The shipped mesh says otherwise, and it is the
// authority: averaging the vertices each bone actually drives puts the hand at
// (-0.18, 0.88), arms hanging straight down. Summing translations alone
// reproduces that to within 4 cm across all 92 bones, against 22 cm for the
// composed version -- and a mod retargeted onto the composed pose comes out with
// its arms raised and its hands mapped to the wrong bones.
struct BoneTransform {
    float t[3] = {0, 0, 0};
};

bool ReadSkeletonBindPose(const Rsc5View& view, const DrawableLayout& drawable,
                          std::vector<BoneTransform>& out, std::vector<uint32_t>& parent_out) {
    uint32_t skeleton = 0, parents = 0, matrices = 0;
    uint16_t count = 0;
    if (!view.U32(drawable.drawable + drawable.skeleton_field, skeleton) || skeleton == 0 ||
        !view.U32(skeleton + 4, parents) || parents == 0 ||
        !view.U32(skeleton + 16, matrices) || matrices == 0 ||
        !view.U16(skeleton + 20, count) || count == 0) {
        return false;
    }

    std::vector<BoneTransform> local(count);
    std::vector<uint32_t> parent(count);
    for (uint16_t bone = 0; bone < count; ++bone) {
        if (!view.U32(parents + bone * 4u, parent[bone])) return false;
        for (int i = 0; i < 3; ++i) {
            if (!view.Float(matrices + bone * 64u + 48u + i * 4u, local[bone].t[i])) return false;
        }
    }

    parent_out = parent;
    out.assign(count, BoneTransform{});
    std::vector<bool> done(count, false);
    for (uint16_t bone = 0; bone < count; ++bone) {
        std::vector<uint16_t> chain;
        uint32_t walk = bone;
        while (walk < count && !done[walk]) {
            chain.push_back(static_cast<uint16_t>(walk));
            if (parent[walk] == walk) break;
            walk = parent[walk];
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const uint16_t current = *it;
            const uint32_t up = parent[current];
            const bool root = up == current || up >= count;
            for (int i = 0; i < 3; ++i) {
                out[current].t[i] =
                    root ? local[current].t[i] : local[current].t[i] + out[up].t[i];
            }
            done[current] = true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Retargeting one humanoid rig onto another
//
// Matching joints to bones by bind-pose position looks reasonable and is wrong,
// because the two rigs do not share a bind pose: MCLA's driver is authored in an
// A-pose, arms angled down at forty-five degrees, while an exported character is
// almost always in a T-pose with the arms straight out. Measured as distance, a
// T-posed wrist lands nearer the game's forearm than its hand, and the fingers
// land nearer the forearm still -- which is exactly what tore the hands apart.
//
// What both rigs do share is shape: a pelvis, a spine up to a chest that the
// arms hang off, a neck to a head, two arms ending at a wrist that branches into
// fingers, two legs ending at a toe. Those landmarks can be found from the
// hierarchy alone, and once the limbs are paired the joints inside them match by
// how far along the limb they sit. Distance along a limb does not care what pose
// the limb is in, so the mapping comes out the same for a T-pose and an A-pose.
// ---------------------------------------------------------------------------

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

// Column-convention 3x3: Apply(v) is M * v.
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    Vec3 Apply(const Vec3& v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
    }
    Mat3 operator*(const Mat3& other) const {
        Mat3 out;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                out.m[row * 3 + col] = m[row * 3 + 0] * other.m[0 * 3 + col] +
                                       m[row * 3 + 1] * other.m[1 * 3 + col] +
                                       m[row * 3 + 2] * other.m[2 * 3 + col];
            }
        }
        return out;
    }
};

// The shortest rotation taking one unit vector onto another (Rodrigues).
Mat3 RotationBetween(const Vec3& from, const Vec3& to) {
    const float cosine = Dot(from, to);
    if (cosine > 0.99999f) return Mat3{};
    Vec3 axis = Cross(from, to);
    float sine = Length(axis);
    if (sine < 1e-6f) {
        // Opposed: any perpendicular axis will do, so build one.
        Vec3 candidate = std::fabs(from.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        axis = Cross(from, candidate);
        sine = Length(axis);
        if (sine < 1e-6f) return Mat3{};
    }
    axis = axis * (1.0f / sine);
    const float angle_sin = sine > 1.0f ? 1.0f : sine;
    const float angle = std::atan2(angle_sin, cosine);
    const float s = std::sin(angle), c = std::cos(angle), t = 1.0f - c;

    Mat3 out;
    out.m[0] = t * axis.x * axis.x + c;
    out.m[1] = t * axis.x * axis.y - s * axis.z;
    out.m[2] = t * axis.x * axis.z + s * axis.y;
    out.m[3] = t * axis.x * axis.y + s * axis.z;
    out.m[4] = t * axis.y * axis.y + c;
    out.m[5] = t * axis.y * axis.z - s * axis.x;
    out.m[6] = t * axis.x * axis.z - s * axis.y;
    out.m[7] = t * axis.y * axis.z + s * axis.x;
    out.m[8] = t * axis.z * axis.z + c;
    return out;
}

// A rig reduced to what retargeting needs: where each joint sits in bind pose
// and who its parent is. Both MCLA's skeleton and the mod's skin become one.
struct Rig {
    std::vector<Vec3> position;
    std::vector<int> parent;
    std::vector<std::vector<int>> children;
    // False for a joint whose bind pose the file never recorded. Such a joint
    // still has a parent so weights on it can be inherited, but it takes no part
    // in the hierarchy the landmarks are read from.
    std::vector<bool> real;

    size_t size() const { return position.size(); }

    void BuildChildren() {
        if (real.size() != position.size()) real.assign(position.size(), true);
        children.assign(position.size(), {});
        for (size_t i = 0; i < parent.size(); ++i) {
            const int up = parent[i];
            if (up >= 0 && static_cast<size_t>(up) < children.size() &&
                up != static_cast<int>(i)) {
                children[static_cast<size_t>(up)].push_back(static_cast<int>(i));
            }
        }
    }

    // A limb tip: a joint with a position worth trusting and nothing real below
    // it. Scaffolding hanging off the end -- the "end" markers rigs leave at the
    // tip of every chain -- does not make its parent an interior joint.
    bool IsTip(int node) const {
        if (node < 0 || !real[static_cast<size_t>(node)]) return false;
        std::vector<int> stack = children[static_cast<size_t>(node)];
        for (int guard = 0; !stack.empty() && guard < 4096; ++guard) {
            const int current = stack.back();
            stack.pop_back();
            if (real[static_cast<size_t>(current)]) return false;
            for (int child : children[static_cast<size_t>(current)]) stack.push_back(child);
        }
        return true;
    }

    bool IsAncestor(int ancestor, int node) const {
        for (int guard = 0; node >= 0 && guard < 256; ++guard) {
            if (node == ancestor) return true;
            const int up = parent[static_cast<size_t>(node)];
            if (up == node) return false;
            node = up;
        }
        return false;
    }

    int Lca(int a, int b) const {
        for (int guard = 0; a >= 0 && guard < 256; ++guard) {
            if (IsAncestor(a, b)) return a;
            const int up = parent[static_cast<size_t>(a)];
            if (up == a) return -1;
            a = up;
        }
        return -1;
    }

    // The joints from `from` down to `to`, `from` first, when `to` sits below it.
    std::vector<int> DownwardPath(int from, int to) const {
        std::vector<int> reversed;
        for (int node = to, guard = 0; node >= 0 && guard < 256; ++guard) {
            reversed.push_back(node);
            if (node == from) {
                std::reverse(reversed.begin(), reversed.end());
                return reversed;
            }
            const int up = parent[static_cast<size_t>(node)];
            if (up == node) break;
            node = up;
        }
        return {};
    }
};

// The joints a humanoid retarget keys off, found from the hierarchy and the
// broad geometry of the bind pose -- which end is up, which side is which --
// and never from a measurement that a change of pose would move.
struct Landmarks {
    int pelvis = -1;       // where the two legs meet
    int chest = -1;        // where the two arms meet
    int head = -1;         // the joint the face hangs off
    int shoulder[2] = {-1, -1};  // [0] = -x side, [1] = +x side
    int wrist[2] = {-1, -1};     // the joint the fingers hang off
    int hip[2] = {-1, -1};
    int foot[2] = {-1, -1};      // the tip of each leg

    bool complete() const {
        return pelvis >= 0 && chest >= 0 && head >= 0 && shoulder[0] >= 0 && shoulder[1] >= 0 &&
               wrist[0] >= 0 && wrist[1] >= 0 && hip[0] >= 0 && hip[1] >= 0 && foot[0] >= 0 &&
               foot[1] >= 0;
    }
};

bool FindLandmarks(const Rig& rig, Landmarks& out) {
    if (rig.size() < 8) return false;

    std::vector<int> leaves;
    for (size_t i = 0; i < rig.size(); ++i)
        if (rig.IsTip(static_cast<int>(i))) leaves.push_back(static_cast<int>(i));
    if (leaves.size() < 5) return false;

    // How far off the midline a joint has to be before it can pass for a limb.
    //
    // Rigs carry locators on the centre line -- a ground node at the feet, a
    // root at the hips -- and a ground node is lower than any foot, so left to
    // itself the search for "the lowest tip on this side" picks the locator and
    // then cannot find where the legs meet. A hand or a foot is off to one side
    // by a good fraction of the body; a locator is off by nothing.
    float tallest = 0.0f, shortest = 0.0f;
    for (size_t i = 0; i < rig.size(); ++i) {
        if (!rig.real[i]) continue;
        tallest = std::max(tallest, rig.position[i].y);
        shortest = std::min(shortest, rig.position[i].y);
    }
    const float midline = std::max(1e-4f, (tallest - shortest) * 0.01f);

    // Feet are the lowest tip on each side, hands the outermost.
    int top = -1;
    for (int side = 0; side < 2; ++side) {
        int foot = -1, hand = -1;
        for (int leaf : leaves) {
            const Vec3& p = rig.position[static_cast<size_t>(leaf)];
            const bool right_side = side == 0 ? p.x < -midline : p.x > midline;
            if (!right_side) continue;
            if (foot < 0 || p.y < rig.position[static_cast<size_t>(foot)].y) foot = leaf;
            if (hand < 0 ||
                std::fabs(p.x) > std::fabs(rig.position[static_cast<size_t>(hand)].x)) {
                hand = leaf;
            }
        }
        if (foot < 0 || hand < 0 || foot == hand) return false;
        out.foot[side] = foot;
        // The hand landmark is only needed to locate the chest and the wrist.
        out.wrist[side] = hand;
    }
    for (int leaf : leaves) {
        if (top < 0 || rig.position[static_cast<size_t>(leaf)].y >
                           rig.position[static_cast<size_t>(top)].y) {
            top = leaf;
        }
    }
    if (top < 0) return false;

    out.pelvis = rig.Lca(out.foot[0], out.foot[1]);
    out.chest = rig.Lca(out.wrist[0], out.wrist[1]);
    if (out.pelvis < 0 || out.chest < 0) return false;

    // The head and each wrist are the last place their chain branches: the face
    // hangs off one joint, the fingers off another.
    auto last_branch = [&](int from, int to) {
        const std::vector<int> path = rig.DownwardPath(from, to);
        int found = -1;
        for (int node : path)
            if (rig.children[static_cast<size_t>(node)].size() >= 2) found = node;
        return found;
    };
    // The head is where the face hangs off -- unless there is no face rig, in
    // which case the last place the chain branches is the chest itself, where
    // the arms leave. Taking that would make the whole upper body "the head".
    out.head = last_branch(out.chest, top);
    if (out.head < 0 || out.head == out.chest) out.head = top;

    for (int side = 0; side < 2; ++side) {
        const int tip = out.wrist[side];
        const int branch = last_branch(out.chest, tip);
        out.wrist[side] = branch >= 0 ? branch : tip;

        const std::vector<int> arm = rig.DownwardPath(out.chest, out.wrist[side]);
        if (arm.size() < 2) return false;
        out.shoulder[side] = arm[1];

        const std::vector<int> leg = rig.DownwardPath(out.pelvis, out.foot[side]);
        if (leg.size() < 2) return false;
        out.hip[side] = leg[1];
    }
    return out.complete();
}

// Pairs the joints of two chains, in order, with both ends pinned: the chains
// start at the same landmark and end at the same one, so a wrist stays on a hand
// and a toe on a toe however differently the two are subdivided.
//
// What the interior joints are chosen to minimise is how much the mesh has to
// stretch. Every joint is placed exactly on the bone it matches, so the spacing
// between two matched bones is the spacing the geometry between the two source
// joints has to live in: pick badly and a limb telescopes into itself. Scoring
// by how far along the chain each joint sits looks equivalent and is not -- the
// driver's neck bone and head bone are fourteen millimetres apart, near enough
// the end of the chain that an exported neck joint measures closest to it, and
// mapping there squeezes seven centimetres of neck into that gap and pushes the
// head down through the shoulders. Comparing segment lengths instead sends the
// same joint one bone lower, where the geometry actually fits.
void MapChain(const Rig& source, const std::vector<int>& source_chain, const Rig& target,
              const std::vector<int>& target_chain, std::vector<int>& mapping) {
    if (source_chain.empty() || target_chain.empty()) return;

    const size_t n = source_chain.size(), m = target_chain.size();
    if (n == 1) {
        mapping[static_cast<size_t>(source_chain[0])] = target_chain[0];
        return;
    }

    auto span = [](const Rig& rig, const std::vector<int>& chain, size_t from, size_t to) {
        return Length(rig.position[static_cast<size_t>(chain[to])] -
                      rig.position[static_cast<size_t>(chain[from])]);
    };
    // Both rigs are already in the same units, so a segment that keeps its
    // length scores zero and the penalty is symmetric between stretch and
    // squeeze. The floor keeps a collapsed pairing expensive but finite, which
    // is what a chain with more joints than bones has to fall back on.
    auto distortion = [](float source_length, float target_length) {
        constexpr float kFloor = 1e-4f;
        return std::fabs(std::log(std::max(target_length, kFloor) /
                                  std::max(source_length, kFloor)));
    };

    constexpr float kUnreachable = 1e30f;
    std::vector<float> cost(n * m, kUnreachable);
    std::vector<size_t> from(n * m, 0);
    cost[0] = 0.0f;  // both chains start together

    for (size_t i = 1; i < n; ++i) {
        const float source_length = span(source, source_chain, i - 1, i);
        for (size_t j = 0; j < m; ++j) {
            for (size_t previous = 0; previous <= j; ++previous) {
                const float before = cost[(i - 1) * m + previous];
                if (before >= kUnreachable) continue;
                const float total =
                    before + distortion(source_length, span(target, target_chain, previous, j));
                if (total < cost[i * m + j]) {
                    cost[i * m + j] = total;
                    from[i * m + j] = previous;
                }
            }
        }
    }

    size_t column = m - 1;
    for (size_t i = n; i-- > 0;) {
        mapping[static_cast<size_t>(source_chain[i])] = target_chain[column];
        column = from[i * m + column];
    }
}

// The spine's lower half runs from the pelvis to the chest, but the two are
// usually siblings rather than one above the other -- a rig routes both through
// a hub at the origin. Walking up through that hub would add a metre-long
// segment that does not exist in either body, so the pelvis is wired straight to
// the first real spine joint instead.
std::vector<int> SpineChain(const Rig& rig, int pelvis, int chest) {
    if (pelvis < 0 || chest < 0) return {};
    const int junction = rig.Lca(pelvis, chest);
    if (junction == pelvis) return rig.DownwardPath(pelvis, chest);

    // With the hub spliced out for having no bind pose of its own, the pelvis
    // and the spine can end up in separate trees with no common ancestor left to
    // find. The chain is the same either way: the pelvis, then the spine from
    // wherever it starts.
    int from = junction;
    if (from < 0) {
        from = chest;
        for (int guard = 0; guard < 256; ++guard) {
            const int up = rig.parent[static_cast<size_t>(from)];
            if (up < 0 || up == from) break;
            from = up;
        }
    }

    const std::vector<int> below = rig.DownwardPath(from, chest);
    if (below.empty()) return {};
    std::vector<int> out{pelvis};
    // A real junction is a joint of its own and already stands for the pelvis
    // here; a tree root reached this way is the first spine joint and stays.
    out.insert(out.end(), junction >= 0 ? below.begin() + 1 : below.begin(), below.end());
    return out;
}

// The whole mapping: every joint of the mod's rig to a bone of the driver's.
bool BuildJointMapping(const Rig& source, const Rig& target, std::vector<int>& mapping,
                       Landmarks& source_marks, Landmarks& target_marks) {
    if (!FindLandmarks(source, source_marks) || !FindLandmarks(target, target_marks)) return false;

    mapping.assign(source.size(), -1);

    MapChain(source, SpineChain(source, source_marks.pelvis, source_marks.chest), target,
             SpineChain(target, target_marks.pelvis, target_marks.chest), mapping);
    MapChain(source, source.DownwardPath(source_marks.chest, source_marks.head), target,
             target.DownwardPath(target_marks.chest, target_marks.head), mapping);
    for (int side = 0; side < 2; ++side) {
        MapChain(source, source.DownwardPath(source_marks.shoulder[side], source_marks.wrist[side]),
                 target, target.DownwardPath(target_marks.shoulder[side], target_marks.wrist[side]),
                 mapping);
        MapChain(source, source.DownwardPath(source_marks.hip[side], source_marks.foot[side]),
                 target, target.DownwardPath(target_marks.hip[side], target_marks.foot[side]),
                 mapping);
    }

    // Fingers ride the hand and the face rides the head. Spreading them over the
    // game's own finger and face bones would be wasted: a driver's hands are on
    // the wheel, and the palette has room for a couple of dozen bones at most,
    // which the limbs need. Collapsing them also keeps a five-fingered rig and a
    // two-fingered one behaving the same.
    for (size_t joint = 0; joint < source.size(); ++joint) {
        for (int side = 0; side < 2; ++side) {
            if (static_cast<int>(joint) != source_marks.wrist[side] &&
                source.IsAncestor(source_marks.wrist[side], static_cast<int>(joint))) {
                mapping[joint] = target_marks.wrist[side];
            }
        }
        // The arms leave from below the head on some rigs, so they are held out
        // of it explicitly: sweeping everything under the head onto one bone
        // would otherwise take both of them with it.
        const bool in_arm =
            source.IsAncestor(source_marks.shoulder[0], static_cast<int>(joint)) ||
            source.IsAncestor(source_marks.shoulder[1], static_cast<int>(joint));
        if (!in_arm && static_cast<int>(joint) != source_marks.head &&
            source.IsAncestor(source_marks.head, static_cast<int>(joint))) {
            mapping[joint] = target_marks.head;
        }
    }

    // Anything left over goes to whichever mapped joint it sits nearest.
    //
    // These are the joints the chains never reached: transform nodes above the
    // pelvis, and -- the ones that matter -- the twist and roll helpers a rig
    // hangs off a limb's parent as siblings rather than stringing into the
    // chain. A thigh helper is a sibling of the knee, so walking up the
    // hierarchy for an answer lands it on the pelvis and leaves the thigh rigid
    // while the knee bends. Distance is measured inside one rig here, between a
    // joint and its own neighbours, so the pose difference that rules distance
    // out for matching the two rigs does not apply.
    for (size_t joint = 0; joint < source.size(); ++joint) {
        if (mapping[joint] >= 0) continue;

        // A joint with no bind pose and no real joint above it is a rig's ground
        // or origin node. It stands nowhere in particular -- at the model origin
        // by default, which is under the feet -- so distance would hand whatever
        // hangs off it to an ankle. The pelvis is the honest answer.
        if (!source.real[joint] && source.parent[joint] < 0) {
            mapping[joint] = target_marks.pelvis;
            continue;
        }

        int best = -1;
        float best_distance = 0.0f;
        for (size_t other = 0; other < source.size(); ++other) {
            if (mapping[other] < 0 || !source.real[other]) continue;
            const Vec3 delta = source.position[other] - source.position[joint];
            const float distance = Dot(delta, delta);
            if (best < 0 || distance < best_distance) {
                best_distance = distance;
                best = static_cast<int>(other);
            }
        }
        mapping[joint] =
            best >= 0 ? mapping[static_cast<size_t>(best)] : target_marks.pelvis;
    }
    return true;
}

// Spreads each vertex's influences over its neighbours, a few rings deep.
//
// This is what stops a retargeted character coming apart, and the reason is in
// how the models being fed to it are weighted. A GTA-era character is RIGIDLY
// weighted: every vertex is 1.0 on a single bone and 0 on all the rest. That is
// fine while the rig it was built for is the rig it is played on, because two
// neighbouring vertices on either side of a joint move with two bones that were
// never going to disagree by much.
//
// The repose is not that. It hands every joint its own rigid transform, built
// from a skeleton with different bone lengths and a different bind pose, and two
// adjacent vertices bound hard to different joints then get two unrelated
// transforms. The edge between them is torn: measured on the driver mod, edges
// were coming out seven and a half times their own length, and a torn strip of
// triangles stretched across half a body is exactly the flat blade that was
// hanging off that character's back.
//
// Smoothing gives the vertices at a seam a share of both bones, so the seam
// bends instead of ripping. Away from a seam every neighbour already carries the
// same influences and this changes nothing, so it is only ever paid for where it
// is needed. On the same model it takes the worst edge from 7.6x to 2.1x.
//
// The averaging runs over WELDED positions, not over the vertex array. A model
// carries a duplicate vertex on each side of every UV and material seam -- 457
// of that character's 1669 -- and those duplicates are not neighbours in the
// triangle list, so smoothing them independently gives two vertices standing in
// the same place two different answers and opens a crack along every seam. They
// are one point on the surface and they have to be smoothed as one.
void SmoothSkinWeights(Mesh& mesh, int rounds) {
    if (rounds <= 0 || !mesh.skinned() || mesh.skin.size() != mesh.vertices.size()) return;

    const size_t count = mesh.vertices.size();

    // Weld: everything standing in the same place, to a hundredth of a
    // millimetre, is one point.
    std::map<std::array<int64_t, 3>, uint32_t> lookup;
    std::vector<uint32_t> node(count, 0);
    for (size_t i = 0; i < count; ++i) {
        const MeshVertex& vertex = mesh.vertices[i];
        const std::array<int64_t, 3> key = {
            static_cast<int64_t>(std::llround(vertex.px * 100000.0)),
            static_cast<int64_t>(std::llround(vertex.py * 100000.0)),
            static_cast<int64_t>(std::llround(vertex.pz * 100000.0))};
        auto [entry, inserted] = lookup.emplace(key, static_cast<uint32_t>(lookup.size()));
        node[i] = entry->second;
    }
    const size_t nodes = lookup.size();

    std::vector<std::vector<uint32_t>> neighbours(nodes);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                if (a == b) continue;
                const uint32_t from = node[mesh.indices[i + a]];
                const uint32_t to = node[mesh.indices[i + b]];
                if (from != to) neighbours[from].push_back(to);
            }
        }
    }
    for (auto& list : neighbours) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    std::vector<std::map<uint16_t, float>> weights(nodes);
    for (size_t i = 0; i < count; ++i) {
        for (int c = 0; c < 4; ++c) {
            if (mesh.skin[i].weight[c] > 0.0f)
                weights[node[i]][mesh.skin[i].joint[c]] += mesh.skin[i].weight[c];
        }
    }
    auto normalise = [](std::map<uint16_t, float>& entry) {
        float total = 0.0f;
        for (const auto& [joint, weight] : entry) total += weight;
        if (total <= 0.0f) return;
        for (auto& [joint, weight] : entry) weight /= total;
    };
    for (auto& entry : weights) normalise(entry);

    for (int round = 0; round < rounds; ++round) {
        std::vector<std::map<uint16_t, float>> next(nodes);
        for (size_t n = 0; n < nodes; ++n) {
            // Half its own, half the average of what surrounds it.
            for (const auto& [joint, weight] : weights[n]) next[n][joint] += weight * 0.5f;
            if (!neighbours[n].empty()) {
                const float share = 0.5f / static_cast<float>(neighbours[n].size());
                for (uint32_t other : neighbours[n]) {
                    for (const auto& [joint, weight] : weights[other])
                        next[n][joint] += weight * share;
                }
            }
            normalise(next[n]);
            if (next[n].empty()) next[n] = weights[n];
        }
        weights.swap(next);
    }

    // Back onto the vertices, keeping the three influences the game's vertex
    // format has room for -- the same three the writer would have kept anyway,
    // so the pose baked into the positions and the weights shipped beside them
    // are built from one set of numbers rather than two.
    for (size_t i = 0; i < count; ++i) {
        std::vector<std::pair<float, uint16_t>> ranked;
        ranked.reserve(weights[node[i]].size());
        for (const auto& [joint, weight] : weights[node[i]]) ranked.emplace_back(weight, joint);
        if (ranked.empty()) continue;
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        if (ranked.size() > 3) ranked.resize(3);

        float total = 0.0f;
        for (const auto& [weight, joint] : ranked) total += weight;
        if (total <= 0.0f) continue;

        MeshSkin& skin = mesh.skin[i];
        skin = MeshSkin{};
        for (size_t c = 0; c < ranked.size(); ++c) {
            skin.joint[c] = ranked[c].second;
            skin.weight[c] = ranked[c].first / total;
        }
    }
}

// Carries the mesh out of its own bind pose and into the driver's.
//
// Skinning only ever applies the delta between a bone's bind pose and where it
// is now, so a mesh authored in a T-pose stays in a T-pose however well its
// weights are matched -- the arms sit where the source rig left them and merely
// twitch as the driver's arms move. That is the raised-arms look.
//
// Each joint gets a rigid transform that rotates its segment onto the direction
// the matching pair of bones points in, accumulated down the chain, and lands it
// on the bone's own bind position. Deriving the rotation from segment directions
// rather than from the source's inverse bind matrices matters: the two rigs do
// not agree on which local axis runs down a bone, and using their local frames
// directly scatters the mesh.
//
// Joints that map to the same bone as their parent -- fingers on a hand, face
// joints on a head -- have no direction to align, so they keep their own offset
// and simply ride the parent's rotation.
void ReposeMesh(Mesh& mesh, const Rig& source, const Rig& target,
                const std::vector<int>& mapping, const Landmarks& source_marks,
                float proportions, bool anchor_bones) {
    const size_t joints = source.size();

    // How short a bone has to be before its direction stops meaning anything.
    //
    // A joint's rotation is read off the segment that leaves it, and that is
    // only an honest reading when the segment is long enough to have a
    // direction. The driver's pelvis and its first spine bone are 2 cm apart; a
    // model's are 2.8 cm apart and tilted back, and lining one up with the other
    // asks for a 42 degree turn -- which is then applied to every vertex the
    // pelvis drives, i.e. the whole hip. Two centimetres of nub cannot say which
    // way a hip faces. A joint whose segment is below this keeps the rotation it
    // inherited, which is the honest answer: nothing was measured.
    float rig_height = 0.0f;
    {
        float low = 0.0f, high = 0.0f;
        bool first = true;
        for (size_t i = 0; i < joints; ++i) {
            if (!source.real[i]) continue;
            const float y = source.position[i].y;
            if (first) { low = high = y; first = false; continue; }
            low = std::min(low, y);
            high = std::max(high, y);
        }
        rig_height = high - low;
    }
    const float shortest_meaningful = std::max(1e-6f, rig_height * 0.02f);

    // The repose is anchored at the pelvis and spreads outward along the chains,
    // not along the raw hierarchy: the hub joints a rig keeps at the origin sit
    // between the pelvis and the spine and would drag everything to the floor.
    std::vector<int> effective(joints, -2);  // -2 unvisited, -1 root of the walk
    effective[static_cast<size_t>(source_marks.pelvis)] = -1;

    // A joint's rotation belongs to the bone that LEAVES it -- an upper arm runs
    // from the shoulder to the elbow, so the shoulder is what has to turn to
    // bring it down. Taking the incoming segment instead leaves every limb
    // rotated one joint late: the shoulder keeps the clavicle's sideways
    // direction and the arm ends up out and then bent, hands on hips.
    std::vector<int> primary(joints, -1);

    auto wire = [&](const std::vector<int>& chain) {
        for (size_t i = 1; i < chain.size(); ++i) {
            effective[static_cast<size_t>(chain[i])] = chain[i - 1];
            // First chain to claim a joint wins, so a branch point follows the
            // trunk: the chest turns with the neck, not with an arm.
            if (primary[static_cast<size_t>(chain[i - 1])] < 0)
                primary[static_cast<size_t>(chain[i - 1])] = chain[i];
        }
    };
    wire(SpineChain(source, source_marks.pelvis, source_marks.chest));
    wire(source.DownwardPath(source_marks.chest, source_marks.head));
    for (int side = 0; side < 2; ++side) {
        wire(source.DownwardPath(source_marks.shoulder[side], source_marks.wrist[side]));
        wire(source.DownwardPath(source_marks.hip[side], source_marks.foot[side]));
        effective[static_cast<size_t>(source_marks.shoulder[side])] = source_marks.chest;
        effective[static_cast<size_t>(source_marks.hip[side])] = source_marks.pelvis;
    }
    for (size_t joint = 0; joint < joints; ++joint) {
        if (effective[joint] == -2) effective[joint] = source.parent[joint];
        if (primary[joint] < 0 && !source.children[joint].empty())
            primary[joint] = source.children[joint].front();
    }

    // Resolve every joint's transform, parents before children.
    std::vector<Mat3> rotation(joints);
    std::vector<Vec3> placed(joints);
    std::vector<bool> done(joints, false);

    std::function<void(int)> resolve = [&](int joint) {
        if (joint < 0 || done[static_cast<size_t>(joint)]) return;
        done[static_cast<size_t>(joint)] = true;

        const int up = effective[static_cast<size_t>(joint)];
        const int bone = mapping[static_cast<size_t>(joint)];

        // Where the joint sits: carried along by the bone above it, and landing
        // exactly on its own bone whenever it has one of its own. Landing
        // exactly is the point -- the game's skinning undoes the bone's bind
        // transform, so anything else shifts the mesh off the character.
        Mat3 inherited;
        if (up < 0 || up == joint) {
            placed[static_cast<size_t>(joint)] =
                (anchor_bones && bone >= 0) ? target.position[static_cast<size_t>(bone)]
                                            : source.position[static_cast<size_t>(joint)];
        } else {
            resolve(up);
            inherited = rotation[static_cast<size_t>(up)];

            const Vec3 segment = source.position[static_cast<size_t>(joint)] -
                                 source.position[static_cast<size_t>(up)];
            const int parent_bone = mapping[static_cast<size_t>(up)];
            if (anchor_bones && bone >= 0 && parent_bone >= 0 && bone != parent_bone) {
                // The direction always comes from the skeleton; how far along it
                // the joint lands is a choice between the skeleton's spacing and
                // the model's own.
                const Vec3 wanted = target.position[static_cast<size_t>(bone)] -
                                    target.position[static_cast<size_t>(parent_bone)];
                const float wanted_length = Length(wanted);
                const float own_length = Length(segment);
                if (wanted_length > 1e-6f && proportions > 0.0f && own_length > 1e-6f) {
                    const float blended =
                        wanted_length * (1.0f - proportions) + own_length * proportions;
                    placed[static_cast<size_t>(joint)] =
                        placed[static_cast<size_t>(up)] + wanted * (blended / wanted_length);
                } else {
                    placed[static_cast<size_t>(joint)] = placed[static_cast<size_t>(up)] + wanted;
                }
            } else {
                placed[static_cast<size_t>(joint)] =
                    placed[static_cast<size_t>(up)] + inherited.Apply(segment);
            }
        }

        // How the joint turns: from the bone that leaves it, towards where the
        // matching pair of target bones points. A joint with no bone of its own
        // to head towards -- a fingertip, a face joint -- rides the rotation it
        // inherited, which is what keeps a hand rigid on its forearm.
        rotation[static_cast<size_t>(joint)] = inherited;
        const int child = primary[static_cast<size_t>(joint)];
        if (child < 0 || bone < 0) return;
        const int child_bone = mapping[static_cast<size_t>(child)];
        if (child_bone < 0 || child_bone == bone) return;

        const Vec3 turned = inherited.Apply(source.position[static_cast<size_t>(child)] -
                                            source.position[static_cast<size_t>(joint)]);
        const Vec3 wanted = target.position[static_cast<size_t>(child_bone)] -
                            target.position[static_cast<size_t>(bone)];
        const float turned_length = Length(turned);
        const float wanted_length = Length(wanted);
        if (turned_length < shortest_meaningful || wanted_length < shortest_meaningful) return;
        if (turned_length > 1e-6f && wanted_length > 1e-6f) {
            rotation[static_cast<size_t>(joint)] =
                RotationBetween(turned * (1.0f / turned_length),
                                wanted * (1.0f / wanted_length)) *
                inherited;
        }
    };
    for (size_t joint = 0; joint < joints; ++joint) resolve(static_cast<int>(joint));

    for (size_t v = 0; v < mesh.vertices.size(); ++v) {
        MeshVertex& vertex = mesh.vertices[v];
        const MeshSkin& skin = mesh.skin[v];
        const Vec3 position{vertex.px, vertex.py, vertex.pz};
        const Vec3 normal{vertex.nx, vertex.ny, vertex.nz};

        Vec3 accumulated{0, 0, 0}, accumulated_normal{0, 0, 0};
        float total = 0.0f;
        for (int c = 0; c < 4; ++c) {
            const uint16_t joint = skin.joint[c];
            const float weight = skin.weight[c];
            if (weight <= 0.0f || joint >= joints) continue;

            const Mat3& rotate = rotation[joint];
            const Vec3 local = position - source.position[joint];
            accumulated = accumulated + (rotate.Apply(local) + placed[joint]) * weight;
            accumulated_normal = accumulated_normal + rotate.Apply(normal) * weight;
            total += weight;
        }
        if (total <= 0.0f) continue;

        const Vec3 result = accumulated * (1.0f / total);
        vertex.px = result.x;
        vertex.py = result.y;
        vertex.pz = result.z;

        const float length = Length(accumulated_normal);
        if (length > 1e-6f) {
            vertex.nx = accumulated_normal.x / length;
            vertex.ny = accumulated_normal.y / length;
            vertex.nz = accumulated_normal.z / length;
        }
    }
}

// Ambient occlusion baked from the MOD'S OWN geometry.
//
// What this replaces, and why neither of the other two answers works:
//
//   uniform_shade alone floods one level over the whole mesh, so the lane's
//   deviation is ZERO. Measured on three shipped wheels the lane runs 0..252
//   with a deviation near fifty -- whl_am_bbs_ch 0..252 mean 155 dev 52,
//   whl_am_amer_razor 5..254 mean 186 dev 45, whl_am_5zigen_5zr 0..249 mean 144
//   dev 55. A flat lane throws all of that away, and what is left lighting the
//   wheel is the material's specular, which is pinned to the geometry and
//   therefore sweeps round as the wheel turns. That is the "the normals change
//   with rotation" report, and it is not the normals.
//
//   inherit_shade carries the template's lane across by proximity instead. It
//   does not merely look wrong, it lights the wheel NEON: the lane's RGB is
//   0x000000 on every shipped wheel measured, so nothing is carried in the
//   colour -- what moves is the UV1 band beside it, and a band is a material
//   selector (RimMaterialA..E in the car config). Scattering bands across a
//   shape that does not share them lands some vertices on the emissive one.
//
// So the lane is computed rather than copied or invented: for each vertex,
// cast rays into the hemisphere its normal faces and count how many are stopped
// by the mesh itself within `kOcclusionReach`. A vertex down between two spokes
// is blocked and comes out dark; one on the face of the rim is open and comes
// out bright. It describes THIS wheel's cavities, which is the one thing the
// other two answers cannot do.
//
// Validated against the shipped distribution before being written: the BMW's
// rim comes out 0..255 mean 156 deviation 77 against a target of mean 155
// deviation 52, so the mean lands on its own and only the contrast needs
// pulling in -- which is what kOcclusionContrast does.
constexpr float kOcclusionReach = 0.25f;   // how far a cavity can occlude
constexpr int kOcclusionRays = 12;
// Measured 77 against the shipped 52. Raw occlusion is more contrasty than a
// baked lane because a baked one carries bounce light this does not.
constexpr float kOcclusionContrast = 52.0f / 77.0f;

void BakeOcclusion(const Mesh& mesh, std::vector<uint8_t>& out) {
    out.assign(mesh.vertices.size(), 255);
    if (mesh.indices.size() < 3 || mesh.vertices.empty()) return;

    float low[3], high[3];
    mesh.Bounds(low, high);
    const float cell = kOcclusionReach * 0.5f;
    if (cell <= 0.0f) return;

    // A uniform grid over the triangles, so a ray only tests what is near it.
    auto cell_of = [&](const float* p, int* c) {
        for (int i = 0; i < 3; ++i) {
            c[i] = static_cast<int>(std::floor((p[i] - low[i]) / cell));
        }
    };
    std::map<std::array<int, 3>, std::vector<uint32_t>> grid;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        std::array<int, 3> seen[3];
        for (int k = 0; k < 3; ++k) {
            const MeshVertex& v = mesh.vertices[mesh.indices[t + k]];
            const float p[3] = {v.px, v.py, v.pz};
            int c[3];
            cell_of(p, c);
            seen[k] = {c[0], c[1], c[2]};
        }
        for (int k = 0; k < 3; ++k) {
            if (k && seen[k] == seen[0]) continue;
            if (k == 2 && seen[2] == seen[1]) continue;
            grid[seen[k]].push_back(static_cast<uint32_t>(t));
        }
    }

    // Moller-Trumbore, the usual one.
    auto blocked = [&](const float* origin, const float* direction) {
        const int steps = static_cast<int>(kOcclusionReach / cell) + 2;
        for (int s = 0; s < steps; ++s) {
            const float p[3] = {origin[0] + direction[0] * s * cell,
                                origin[1] + direction[1] * s * cell,
                                origin[2] + direction[2] * s * cell};
            int c[3];
            cell_of(p, c);
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        const auto found =
                            grid.find({c[0] + dx, c[1] + dy, c[2] + dz});
                        if (found == grid.end()) continue;
                        for (uint32_t t : found->second) {
                            const MeshVertex& a = mesh.vertices[mesh.indices[t]];
                            const MeshVertex& b = mesh.vertices[mesh.indices[t + 1]];
                            const MeshVertex& cc = mesh.vertices[mesh.indices[t + 2]];
                            const Vec3 e1{b.px - a.px, b.py - a.py, b.pz - a.pz};
                            const Vec3 e2{cc.px - a.px, cc.py - a.py, cc.pz - a.pz};
                            const Vec3 ray{direction[0], direction[1], direction[2]};
                            const Vec3 h = Cross(ray, e2);
                            const float det = Dot(e1, h);
                            if (std::fabs(det) < 1e-9f) continue;
                            const float inverse = 1.0f / det;
                            const Vec3 sv{origin[0] - a.px, origin[1] - a.py,
                                          origin[2] - a.pz};
                            const float u = inverse * Dot(sv, h);
                            if (u < 0.0f || u > 1.0f) continue;
                            const Vec3 q = Cross(sv, e1);
                            const float v = inverse * Dot(ray, q);
                            if (v < 0.0f || u + v > 1.0f) continue;
                            const float distance = inverse * Dot(e2, q);
                            if (distance > 1e-4f && distance < kOcclusionReach) return true;
                        }
                    }
                }
            }
        }
        return false;
    };

    // A fixed set of directions rather than a random one, so two runs of the
    // same build produce the same lane and a difference in game is a difference
    // in the mod.
    uint32_t seed = 0x9E3779B9u;
    auto next = [&]() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return static_cast<float>(seed & 0xFFFFFFu) / float(0xFFFFFF) * 2.0f - 1.0f;
    };

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const MeshVertex& vertex = mesh.vertices[i];
        Vec3 normal{vertex.nx, vertex.ny, vertex.nz};
        const float length = Length(normal);
        if (length < 1e-6f) continue;
        normal = normal * (1.0f / length);
        const float origin[3] = {vertex.px + normal.x * 1e-3f,
                                 vertex.py + normal.y * 1e-3f,
                                 vertex.pz + normal.z * 1e-3f};

        int hits = 0;
        for (int r = 0; r < kOcclusionRays; ++r) {
            Vec3 direction{next(), next(), next()};
            const float size = Length(direction);
            if (size < 1e-6f) continue;
            direction = direction * (1.0f / size);
            if (Dot(direction, normal) < 0.0f) direction = direction * -1.0f;
            const float d[3] = {direction.x, direction.y, direction.z};
            if (blocked(origin, d)) ++hits;
        }
        const float open = 1.0f - static_cast<float>(hits) / kOcclusionRays;
        out[i] = static_cast<uint8_t>(std::clamp(open * 255.0f, 0.0f, 255.0f));
    }
}

// Tangents that agree with the UVs.
//
// The vertex format carries one, and the character shaders are all normal-map
// shaders, so it is the basis a normal map is read in. Filling it with any old
// vector perpendicular to the normal -- which is what standing in for a real
// tangent amounts to -- gives every vertex an unrelated basis and turns a normal
// map into noise, which the light then picks out as a hard sheen.
void ComputeTangents(const Mesh& mesh, std::vector<Vec3>& out) {
    out.assign(mesh.vertices.size(), Vec3{0, 0, 0});
    const size_t count = mesh.vertices.size();

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        if (a >= count || b >= count || c >= count) continue;

        const MeshVertex& va = mesh.vertices[a];
        const MeshVertex& vb = mesh.vertices[b];
        const MeshVertex& vc = mesh.vertices[c];

        const Vec3 edge1{vb.px - va.px, vb.py - va.py, vb.pz - va.pz};
        const Vec3 edge2{vc.px - va.px, vc.py - va.py, vc.pz - va.pz};
        const float s1 = vb.u - va.u, t1 = vb.v - va.v;
        const float s2 = vc.u - va.u, t2 = vc.v - va.v;

        const float determinant = s1 * t2 - s2 * t1;
        if (std::fabs(determinant) < 1e-12f) continue;
        const float inverse = 1.0f / determinant;

        const Vec3 tangent{(t2 * edge1.x - t1 * edge2.x) * inverse,
                           (t2 * edge1.y - t1 * edge2.y) * inverse,
                           (t2 * edge1.z - t1 * edge2.z) * inverse};
        out[a] = out[a] + tangent;
        out[b] = out[b] + tangent;
        out[c] = out[c] + tangent;
    }

    for (size_t i = 0; i < count; ++i) {
        const MeshVertex& vertex = mesh.vertices[i];
        const Vec3 normal{vertex.nx, vertex.ny, vertex.nz};
        // Gram-Schmidt: the tangent has to lie in the surface.
        Vec3 tangent = out[i] - normal * Dot(normal, out[i]);
        float length = Length(tangent);
        if (length < 1e-6f) {
            // A vertex no triangle gave a usable UV gradient to. Any vector in
            // the surface will do; it is only reached where the mapping is
            // degenerate anyway.
            const Vec3 axis = std::fabs(normal.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            tangent = Cross(normal, axis);
            length = Length(tangent);
            if (length < 1e-6f) { out[i] = Vec3{1, 0, 0}; continue; }
        }
        out[i] = tangent * (1.0f / length);
    }
}

Rig RigFromSkeleton(const std::vector<BoneTransform>& bones,
                    const std::vector<uint32_t>& parents) {
    Rig rig;
    rig.position.resize(bones.size());
    rig.parent.resize(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        rig.position[i] = Vec3{bones[i].t[0], bones[i].t[1], bones[i].t[2]};
        const uint32_t up = parents[i];
        rig.parent[i] = (up == i || up >= bones.size()) ? -1 : static_cast<int>(up);
    }
    rig.BuildChildren();
    return rig;
}

// A joint whose inverse bind matrix has no translation in it sits at the model
// origin, which for a standing character is on the floor between the feet. Some
// are scaffolding -- ground and root nodes, "end" markers at the tip of a chain,
// twist helpers -- and some are not: a rig exported from Source names its pelvis
// and anchors it at the origin like everything else, so the joint is perfectly
// real and only its POSITION is missing.
//
// Which is why these are no longer cut out of the hierarchy. Removing the pelvis
// takes the only path between the two legs with it, and a retarget that starts
// by asking where the legs meet gets no answer at all -- the model then falls
// back to riding one bone, looking for all the world like it was never rigged.
// They stay in the tree; what is dropped is trust in where they say they are.
//
// Rigs are full of them -- the stack of root and anchor nodes above the pelvis,
// "end" markers at the tip of a chain, twist helpers -- and they are not only at
// the top: one character's hip joint is blank while the thigh joints hanging off
// it are real, so a leg chain read straight from the hierarchy starts at the
// origin and runs a metre and a half to the knee. That is what turns a torso
// into a thread and stretches a neck across the whole chest.
//
// So they are spliced out: each keeps its nearest real ancestor's position, and
// the hierarchy is rewired around them. Weights that land on one then behave as
// though they had landed on that ancestor, which is the only reading of a joint
// with no bind pose that means anything.
bool IsBlankBind(const float* matrix) {
    constexpr float kTolerance = 1e-6f;  // exporters write these as exact zeros
    return std::fabs(matrix[12]) < kTolerance && std::fabs(matrix[13]) < kTolerance &&
           std::fabs(matrix[14]) < kTolerance;
}

Rig RigFromMesh(const Mesh& mesh) {
    Rig rig;
    const size_t joints = mesh.joint_count();
    rig.position.resize(joints);
    rig.parent.assign(joints, -1);

    const bool have_binds = mesh.joint_inverse_bind.size() >= joints * 16;
    std::vector<bool> real(joints, true);
    for (size_t i = 0; i < joints; ++i) {
        if (have_binds && IsBlankBind(&mesh.joint_inverse_bind[i * 16])) real[i] = false;
        rig.position[i] = Vec3{mesh.joint_bind[i * 3], mesh.joint_bind[i * 3 + 1],
                               mesh.joint_bind[i * 3 + 2]};
        if (i < mesh.joint_parent.size()) rig.parent[i] = mesh.joint_parent[i];
    }

    rig.real = real;
    rig.BuildChildren();

    // Give the untrusted joints somewhere sensible to be: the middle of what
    // hangs off them. A pelvis recorded at the origin lands back between the two
    // hips and the spine, which is where it belongs and what every measurement
    // after this wants. Deepest first, so a run of them resolves inwards from
    // the real joints at the ends.
    std::vector<int> stack, post;
    for (size_t i = 0; i < joints; ++i)
        if (rig.parent[i] < 0) stack.push_back(static_cast<int>(i));
    while (!stack.empty()) {
        const int node = stack.back();
        stack.pop_back();
        post.push_back(node);
        for (int child : rig.children[static_cast<size_t>(node)]) stack.push_back(child);
    }
    for (auto it = post.rbegin(); it != post.rend(); ++it) {
        const size_t node = static_cast<size_t>(*it);
        if (real[node]) continue;
        Vec3 sum{0, 0, 0};
        int taps = 0;
        for (int child : rig.children[node]) {
            // An "end" marker with nothing under it never got a position either.
            if (!real[static_cast<size_t>(child)] &&
                rig.children[static_cast<size_t>(child)].empty()) {
                continue;
            }
            sum = sum + rig.position[static_cast<size_t>(child)];
            ++taps;
        }
        if (taps > 0) {
            rig.position[node] = sum * (1.0f / static_cast<float>(taps));
        } else if (rig.parent[node] >= 0) {
            rig.position[node] = rig.position[static_cast<size_t>(rig.parent[node])];
        }
    }

    // String limb helpers back into the limb they belong to.
    //
    // A rig exported from an animation package often hangs a limb's twist and
    // roll joints off the limb's PARENT rather than off each other: hip twist,
    // thigh, thigh again and knee all arrive as siblings of one another, four
    // children of one hip. Read as a hierarchy that says the thigh is not part
    // of the leg, so the leg chain starts at the knee and everything above it
    // falls back to the pelvis -- a thigh that stays rigid while the knee bends.
    //
    // A sibling is treated as a continuation rather than a branch when it lies
    // further out than the other, nearer to it than to the shared parent, and
    // roughly along the same line. That last test is what separates the two
    // cases: a thigh continues into a knee at fifteen degrees, while a neck
    // leaves a chest forty-five degrees away from where a shoulder does.
    constexpr float kCollinear = 0.85f;  // about thirty degrees
    std::vector<int> rewired = rig.parent;
    for (size_t child = 0; child < joints; ++child) {
        if (!real[child]) continue;
        const int up = rig.parent[child];
        if (up < 0) continue;

        const Vec3 from_parent = rig.position[child] - rig.position[static_cast<size_t>(up)];
        const float reach = Length(from_parent);
        if (reach < 1e-6f) continue;

        int best = -1;
        float best_gap = 0.0f;
        for (int sibling : rig.children[static_cast<size_t>(up)]) {
            if (sibling == static_cast<int>(child)) continue;
            const Vec3 out = rig.position[static_cast<size_t>(sibling)] -
                             rig.position[static_cast<size_t>(up)];
            const float along = Length(out);
            if (along >= reach || along < 1e-6f) continue;

            const Vec3 rest = rig.position[child] - rig.position[static_cast<size_t>(sibling)];
            const float gap = Length(rest);
            if (gap >= reach || gap < 1e-6f) continue;
            if (Dot(out * (1.0f / along), rest * (1.0f / gap)) < kCollinear) continue;

            if (best < 0 || gap < best_gap) {
                best_gap = gap;
                best = sibling;
            }
        }
        // Every candidate is strictly nearer the parent than the joint it
        // adopts, so this cannot close a loop.
        if (best >= 0) rewired[child] = best;
    }
    rig.parent = std::move(rewired);
    rig.BuildChildren();
    return rig;
}

}  // namespace

bool ParseRsc5Header(const std::vector<uint8_t>& file, Rsc5Resource& out,
                     size_t& payload_offset, size_t& payload_size, std::string& error) {
    if (file.size() < 20) {
        error = "file too small for an RSC5 header";
        return false;
    }
    if (LoadBE32(file.data()) != kRsc5Magic) {
        error = "not an RSC5 resource";
        return false;
    }
    if (LoadBE32(file.data() + 12) != kXCompressMagic) {
        error = "missing XCompress marker";
        return false;
    }

    out.type = LoadBE32(file.data() + 4);
    out.flag = LoadBE32(file.data() + 8);
    out.virtual_size = (out.flag & 0x7FFu) << (((out.flag >> 11) & 0xFu) + 8);
    out.physical_size = ((out.flag >> 15) & 0x7FFu) << (((out.flag >> 26) & 0xFu) + 8);

    payload_offset = 20;
    payload_size = LoadBE32(file.data() + 16);
    if (payload_offset + payload_size > file.size()) payload_size = file.size() - payload_offset;

    if (out.virtual_size == 0) {
        error = "resource has no virtual segment";
        return false;
    }
    return true;
}

bool BuildRsc5File(Rsc5Resource& resource, std::vector<uint8_t>& out_file,
                   uint32_t& out_flag, std::string& error) {
    size_t expected = static_cast<size_t>(resource.virtual_size) + resource.physical_size;
    if (resource.data.size() != expected) {
        error = "resource payload does not match its segment sizes";
        return false;
    }

    // There is no upper budget on how long the file may be, and the padding
    // that used to be added here to respect one was doing real harm.
    //
    // Read from the binary rather than assumed. sub_821E5E58 asks the streamer
    // for `offset = 0xC` plus the destination chunk list, whose sizes sum to
    // virtual + physical, so the request lands EXACTLY on the ceiling
    // sub_821BCB10 enforces (`virtual + physical + 12`) and can never exceed it.
    // The read loop in sub_821BC140 then pulls `min(0x8000, entry size)` at a
    // time until the chunks are full, consulting the file's own length: a file
    // longer than it needs simply has a tail nobody reads.
    //
    // The one real constraint is the other direction. If the stream runs out
    // before the chunks are full, `v47` becomes zero, the read returns zero,
    // which is not treated as a short read, and the loop asks for zero bytes
    // forever. So a segment must never be declared larger than the bytes the
    // stream actually decodes to -- which is why the padding is gone: it grew
    // the declared size, changed the page class the resource had always used,
    // and bought nothing.

    // One LZX frame carries at most 32768 bytes, and a STORED frame is the one
    // shape the console's streaming decoder will not split.
    //
    // Traced in the game (the inflate probe): on the first call the decoder is
    // handed 32748 bytes -- a 32768-byte read less the twelve it starts past --
    // eats all of them and emits nothing, because a stored frame of 32768 costs
    // 32786 and it wants the whole frame before it writes a byte. On the refill
    // it finishes that frame, fills the first output chunk, and swallows the
    // next frame too. Then the output has room, the file has no bytes left, and
    // the read loop only refills when the buffer empties: it asks for zero
    // bytes forever. One frame survives this because the refill completes it;
    // two never do.
    //
    // So anything past a single frame goes down the streamer's other road.
    constexpr size_t kLzxFrame = 32768;
    const bool one_frame = resource.data.size() <= kLzxFrame;

    out_file.clear();
    if (one_frame) {
        const std::vector<uint8_t> stream =
            LzxEncodeStored(resource.data.data(), resource.data.size());
        out_flag = resource.flag | 0x40000000u;
        out_file.resize(20);
        StoreBE32(out_file.data() + 0, kRsc5Magic);
        StoreBE32(out_file.data() + 4, resource.type);
        StoreBE32(out_file.data() + 8, out_flag);
        StoreBE32(out_file.data() + 12, kXCompressMagic);
        StoreBE32(out_file.data() + 16, static_cast<uint32_t>(stream.size()));
        out_file.insert(out_file.end(), stream.begin(), stream.end());
        return true;
    }

    // The uncompressed road, read straight out of sub_821BC140: with bit 30
    // clear it never touches the inflater and reads each destination chunk
    // directly, one after another, starting at the request offset of twelve
    // that sub_821E5E58 passes. The compressed branch is the one that subtracts
    // those twelve again; this one does not. So the payload sits at offset
    // TWELVE, not twenty, and the file is exactly 12 + virtual + physical --
    // which is also exactly the ceiling sub_821BCB10 enforces for an
    // uncompressed entry, `offset + requested <= entry size`.
    //
    // Getting that length wrong is why this road looked impassable before: a
    // 20-byte header puts the payload eight bytes late AND leaves the entry
    // eight bytes short of the ceiling, so the request is refused and the car
    // loads forever with nothing logged.
    out_flag = (resource.flag | 0x80000000u) & ~0x40000000u;
    out_file.resize(12);
    StoreBE32(out_file.data() + 0, kRsc5Magic);
    StoreBE32(out_file.data() + 4, resource.type);
    StoreBE32(out_file.data() + 8, out_flag);
    out_file.insert(out_file.end(), resource.data.begin(), resource.data.end());
    return true;
}

bool EncodeSegmentSize(uint32_t size, uint32_t& mantissa, uint32_t& shift,
                       uint32_t preferred_shift) {
    auto try_shift = [&](uint32_t candidate) {
        if (candidate < 3 || candidate > 15) return false;
        const uint32_t unit = 1u << (candidate + 8);
        const uint32_t count = (size + unit - 1) / unit;
        if (count > 0x7FFu) return false;
        mantissa = count;
        shift = candidate;
        return true;
    };

    // The page class the resource already uses first, so a grown segment keeps
    // the geometry the loader was given it in; then the finest page that fits,
    // which wastes the least.
    if (preferred_shift != 0 && try_shift(preferred_shift)) return true;
    for (uint32_t candidate = 3; candidate <= 15; ++candidate) {
        if (try_shift(candidate)) return true;
    }
    return false;
}

namespace {

// Rsc5Texture: name pointer, then the D3DBaseTexture the GPU actually fetches
// from, then the dimensions the loader kept for itself.
constexpr uint32_t kTextureName = 24;
constexpr uint32_t kTextureD3d = 28;
constexpr uint32_t kTextureWidth = 32;
constexpr uint32_t kTextureHeight = 34;

// The Xenos texture fetch constant sits at D3DBaseTexture+0x1C. Word 1 packs
// the base address in its top twenty bits and the data format in its bottom
// six; word 5 packs the mip chain's address the same way.
constexpr uint32_t kFetchBaseWord = 0x1Cu + 4;
constexpr uint32_t kFetchMipWord = 0x1Cu + 20;
// Fetch constant dword 4: mip_min_level at bits 2..5, mip_max_level at 6..9.
constexpr uint32_t kFetchMipLevelWord = 0x1Cu + 16;
constexpr uint32_t kAddressMask = 0xFFFFF000u;
constexpr uint32_t kFormatMask = 0x3Fu;
constexpr uint32_t kFormatDxt1 = 18;
constexpr uint32_t kFormatDxt45 = 20;

// Resources come out of the packer with unwritten space left as 0xCD, which is
// how a mip level that was never authored is told apart from one that was.
constexpr uint8_t kFillerByte = 0xCD;

struct TextureRef {
    uint32_t address = 0;   // the Rsc5Texture struct
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t base = 0;      // pointer to the top level's bytes
    uint32_t mip = 0;       // pointer to levels 1..n, 0 when there are none
    BlockFormat format = BlockFormat::kBc3;
    bool supported = false;
};

bool ReadTexture(const Rsc5View& view, uint32_t address, TextureRef& out) {
    out.address = address;
    uint32_t d3d = 0;
    uint32_t name_pointer = 0;
    uint16_t width = 0, height = 0;
    if (!view.U32(address + kTextureName, name_pointer) ||
        !view.U32(address + kTextureD3d, d3d) || d3d == 0 ||
        !view.U16(address + kTextureWidth, width) ||
        !view.U16(address + kTextureHeight, height) || width == 0 || height == 0) {
        return false;
    }
    view.String(name_pointer, out.name);
    const size_t suffix = out.name.find(".dds");
    if (suffix != std::string::npos) out.name.erase(suffix);

    uint32_t base_word = 0, mip_word = 0;
    if (!view.U32(d3d + kFetchBaseWord, base_word)) return false;
    view.U32(d3d + kFetchMipWord, mip_word);

    out.width = width;
    out.height = height;
    out.base = base_word & kAddressMask;
    out.mip = mip_word & kAddressMask;
    if (out.mip == out.base) out.mip = 0;

    switch (base_word & kFormatMask) {
        case kFormatDxt1: out.format = BlockFormat::kBc1; out.supported = true; break;
        case kFormatDxt45: out.format = BlockFormat::kBc3; out.supported = true; break;
        default: out.supported = false; break;
    }
    return true;
}

// Every texture parameter of one shader, in the order the effect declares them.
std::vector<uint32_t> ShaderTextureParams(const Rsc5View& view, uint32_t shader) {
    std::vector<uint32_t> out;
    uint32_t data_pointer = 0, types_pointer = 0;
    uint16_t count = 0;
    if (!view.U32(shader + 16, data_pointer) || !view.U16(shader + 24, count) ||
        !view.U32(shader + 28, types_pointer) || count == 0) {
        return out;
    }
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t parameter = 0;
        uint8_t type = 1;
        if (!view.U32(data_pointer + i * 4u, parameter) || parameter == 0) continue;
        if (!view.U8(types_pointer + i, type) || type != 0) continue;  // 0 == texture
        out.push_back(parameter);
    }
    return out;
}

// A wheel's textures are not in its drawable. They live in the sibling
// `<wheel>.xtp`, resource type 83, whose root is its own object at the start of
// the virtual segment:
//
//   root      +0x0C  ->  material pack
//   pack      +0x04  ->  grcTextureDictionary   (0 when the wheel is untextured)
//             +0x08  ->  the pack's materials, count at +0x0C
//   dictionary+0x10  ->  name hashes, u16 count at +0x14
//             +0x18  ->  grcTexture*, u16 count at +0x1C
//
// A grcTexture is byte for byte the one ReadTexture already parses, so only the
// walk down to the array is new. Checked against all 175 shipped wheels: 147
// carry exactly one texture -- 256x256 DXT5 for most of them, DXT1 for
// twenty-two, and a handful at 128 or 64 -- and the other 28 have no physical
// segment at all and no dictionary to point at, which is how an untextured
// wheel is stored rather than a case to be worked around.
constexpr uint32_t kTypeTexturePack = 83;
constexpr uint32_t kPackField = 12;
constexpr uint32_t kPackDictionary = 4;
constexpr uint32_t kDictionaryEntries = 24;
constexpr uint32_t kDictionaryCount = 28;

std::vector<uint32_t> TexturePackTextures(const Rsc5View& view) {
    std::vector<uint32_t> out;
    uint32_t pack = 0, dictionary = 0, entries = 0;
    uint16_t count = 0;
    if (!view.U32(kVirtualBase + kPackField, pack) || pack == 0) return out;
    if (!view.U32(pack + kPackDictionary, dictionary) || dictionary == 0) return out;
    if (!view.U32(dictionary + kDictionaryEntries, entries) || entries == 0) return out;
    if (!view.U16(dictionary + kDictionaryCount, count) || count == 0) return out;

    for (uint16_t i = 0; i < count; ++i) {
        uint32_t texture = 0;
        if (view.U32(entries + i * 4u, texture) && texture != 0) out.push_back(texture);
    }
    return out;
}

// Which of the pack's materials samples `texture`.
//
// The pack's material array is in the same order as the drawable's shader
// indices -- a wheel ships two, and its models name them 0 and 1 -- so the
// answer is the shader the mesh has to be drawn with for the texture to be
// read at all. A material states where its parameters live (+0x10) and how big
// that block is (+0x18, the count in the high half and the byte size in the
// low), so the search is a bounded scan of one block per material rather than
// a guess at the parameter layout.
constexpr uint32_t kPackMaterials = 8;
// Which effect a material runs, as an index into the game's car shader list.
constexpr uint32_t kMaterialEffect = 12;
constexpr uint32_t kPackMaterialCount = 12;
constexpr uint32_t kMaterialParameters = 16;
constexpr uint32_t kMaterialParameterSize = 24;

bool TexturePackMaterialOf(const Rsc5View& view, uint32_t texture, uint32_t& index) {
    uint32_t pack = 0, materials = 0;
    uint16_t count = 0;
    if (!view.U32(kVirtualBase + kPackField, pack) || pack == 0) return false;
    if (!view.U32(pack + kPackMaterials, materials) || materials == 0) return false;
    if (!view.U16(pack + kPackMaterialCount, count) || count == 0) return false;

    for (uint16_t i = 0; i < count; ++i) {
        uint32_t material = 0, parameters = 0, packed = 0;
        if (!view.U32(materials + i * 4u, material) || material == 0) continue;
        if (!view.U32(material + kMaterialParameters, parameters) || parameters == 0) continue;
        if (!view.U32(material + kMaterialParameterSize, packed)) continue;

        const uint32_t bytes = packed & 0xFFFFu;
        for (uint32_t at = 0; at + 4 <= bytes; at += 4) {
            uint32_t word = 0;
            if (view.U32(parameters + at, word) && word == texture) {
                index = i;
                return true;
            }
        }
    }
    return false;
}

// How far a write at `address` may run before it reaches whatever the resource
// allocated next. `allocations` is every address the resource hands out, sorted
// and deduplicated, which is what keeps a texture from spilling into the buffer
// that follows it.
size_t RoomAfter(const Rsc5View& view, const Rsc5Resource& resource,
                 const std::vector<uint32_t>& allocations, uint32_t address) {
    size_t offset = 0;
    if (!view.Offset(address, 1, offset)) return 0;
    const auto next = std::upper_bound(allocations.begin(), allocations.end(), address);
    if (next == allocations.end()) return resource.data.size() - offset;
    size_t next_offset = 0;
    if (!view.Offset(*next, 1, next_offset) || next_offset <= offset) return 0;
    return next_offset - offset;
}

// Writes one image over one shipped texture, in place: resampled to the size it
// already is, encoded in the format it already declares, tiled, and dropped at
// the address it already lives at.
bool WriteTextureInPlace(Rsc5View& view, Rsc5Resource& resource,
                         const std::vector<uint32_t>& allocations, const TextureRef& slot,
                         const Image& image, uint32_t& out_levels) {
    auto write_level = [&](uint32_t address, size_t skip, const Image& source,
                           size_t budget) -> size_t {
        std::vector<uint8_t> blocks, tiled;
        CompressBlocks(source, slot.format, blocks);
        TileXbox360(blocks, source.width, source.height, slot.format, tiled);

        const uint32_t expected = StoredTextureSize(source.width, source.height, slot.format);
        if (tiled.size() != expected || skip + expected > budget) return 0;

        size_t offset = 0;
        if (!view.Offset(address, skip + expected, offset)) return 0;
        std::memcpy(resource.data.data() + offset + skip, tiled.data(), tiled.size());
        return static_cast<size_t>(expected);
    };

    Image level;
    ResizeImage(image, slot.width, slot.height, level);
    if (write_level(slot.base, 0, level, RoomAfter(view, resource, allocations, slot.base)) == 0)
        return false;

    // The mip chain, if the resource stores one. Levels are packed one after
    // another, each padded out to a 128-texel virtual image, and the run stops
    // at the first level the original left unwritten -- matching the shipped
    // level count is what keeps the write inside its allocation.
    out_levels = 0;
    if (slot.mip == 0) return true;

    const size_t budget = RoomAfter(view, resource, allocations, slot.mip);
    size_t mip_offset = 0;
    view.Offset(slot.mip, 1, mip_offset);

    size_t written = 0;
    Image current = level;
    while (current.width > 1 || current.height > 1) {
        Image next;
        HalveImage(current, next);
        const uint32_t stored = StoredTextureSize(next.width, next.height, slot.format);
        if (written + stored > budget) break;

        const uint8_t* existing = resource.data.data() + mip_offset + written;
        const bool authored = std::any_of(existing, existing + stored,
                                          [](uint8_t byte) { return byte != kFillerByte; });
        if (!authored) break;

        if (write_level(slot.mip, written, next, budget) == 0) break;
        written += stored;
        ++out_levels;
        current = std::move(next);
    }

    // Declare only the levels that were written.
    //
    // The run above stops where the Xenos packs the small levels into one
    // tail, so a shipped chain is only partly replaced -- and a sampler that
    // still believes the old level count reads the DONOR's picture from the
    // levels left behind. Measured on the BMW e38: vp_chv_impala_96_trim
    // declares mip_max_level 7 (512x512) and 6 in the .xtl, the write
    // reached 2 and 1, and the badges turned the colour of the Impala's trim
    // sheet more the further away they were. mip_max_level is fetch constant
    // dword 4, bits 6..9.
    uint32_t levels_word = 0;
    uint32_t d3d = 0;
    if (view.U32(slot.address + kTextureD3d, d3d) && d3d != 0 &&
        view.U32(d3d + kFetchMipLevelWord, levels_word)) {
        const uint32_t declared = (levels_word >> 6) & 0xFu;
        if (declared > out_levels) {
            levels_word = (levels_word & ~(0xFu << 6)) | (out_levels << 6);
            view.SetU32(d3d + kFetchMipLevelWord, levels_word);
        }
    }
    return true;
}

// Every vertex and index buffer the drawable owns, across all four LODs.
//
// Texture data and geometry data share the physical segment, so the bound on
// how far a texture write may run has to know about both: the shipped
// resources leave slack after a mip chain, and in at least one of them the
// vertex buffer is what sits in that slack.
void CollectBufferAddresses(const Rsc5View& view, const DrawableLayout& drawable,
                            std::vector<uint32_t>& out) {
    for (uint32_t slot = 0; slot < drawable.lod_slots; ++slot) {
        uint32_t lod = 0;
        if (!view.U32(drawable.drawable + drawable.lod_field + slot * 4, lod) || lod == 0) continue;

        uint32_t model_array = 0;
        uint16_t model_count = 0;
        if (!view.U32(lod, model_array) || !view.U16(lod + 4, model_count)) continue;

        for (uint16_t m = 0; m < model_count; ++m) {
            uint32_t model = 0;
            if (!view.U32(model_array + m * 4, model) || model == 0) continue;

            uint32_t geometry_array = 0;
            uint16_t geometry_count = 0;
            if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count))
                continue;

            for (uint16_t g = 0; g < geometry_count; ++g) {
                uint32_t geometry = 0, buffer = 0, address = 0;
                if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;
                if (view.U32(geometry + 12, buffer) && buffer &&
                    view.U32(buffer + 24, address) && address) {
                    out.push_back(address);
                }
                if (view.U32(geometry + 28, buffer) && buffer &&
                    view.U32(buffer + 8, address) && address) {
                    out.push_back(address);
                }
            }
        }
    }
}

// The effect a shader runs, e.g. "Character_normalmap".
std::string ShaderName(const Rsc5View& view, uint32_t shader) {
    uint32_t name = 0;
    std::string out;
    if (view.U32(shader + 48, name)) view.String(name, out);
    return out;
}

bool ShaderGroupOf(const Rsc5View& view, const DrawableLayout& drawable, uint32_t& group,
                   uint32_t& array, uint16_t& count) {
    if (!drawable.shader_group) return false;
    if (!view.U32(drawable.drawable + 8, group) || group == 0) return false;
    return view.U32(group + 8, array) && array != 0 && view.U16(group + 12, count) && count != 0;
}

// Which models of a drawable's first LOD are authored in a bone's local frame
// rather than the car's. See MeshOffset::car_space_only for the measurement.
std::vector<bool> LocalModels(const Rsc5View& view, const std::vector<uint8_t>& data,
                              uint32_t model_array, uint16_t model_count) {
    std::vector<bool> local(model_count, false);
    constexpr float kFloor = 0.15f;
    constexpr float kStraddle = 0.02f;
    for (uint16_t m = 0; m < model_count; ++m) {
        uint32_t model = 0, geometry_array = 0;
        uint16_t geometry_count = 0;
        if (!view.U32(model_array + m * 4, model) || model == 0) continue;
        if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count)) continue;

        float low[3] = {0, 0, 0}, high[3] = {0, 0, 0};
        bool any = false;
        for (uint16_t g = 0; g < geometry_count; ++g) {
            uint32_t geometry = 0, buffer = 0, address = 0;
            uint16_t vertex_count = 0;
            if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;
            if (!view.U32(geometry + 12, buffer) || buffer == 0) continue;
            if (!view.U16(geometry + 52, vertex_count) || vertex_count == 0) continue;
            if (!view.U32(buffer + 24, address) || address == 0) continue;
            VertexLayout layout;
            if (!ReadVertexLayout(view, buffer, layout, false)) continue;
            size_t at = 0;
            if (!view.Offset(address, static_cast<size_t>(vertex_count) * layout.stride, at))
                continue;
            for (uint16_t v = 0; v < vertex_count; ++v) {
                const uint8_t* p = data.data() + at + v * layout.stride +
                                   layout.offset[kSemPosition];
                for (int i = 0; i < 3; ++i) {
                    const float value = LoadBEFloat(p + i * 4);
                    low[i] = any ? std::min(low[i], value) : value;
                    high[i] = any ? std::max(high[i], value) : value;
                }
                any = true;
            }
        }
        if (!any) continue;
        const bool below_floor = (low[1] + high[1]) * 0.5f < kFloor;
        const bool straddles = low[0] < kStraddle && high[0] > -kStraddle &&
                               low[1] < kStraddle && high[1] > -kStraddle &&
                               low[2] < kStraddle && high[2] > -kStraddle;
        local[m] = below_floor || straddles;
    }
    return local;
}

// The box a template's own mesh occupies, for the roots that do not store one.
//
// The character drawable states its bounds and they are used as they are; a
// wheel does not, so the shipped vertices are measured instead. Reading them
// costs one pass over the high LOD and gives the same thing the box would: the
// space the replacement has to be fitted into so the game's culling, LOD
// distances and wheel radius all stay true.
bool MeasureTemplateBounds(const Rsc5View& view, const std::vector<uint8_t>& data,
                           const DrawableLayout& drawable, float* box_min, float* box_max) {
    bool any = false;

    uint32_t lod = 0;
    if (!view.U32(drawable.drawable + drawable.lod_field, lod) || lod == 0) return false;

    uint32_t model_array = 0;
    uint16_t model_count = 0;
    if (!view.U32(lod, model_array) || !view.U16(lod + 4, model_count)) return false;

    for (uint16_t m = 0; m < model_count; ++m) {
        uint32_t model = 0;
        if (!view.U32(model_array + m * 4, model) || model == 0) continue;

        uint32_t geometry_array = 0;
        uint16_t geometry_count = 0;
        if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count)) continue;

        for (uint16_t g = 0; g < geometry_count; ++g) {
            uint32_t geometry = 0, buffer = 0, address = 0;
            uint16_t vertex_count = 0;
            if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;
            if (!view.U32(geometry + 12, buffer) || buffer == 0) continue;
            if (!view.U16(geometry + 52, vertex_count) || vertex_count == 0) continue;
            if (!view.U32(buffer + 24, address) || address == 0) continue;

            VertexLayout layout;
            if (!ReadVertexLayout(view, buffer, layout)) continue;

            size_t at = 0;
            const size_t bytes = static_cast<size_t>(vertex_count) * layout.stride;
            if (!view.Offset(address, bytes, at)) continue;

            for (uint16_t v = 0; v < vertex_count; ++v) {
                const uint8_t* position = data.data() + at + v * layout.stride +
                                          layout.offset[kSemPosition];
                for (int i = 0; i < 3; ++i) {
                    const float value = LoadBEFloat(position + i * 4);
                    if (!any) {
                        box_min[i] = box_max[i] = value;
                    } else {
                        box_min[i] = std::min(box_min[i], value);
                        box_max[i] = std::max(box_max[i], value);
                    }
                }
                any = true;
            }
        }
    }
    return any;
}

// Slides a wheel out along its axle until its face meets the shipped wheel's.
//
// X is the axle: every shipped rim measures half a unit across it against a full
// unit of diameter. What the box does not say is that the wheel is not centred in
// it -- the hub and spokes sit between about -0.25 and -0.15, and the only thing
// out at +0.25 is the thin lip where the barrel ends. Measured across fourteen of
// them the hub is at negative X every time, from -0.02 on a flat face to -0.30 on
// a deep dish. So centring a replacement in the box, which is what fitting does,
// buries a bare rim half a unit inside the tyre.
//
// The correction is to put the mod's own outer face on the template's outermost
// plane. Which end of the mod that face is on is not something a modeller can be
// asked to get right, so it is read off the mesh: the hub end is the one with
// geometry near the axle, since a wheel is open at the back and solid at the
// centre of its face. If the mod came in facing the other way it is turned around
// first.
void AlignMeshToAxle(Mesh& mesh, float target_min_x) {
    if (mesh.vertices.empty()) return;

    auto hub_and_extremes = [&](float& hub, float& min_x, float& max_x) {
        min_x = max_x = mesh.vertices.front().px;
        float radius_max = 0.0f;
        for (const MeshVertex& vertex : mesh.vertices) {
            min_x = std::min(min_x, vertex.px);
            max_x = std::max(max_x, vertex.px);
            radius_max = std::max(radius_max,
                                  std::sqrt(vertex.py * vertex.py + vertex.pz * vertex.pz));
        }
        // Everything within a quarter of the disc's radius counts as the hub.
        const float limit = radius_max * 0.25f;
        double sum = 0.0;
        size_t count = 0;
        for (const MeshVertex& vertex : mesh.vertices) {
            if (std::sqrt(vertex.py * vertex.py + vertex.pz * vertex.pz) > limit) continue;
            sum += vertex.px;
            ++count;
        }
        // A rim with an open centre has no hub to read; its own midpoint is then
        // as good a guess as any, and the flip below simply does not trigger.
        hub = count ? static_cast<float>(sum / static_cast<double>(count))
                    : (min_x + max_x) * 0.5f;
    };

    float hub = 0.0f, min_x = 0.0f, max_x = 0.0f;
    hub_and_extremes(hub, min_x, max_x);

    if (hub > (min_x + max_x) * 0.5f) {
        TransformMesh(mesh, 180.0f, 0.0f, 0.0f, 1.0f);
        hub_and_extremes(hub, min_x, max_x);
    }

    const float shift = target_min_x - min_x;
    for (auto& vertex : mesh.vertices) vertex.px += shift;
}

// ---------------------------------------------------------------------------
// Diagnostics.
//
// None of this runs unless MeshOffset::diagnose is set. What it exists for: the
// rewrite has three places a character can be ruined -- the retarget can put a
// joint on the wrong bone, the dealing can refuse triangles or split them across
// palettes, and a submesh the writer cannot describe is silently left drawing
// whatever the template shipped. All three arrive on screen looking the same
// (a mangled character) and none of them says anything in the log, so they are
// written out instead and read afterwards.

std::string Number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", value);
    return text;
}

std::string Hex(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

std::string Point(const Vec3& p) {
    return "(" + Number(p.x) + ", " + Number(p.y) + ", " + Number(p.z) + ")";
}

// The mesh as Wavefront .obj, so it can be opened in anything. Positions,
// normals and UVs only -- the point is the shape.
std::string MeshToObj(const Mesh& mesh) {
    std::string out;
    out.reserve(mesh.vertices.size() * 64);
    for (const MeshVertex& vertex : mesh.vertices) {
        out += "v " + Number(vertex.px) + " " + Number(vertex.py) + " " + Number(vertex.pz) + "\n";
    }
    for (const MeshVertex& vertex : mesh.vertices)
        out += "vt " + Number(vertex.u) + " " + Number(1.0f - vertex.v) + "\n";
    for (const MeshVertex& vertex : mesh.vertices) {
        out += "vn " + Number(vertex.nx) + " " + Number(vertex.ny) + " " + Number(vertex.nz) + "\n";
    }
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        out += "f";
        for (int c = 0; c < 3; ++c) {
            const std::string n = std::to_string(mesh.indices[i + c] + 1);
            out += " " + n + "/" + n + "/" + n;
        }
        out += "\n";
    }
    return out;
}

// A rig written out as a tree, deepest last, with each joint's bone and where
// that bone stands. Reading the two columns against each other is what shows a
// limb mapped onto the wrong chain.
std::string RigReport(const Rig& source, const Rig& target, const std::vector<int>& mapping,
                      const std::vector<std::string>& names, const Landmarks& marks) {
    auto label = [&](int joint) {
        if (joint < 0) return std::string("-");
        std::string text = std::to_string(joint);
        if (static_cast<size_t>(joint) < names.size() && !names[static_cast<size_t>(joint)].empty())
            text += " " + names[static_cast<size_t>(joint)];
        return text;
    };

    std::string out;
    out += "  landmarks: pelvis=" + label(marks.pelvis) + " chest=" + label(marks.chest) +
           " head=" + label(marks.head) + "\n";
    for (int side = 0; side < 2; ++side) {
        out += std::string("    side ") + (side ? "+x" : "-x") +
               ": shoulder=" + label(marks.shoulder[side]) + " wrist=" + label(marks.wrist[side]) +
               " hip=" + label(marks.hip[side]) + " foot=" + label(marks.foot[side]) + "\n";
    }
    out += "  joint -> bone (source position, target position, gap)\n";
    for (size_t joint = 0; joint < source.size(); ++joint) {
        const int bone = joint < mapping.size() ? mapping[joint] : -1;
        out += "    " + label(static_cast<int>(joint)) +
               " parent=" + std::to_string(source.parent[joint]) +
               (source.real[joint] ? "" : " (no bind)") + " at " + Point(source.position[joint]) +
               " -> ";
        if (bone < 0 || static_cast<size_t>(bone) >= target.size()) {
            out += "UNMAPPED\n";
            continue;
        }
        const Vec3 gap = target.position[static_cast<size_t>(bone)] - source.position[joint];
        out += "bone " + std::to_string(bone) + " at " +
               Point(target.position[static_cast<size_t>(bone)]) + " gap " + Number(Length(gap)) +
               "\n";
    }
    return out;
}

}  // namespace

bool RewriteDrawableGeometry(Rsc5Resource& resource, Mesh mesh, uint32_t bone,
                             const MeshOffset& offset, std::string& error,
                             RewriteStats* stats) {
    // The 16-bit index buffer bounds a SUBMESH, not the model that arrives.
    //
    // There used to be a check here refusing any mesh of more than 65535
    // vertices, and it was wrong: the indices written out are local to the chunk
    // dealt into each submesh, and a chunk can never hold more vertices than the
    // submesh it fills -- a few thousand. A source model is decimated down to
    // that long before anything is written, so refusing it up front threw away
    // models that would have fitted comfortably. A 200,000-vertex wheel was
    // being rejected on its way to a slot with room for 3,700.
    Rsc5View view(resource.data, resource.virtual_size);

    const bool diagnose = offset.diagnose && stats != nullptr;
    std::string report;
    auto say = [&](const std::string& line) {
        if (diagnose) report += line + "\n";
    };

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) {
        error = "no drawable in resource";
        return false;
    }
    // The flag is the page geometry, and the page geometry is a budget: the
    // loader builds one chunk per page and its map holds 127 of them. A mantissa
    // IS the page count, so a segment re-encoded onto a smaller page can need
    // more chunks than the map can hold -- which is not a size the encoder was
    // ever checking.
    auto page_note = [&](const char* when) {
        const uint32_t vm = resource.flag & 0x7FFu, vs = (resource.flag >> 11) & 0xFu;
        const uint32_t pm = (resource.flag >> 15) & 0x7FFu, ps = (resource.flag >> 26) & 0xFu;
        say(std::string(when) + " flag " + Hex(resource.flag) + ": virtual " +
            std::to_string(vm) + " page(s) of " + std::to_string(1u << (vs + 8)) + " = " +
            std::to_string(vm << (vs + 8)) + ", physical " + std::to_string(pm) +
            " page(s) of " + std::to_string(1u << (ps + 8)) + " = " +
            std::to_string(pm << (ps + 8)) + ", " + std::to_string(vm + pm) +
            " chunk(s)");
    };
    say("resource type " + std::to_string(resource.type) + ", drawable at " +
        Hex(drawable.drawable) + ", virtual " + std::to_string(resource.virtual_size) +
        " bytes, physical " + std::to_string(resource.physical_size));
    if (diagnose) page_note("shipped");
    say("source mesh: " + std::to_string(mesh.vertices.size()) + " vertices, " +
        std::to_string(mesh.indices.size() / 3) + " triangles, " +
        std::to_string(mesh.joint_count()) + " joints, " + std::to_string(mesh.parts.size()) +
        " part(s)");

    // Fit the mesh inside the drawable's own bounding box so every bound,
    // radius and LOD distance in the template stays correct. A root that keeps
    // no box is measured from its own vertices instead.
    float box_min[3], box_max[3];
    bool bounds_ok = true;
    if (drawable.inline_bounds) {
        for (int i = 0; i < 3; ++i) {
            bounds_ok &= view.Float(drawable.drawable + 32 + i * 4, box_min[i]);
            bounds_ok &= view.Float(drawable.drawable + 48 + i * 4, box_max[i]);
        }
    } else {
        bounds_ok = MeasureTemplateBounds(view, resource.data, drawable, box_min, box_max);
    }
    if (!bounds_ok) {
        error = "cannot read drawable bounds";
        return false;
    }
    if (!offset.pre_fitted) {
        TransformMesh(mesh, offset.yaw, offset.pitch, offset.roll, 1.0f);

        // Height-match the mesh, joints included, so the retarget compares two
        // rigs measured in the same units and the reposed result comes out
        // life-sized.
        FitMeshToBox(mesh, box_min, box_max);
        if (offset.align_axle) AlignMeshToAxle(mesh, box_min[0]);
    }
    if (diagnose) {
        float min[3], max[3];
        mesh.Bounds(min, max);
        say("template box " + Point(Vec3{box_min[0], box_min[1], box_min[2]}) + " .. " +
            Point(Vec3{box_max[0], box_max[1], box_max[2]}));
        say("mesh after fit " + Point(Vec3{min[0], min[1], min[2]}) + " .. " +
            Point(Vec3{max[0], max[1], max[2]}));
        stats->obj_before = MeshToObj(mesh);
    }

    // Retarget and repose before anything reorders the vertices: the mod's rig
    // is matched to the driver's limb by limb, and the mesh is rebuilt in the
    // driver's bind pose. Without this the mesh keeps whatever pose it was
    // authored in -- a T-pose against MCLA's A-pose leaves the arms up.
    std::vector<BoneTransform> bones;
    std::vector<uint32_t> bone_parent;
    std::vector<int> joint_to_bone;
    bool retargeted = false;
    if (mesh.skinned() && bone == kAutomaticBone && offset.weight_smoothing > 0) {
        SmoothSkinWeights(mesh, offset.weight_smoothing);
        say("skin weights smoothed " + std::to_string(offset.weight_smoothing) +
            " round(s) over the welded surface");
    }
    if (mesh.skinned() && bone == kAutomaticBone &&
        ReadSkeletonBindPose(view, drawable, bones, bone_parent) && !bones.empty()) {
        const Rig target = RigFromSkeleton(bones, bone_parent);
        const Rig source = RigFromMesh(mesh);
        Landmarks source_marks, target_marks;
        if (BuildJointMapping(source, target, joint_to_bone, source_marks, target_marks)) {
            ReposeMesh(mesh, source, target, joint_to_bone, source_marks,
                       std::clamp(offset.proportions, 0.0f, 1.0f), offset.anchor_bones);
            retargeted = true;
            if (diagnose) {
                say("");
                say("retarget: " + std::to_string(source.size()) + " source joints onto " +
                    std::to_string(target.size()) + " bones, proportions " +
                    Number(std::clamp(offset.proportions, 0.0f, 1.0f)));
                say("source rig");
                report += RigReport(source, target, joint_to_bone, mesh.joint_name, source_marks);
                say("target rig (the driver's own skeleton)");
                std::vector<int> identity(target.size());
                for (size_t i = 0; i < identity.size(); ++i) identity[i] = static_cast<int>(i);
                report += RigReport(target, target, identity, {}, target_marks);
            }
        } else {
            say("retarget REFUSED: the landmarks do not read as a humanoid on one of the two "
                "rigs; the mesh keeps its authored pose");
        }
        if (!retargeted) {
            // A rig that does not read as a humanoid -- a prop, a partial
            // skeleton -- keeps its own pose rather than being folded into a
            // shape the landmarks could not describe.
            joint_to_bone.clear();
        }
    }

    if (offset.scale != 1.0f && offset.scale > 0.0f) {
        // Scale about the fitted centre so tuning size does not move the model.
        float min[3], max[3];
        mesh.Bounds(min, max);
        const float centre[3] = {(min[0] + max[0]) * 0.5f, min[1], (min[2] + max[2]) * 0.5f};
        for (auto& vertex : mesh.vertices) {
            vertex.px = (vertex.px - centre[0]) * offset.scale + centre[0];
            vertex.py = (vertex.py - centre[1]) * offset.scale + centre[1];
            vertex.pz = (vertex.pz - centre[2]) * offset.scale + centre[2];
        }
    }

    uint32_t lod = 0;
    if (!view.U32(drawable.drawable + drawable.lod_field, lod) || lod == 0) {
        error = "drawable has no high LOD";
        return false;
    }

    uint32_t model_array = 0;
    uint16_t model_count = 0;
    if (!view.U32(lod, model_array) || !view.U16(lod + 4, model_count) || model_count == 0) {
        error = "LOD has no models";
        return false;
    }

    std::vector<GeometryRef> geometries;
    say("");
    say("template LOD 0: " + std::to_string(model_count) + " model(s)");
    for (uint16_t m = 0; m < model_count; ++m) {
        uint32_t model = 0;
        if (!view.U32(model_array + m * 4, model) || model == 0) continue;

        uint32_t geometry_array = 0;
        uint16_t geometry_count = 0;
        if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count)) continue;

        // Which shader draws each of the model's geometries: a parallel array
        // of shader-group indices hanging off model+16.
        uint32_t shader_map = 0;
        view.U32(model + 16, shader_map);

        for (uint16_t g = 0; g < geometry_count; ++g) {
            uint32_t geometry = 0;
            if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;

            GeometryRef ref;
            ref.address = geometry;
            uint16_t vertex_count = 0, bone_count = 0;
            if (!view.U32(geometry + 12, ref.vertex_buffer) ||
                !view.U32(geometry + 28, ref.index_buffer) ||
                !view.U32(geometry + 44, ref.index_count) ||
                !view.U16(geometry + 52, vertex_count) ||
                !view.U32(geometry + 56, ref.bone_palette) ||
                !view.U16(geometry + 62, bone_count)) {
                continue;
            }
            ref.vertex_count = vertex_count;
            ref.bone_count = bone_count;
            uint16_t shader = 0;
            if (shader_map && view.U16(shader_map + g * 2u, shader)) ref.shader = shader;
            ref.shader_map = shader_map;
            ref.slot_in_model = g;
            ref.model = m;
            // A pass that owns one shader only considers that shader's slots,
            // and reads the assignment as it SHIPPED -- an earlier pass may
            // already have repointed a slot it took over, and rereading that
            // would let the second pass steal it back.
            if (offset.only_shader >= 0 &&
                ref.shader != static_cast<uint32_t>(offset.only_shader)) {
                continue;
            }
            // A submesh whose layout cannot be written is simply not offered as
            // a slot; the rest of the character still gets replaced.
            const bool writable = ReadVertexLayout(view, ref.vertex_buffer, ref.layout,
                                                   !offset.allow_untextured);
            if (diagnose) say("  submesh model " + std::to_string(m) + " slot " + std::to_string(g) + " at " +
                Hex(geometry) + ": " + std::to_string(ref.vertex_count) + " vertices, " +
                std::to_string(ref.index_count) + " indices, " + std::to_string(ref.bone_count) +
                " palette entries, shader " + std::to_string(ref.shader) +
                (writable ? ", layout stride " + std::to_string(ref.layout.stride)
                          : ", LAYOUT REJECTED -- neither written nor silenced"));
            if (diagnose) {
                // What the buffers say about where their data is, in all four
                // places it is stated. The software pointers at +8 and +24 are
                // resource-relative (0x5.. virtual, 0x6.. physical); whether
                // word 6 of the GPU descriptor is the same kind of address is
                // exactly what the grow path assumes and nothing has ever
                // checked -- and a fetch pointed at the wrong memory is spikes.
                uint32_t vb8 = 0, vb24 = 0, vgpu = 0, vw6 = 0, vw7 = 0;
                uint32_t ib8 = 0, igpu = 0, iw6 = 0, iw7 = 0;
                view.U32(ref.vertex_buffer + 8, vb8);
                view.U32(ref.vertex_buffer + 24, vb24);
                view.U32(ref.vertex_buffer + 28, vgpu);
                if (vgpu) {
                    view.U32(vgpu + kGpuHeaderAddressWord, vw6);
                    view.U32(vgpu + kGpuHeaderSizeWord, vw7);
                }
                view.U32(ref.index_buffer + 8, ib8);
                view.U32(ref.index_buffer + 12, igpu);
                if (igpu) {
                    view.U32(igpu + kGpuHeaderAddressWord, iw6);
                    view.U32(igpu + kGpuHeaderSizeWord, iw7);
                }
                say("      vertex buffer +8=" + Hex(vb8) + " +24=" + Hex(vb24) + " gpu@" +
                    Hex(vgpu) + " word6=" + Hex(vw6) + " word7=" + Hex(vw7));
                say("      index  buffer +8=" + Hex(ib8) + " gpu@" + Hex(igpu) + " word6=" +
                    Hex(iw6) + " word7=" + Hex(iw7));
            }
            if (!writable) continue;
            geometries.push_back(ref);
        }
    }

    if (offset.car_space_only && !geometries.empty()) {
        const std::vector<bool> local =
            LocalModels(view, resource.data, model_array, model_count);
        size_t silenced = 0;
        for (const GeometryRef& reference : geometries) {
            if (reference.model >= local.size() || !local[reference.model]) continue;
            // Silenced here and now, not merely left out: left out, the donor's
            // own bumper paint and door glass would go on drawing beside the
            // replacement.
            view.SetU32(reference.address + 44, 0);
            view.SetU32(reference.address + 48, 0);
            view.SetU16(reference.address + 52, 0);
            if (reference.index_buffer) view.SetU32(reference.index_buffer + 4, 0);
            if (reference.vertex_buffer) view.SetU16(reference.vertex_buffer + 4, 0);
            ++silenced;
        }
        geometries.erase(std::remove_if(geometries.begin(), geometries.end(),
                                        [&](const GeometryRef& reference) {
                                            return reference.model < local.size() &&
                                                   local[reference.model];
                                        }),
                         geometries.end());
        if (stats) stats->local_silenced = static_cast<uint32_t>(silenced);
        if (geometries.empty()) {
            error = "every submesh of this shader is in a bone-local model (" +
                    std::to_string(silenced) + " silenced) -- give its materials a shader "
                    "that has a car-space model, in this drawable or a car-space part slot";
            return false;
        }
    }

    if (geometries.empty()) {
        error = "template has no geometry";
        return false;
    }

    // For a drawable whose models are separate objects, narrow the whole rewrite
    // to the biggest of them before anything else looks at the list. Everything
    // downstream -- the budget, the dealing, the silencing -- then applies to
    // that model alone, and the rest of the drawable is never touched.
    if (offset.whole_models) {
        std::map<uint16_t, size_t> weight;
        for (const GeometryRef& reference : geometries)
            weight[reference.model] += reference.vertex_count;

        uint16_t chosen = geometries.front().model;
        size_t best = 0;
        for (const auto& [model, vertices] : weight) {
            if (vertices > best) {
                best = vertices;
                chosen = model;
            }
        }
        geometries.erase(std::remove_if(geometries.begin(), geometries.end(),
                                        [&](const GeometryRef& reference) {
                                            return reference.model != chosen;
                                        }),
                         geometries.end());
        if (geometries.empty()) {
            error = "template's largest model has no writable geometry";
            return false;
        }
    }

    // Hold back the submeshes drawn by a shader the caller reserved.
    //
    // A wheel's badge is a nine-vertex quad on its own material, and that
    // material is the only one sampling a texture. Dealing rim triangles into
    // it puts them on the wrong surface, and silencing it -- which is what
    // happened to every replaced wheel until now -- takes the badge off the car
    // and leaves the pack's texture read by nothing. Left alone it keeps
    // drawing what it shipped to draw, over whatever paint was written.
    //
    // The reservation is dropped rather than obeyed when it would claim the
    // whole drawable: twenty wheels ship as a single submesh, and on those
    // there is no separate badge to protect.
    if (offset.reserve_shader >= 0) {
        const uint32_t reserved = static_cast<uint32_t>(offset.reserve_shader);
        const bool anything_left =
            std::any_of(geometries.begin(), geometries.end(),
                        [&](const GeometryRef& r) { return r.shader != reserved; });
        if (anything_left) {
            geometries.erase(std::remove_if(geometries.begin(), geometries.end(),
                                            [&](const GeometryRef& r) {
                                                return r.shader == reserved;
                                            }),
                             geometries.end());
        }
    }

    // Keep the shipped per-vertex shading fields before any of them are
    // overwritten.
    //
    // The colour field carries baked shading in one lane -- 0xE4, 0xDE, 0xF8
    // across the driver's own body -- and writing a flat value there is what
    // leaves a model looking bleached and polished, the shading it stands for
    // switched off. The other three lanes are not ours to invent either: a
    // character ships 0xXXFFFFFF and a wheel ships 0xXX000000, so the whole word
    // is carried over rather than the one byte that varies within a character.
    //
    // The same goes for the second UV set, which a wheel has and a character
    // does not. It is not a texture coordinate at all there: it holds one of
    // four constants, each covering its own band of the wheel, and leaving it
    // zeroed hands every vertex the same one -- which is a wheel whose lighting
    // does not follow its shape.
    //
    // There is no per-vertex correspondence to inherit any of this through, but
    // the mesh has just been fitted into this very template's space, so the
    // nearest shipped vertex stands in roughly the same place on the same part
    // and its values transfer.
    struct Shade {
        float x = 0, y = 0, z = 0;
        float nx = 0, ny = 0, nz = 0;
        uint32_t colour = 0xFFFFFFFFu;
        uint32_t texcoord1 = 0;
        uint32_t texcoord0 = 0;
        uint32_t geometry = 0;  // address of the submesh it was read from
    };
    std::vector<Shade> shipped;
    for (size_t index = 0; index < geometries.size(); ++index) {
        const GeometryRef& reference = geometries[index];
        uint32_t address = 0;
        size_t at = 0;
        if (!view.U32(reference.vertex_buffer + 24, address) || address == 0) continue;
        const size_t bytes = static_cast<size_t>(reference.vertex_count) * reference.layout.stride;
        if (!view.Offset(address, bytes, at)) continue;
        for (uint32_t v = 0; v < reference.vertex_count; ++v) {
            const uint8_t* source = resource.data.data() + at + v * reference.layout.stride;
            Shade entry;
            const uint8_t* position = source + reference.layout.offset[kSemPosition];
            entry.x = LoadBEFloat(position + 0);
            entry.y = LoadBEFloat(position + 4);
            entry.z = LoadBEFloat(position + 8);
            if (reference.layout.has(kSemNormal)) {
                UnpackDec3N(LoadBE32(source + reference.layout.offset[kSemNormal]), entry.nx,
                            entry.ny, entry.nz);
            }
            if (reference.layout.has(kSemColour))
                entry.colour = LoadBE32(source + reference.layout.offset[kSemColour]);
            if (reference.layout.has(kSemTexcoord1))
                entry.texcoord1 = LoadBE32(source + reference.layout.offset[kSemTexcoord1]);
            if (reference.layout.has(kSemTexcoord0))
                entry.texcoord0 = LoadBE32(source + reference.layout.offset[kSemTexcoord0]);
            entry.geometry = reference.address;
            shipped.push_back(entry);
        }
    }

    // The template's own vertices, as a point cloud.
    //
    // Where the game's flesh sits relative to the game's bones is the one thing
    // the rewrite has never been able to see, and it is what says whether a
    // joint belongs where the skeleton puts it. The driver's spine bones stand
    // 9 cm behind the middle of a model's own spine bones, so planting one on
    // the other drags the belly in; whether that is wrong, and by how much,
    // is a question only the shipped body can answer.
    if (diagnose) {
        std::string cloud;
        cloud.reserve(shipped.size() * 40);
        for (const Shade& entry : shipped) {
            cloud += "v " + Number(entry.x) + " " + Number(entry.y) + " " + Number(entry.z) + "\n";
        }
        stats->obj_template = std::move(cloud);
    }

    // Every submesh is a slot the mod can be written into, not just the biggest.
    //
    // The resource cannot grow -- that was tried, and even relocating the
    // shipped bytes untouched renders garbage -- so the mesh has to live inside
    // buffers the template already owns. It does not have to live inside ONE of
    // them, though, and using a single submesh throws away most of what the
    // drawable has: the driver is fifteen submeshes totalling 8208 vertices and
    // 37512 indices, against 2320 and 12957 in the largest. Filling them all
    // raises the budget three and a half times, which is the difference between
    // a model welded down to a sixth of itself and one that arrives whole.
    //
    // They can be treated alike because they are alike: all fifteen models carry
    // the same flags, the same 92-matrix count and the same skinned flag, and
    // differ only in how many vertices, indices and palette entries they hold.
    std::sort(geometries.begin(), geometries.end(),
              [](const GeometryRef& a, const GeometryRef& b) {
                  if (a.vertex_count != b.vertex_count) return a.vertex_count > b.vertex_count;
                  return a.address < b.address;
              });

    const GeometryRef& biggest = geometries.front();

    // Which shader the whole model should be drawn with.
    //
    // The biggest submesh is the head, and the head runs
    // Character_skin_blendshape_normalmap -- a skin shader, forty-seven
    // parameters against the thirty of the plain Character_normalmap the game
    // uses for arms, torso and legs. Putting a whole clothed character through
    // it lights every surface as flesh: the subsurface and rim terms wash out
    // at grazing angles, which is the pale, polished look along a back or a
    // flank. It also drags in the tiling skin-pore detail map, which is not
    // ours and is not skin.
    //
    // So the plain one is preferred, and among those the one already sampling
    // the same diffuse the head does, since that is the texture the atlas is
    // written over. Every shader here shares a draw bucket, so this only
    // changes how the surface is lit.
    uint32_t render_shader = biggest.shader;
    {
        uint32_t group = 0, array = 0;
        uint16_t count = 0;
        if (ShaderGroupOf(view, drawable, group, array, count)) {
            auto diffuse_of = [&](uint32_t index) -> uint32_t {
                uint32_t shader = 0;
                if (index >= count || !view.U32(array + index * 4u, shader) || shader == 0)
                    return 0;
                const std::vector<uint32_t> parameters = ShaderTextureParams(view, shader);
                return parameters.empty() ? 0 : parameters.front();
            };
            const uint32_t wanted = diffuse_of(biggest.shader);

            uint32_t fallback = 0xFFFFFFFFu;
            for (uint16_t s = 0; s < count; ++s) {
                uint32_t shader = 0;
                if (!view.U32(array + s * 4u, shader) || shader == 0) continue;
                if (ShaderName(view, shader) != "Character_normalmap") continue;
                if (fallback == 0xFFFFFFFFu) fallback = s;
                if (wanted != 0 && diffuse_of(s) == wanted) {
                    fallback = s;
                    break;
                }
            }
            if (fallback != 0xFFFFFFFFu) render_shader = fallback;
        }
    }
    // A wheel's shader group cannot be read here at all, so which of its
    // materials samples a texture is something only the sibling texture pack
    // knows. The caller looks it up there and says so.
    if (offset.force_shader >= 0) render_shader = static_cast<uint32_t>(offset.force_shader);
    if (stats) stats->shader = render_shader;

    // Every texture the chosen shader samples, not just the one that gets
    // replaced. Only the first is written over, so the rest are the SHIPPED
    // character's -- and a normal map belonging to another head, lit across
    // this one, is a face full of somebody else's creases and a specular that
    // answers to nothing on the surface. Naming them is the first step to
    // deciding which of them may be left alone.
    if (diagnose) {
        uint32_t group = 0, array = 0, shader = 0;
        uint16_t count = 0;
        if (ShaderGroupOf(view, drawable, group, array, count) && render_shader < count &&
            view.U32(array + render_shader * 4u, shader) && shader != 0) {
            say("");
            say("shader " + std::to_string(render_shader) + " (" + ShaderName(view, shader) +
                ") samples:");
            const std::vector<uint32_t> parameters = ShaderTextureParams(view, shader);
            for (size_t i = 0; i < parameters.size(); ++i) {
                TextureRef texture;
                if (!ReadTexture(view, parameters[i], texture)) {
                    say("  [" + std::to_string(i) + "] parameter " + Hex(parameters[i]) +
                        ": not a texture this can read");
                    continue;
                }
                say("  [" + std::to_string(i) + "] " +
                    (texture.name.empty() ? std::string("<unnamed>") : texture.name) + " " +
                    std::to_string(texture.width) + "x" + std::to_string(texture.height) +
                    " format " + std::to_string(static_cast<int>(texture.format)) +
                    (texture.supported ? "" : " (unsupported)") + " base " + Hex(texture.base) +
                    " mip " + Hex(texture.mip) + (i == 0 ? "   <- the one replaced" : ""));
            }
        }
    }

    // The diagnostic paths write one palette entry by hand and only make sense
    // against a single submesh.
    const bool slot_test = bone == kSlotOneTest;
    const bool single_slot = slot_test || !offset.submeshes;
    if (single_slot) geometries.resize(1);

    // Give the mesh room of its own, rather than making it fit what shipped.
    //
    // The resource is made longer and the biggest submesh is pointed at the new
    // space: buffers at the end of the physical segment, both of the vertex
    // buffer's data pointers, both GPU fetch descriptors. Nothing that was
    // already in the resource moves, so every other address in it still resolves
    // to the same byte.
    //
    // 65535 is a real ceiling and stays: a submesh states its vertex count in
    // sixteen bits and indexes into it with sixteen more. It is far above what
    // any slot shipped with -- the wheel this was built for holds 3,701.
    // A retargeted character used to be held out of this, from when growing a
    // resource was believed not to work at all and the probe that said otherwise
    // could only be run on something simpler. It works: a wheel came back with
    // 6177 vertices and 9318 triangles in a slot that shipped room for 5673 and
    // 6436, welded nowhere. What a character still cannot grow is its bone
    // palette -- that is a separate allocation and the biggest submesh names 28
    // bones -- so a rig needing more than that still has its triangles dealt
    // across the other slots rather than crammed into one.
    if (offset.grow_buffers && !geometries.empty()) {
      // Growing one slot is not always enough any more.
      //
      // It was while a slot could be asked for the whole mesh. With the
      // sixteen-bit draw ceiling below, the biggest slot tops out at 21,845
      // triangles, and a character carrying more than that needs a second slot
      // made big enough to take the remainder rather than being decimated down
      // to whatever the other slots happened to ship with. Slots are grown in
      // capacity order until the room covers the mesh; a probe still moves only
      // the first, since it is measuring one relocation.
      size_t have_vertices = 0, have_indices = 0;
      for (const GeometryRef& slot : geometries) {
          have_vertices += slot.vertex_count;
          have_indices += slot.index_count;
      }
      const size_t slots_to_grow = offset.grow_probe ? size_t(1) : geometries.size();
      for (size_t growing = 0; growing < slots_to_grow; ++growing) {
        if (have_vertices >= mesh.vertices.size() && have_indices >= mesh.indices.size()) break;
        GeometryRef& target = geometries[growing];
        const uint32_t had_vertices = target.vertex_count;
        const uint32_t had_indices = target.index_count;

        // What this slot is asked to hold.
        //
        // The first slot is asked for the whole mesh, as it always was. A slot
        // opened after it is only there for what the ceiling left over, so it
        // asks for the shortfall and not a byte more -- a second slot sized for
        // the whole character would be most of a megabyte nothing ever draws.
        // Vertices are sized against the leftover INDICES, since a triangle
        // dealt to another slot brings its vertices with it and, worst case,
        // shares none of them.
        //
        // The shortfall is asked for with a quarter again on top, because it is
        // a floor and not a fit. Dealing refuses a triangle that would overrun
        // the slot it is working on and leaves it for the next one, so a set of
        // slots sized to the exact total always ends with triangles nobody can
        // take -- and the answer to that is a fifth off the whole mesh and
        // another attempt. Measured on a car body cut to shader: 31,844
        // triangles into slots summing to exactly 31,844 came back decimated to
        // 19,643. The headroom is a few kilobytes.
        const size_t indices_short =
            mesh.indices.size() > have_indices
                ? (mesh.indices.size() - have_indices) * 5 / 4 + 3072
                : 0;
        uint32_t want_vertices = static_cast<uint32_t>(std::min<size_t>(
            growing == 0 ? mesh.vertices.size() : target.vertex_count + indices_short,
            0xFFFFu));
        // Indices have a ceiling of their own, and it is not the buffer's.
        //
        // A submesh is drawn by one PM4 draw, and that packet states how many
        // indices it draws in VGT_DRAW_INITIATOR.num_indices -- sixteen bits.
        // Room in the index buffer is therefore not permission to fill it: a
        // slot asked for more than 65535 indices cannot have that count stated:
        // it is truncated on the way into the packet, and the half that does not
        // fit lands wherever the geometry's own count field ends.
        //
        // The character that crossed it -- 80,874 indices asked of one slot --
        // came back as spikes across the screen the moment animation moved a
        // bone, and drew whole under `forceidentitypose`, which asks nothing of
        // the bone matrices. Which of the two halves of the overflow does that
        // was not chased further: a count over the ceiling cannot be described
        // to the hardware at all, so there is nothing above it to keep.
        //
        // The wheel this path was built on never crossed it (27,954 indices at
        // its widest), which is why growing worked there and only a
        // 26,958-triangle character showed the fault.
        //
        // 65535 is a whole number of triangles, 21845, so the cap costs nothing
        // beyond the triangles it leaves for the other slots.
        constexpr uint32_t kMaxDrawIndices = 65535;
        uint32_t want_indices = static_cast<uint32_t>(std::min<size_t>(
            growing == 0 ? mesh.indices.size() : target.index_count + indices_short,
            kMaxDrawIndices));
        if (growing == 0 && mesh.indices.size() > kMaxDrawIndices) {
            say("the mesh states " + std::to_string(mesh.indices.size()) +
                " indices; one submesh can be asked for at most " +
                std::to_string(kMaxDrawIndices) + ", so the rest is dealt to the other slots");
        }


        if (want_vertices > target.vertex_count || want_indices > target.index_count) {
            // Would this slot take the resource past what the game itself ever
            // streams?
            //
            // Then it takes what is left rather than nothing. Refusing the
            // growth outright leaves the slot at the size it shipped with --
            // fifteen hundred vertices on a car body -- and the mesh is welded
            // down to that, which throws away far more than the ceiling asked
            // for. Vertices and indices are cut by the same fraction, because a
            // triangle needs both and starving one wastes the other.
            if (offset.grow_ceiling != 0) {
                const uint32_t stride = target.layout.stride;
                const uint64_t floor = uint64_t(resource.virtual_size) + resource.physical_size +
                                       offset.grow_slack + 512;
                const uint64_t room =
                    floor < offset.grow_ceiling ? offset.grow_ceiling - floor : 0;
                const uint64_t need =
                    uint64_t(want_vertices) * stride + uint64_t(want_indices) * 2;
                if (need > room) {
                    const double share = room ? double(room) / double(need) : 0.0;
                    want_vertices = std::max<uint32_t>(
                        target.vertex_count, static_cast<uint32_t>(want_vertices * share));
                    want_indices = std::max<uint32_t>(
                        target.index_count, static_cast<uint32_t>(want_indices * share));
                    want_indices -= want_indices % 3;
                    say("slot " + std::to_string(growing) + " capped to " +
                        std::to_string(want_vertices) + " vertices and " +
                        std::to_string(want_indices) + " indices by the " +
                        std::to_string(offset.grow_ceiling) + "-byte ceiling");
                    if (want_vertices <= target.vertex_count &&
                        want_indices <= target.index_count) {
                        continue;
                    }
                }
            }
            // Which segment the new buffers belong in is not a choice: they go
            // where this submesh's buffers already are. A character and a wheel
            // keep theirs in the physical segment; a vehicle part has no
            // physical segment at all and keeps everything virtual.
            uint32_t existing = 0;
            if (!view.U32(target.vertex_buffer + 24, existing) || existing == 0) {
                error = "cannot tell which segment the submesh's buffers live in";
                return false;
            }
            const bool physical = (existing & 0xF0000000u) == kPhysicalBase;
            const uint32_t segment = physical ? resource.physical_size : resource.virtual_size;

            // Probe 2 relocates what the template already holds rather than
            // making room for the mod, so the only thing being tested is
            // whether the new space can be written to and fetched from.
            const uint32_t place_vertices =
                offset.grow_probe == 2 ? target.vertex_count : want_vertices;
            const uint32_t place_indices =
                offset.grow_probe == 2 ? target.index_count : want_indices;
            const uint32_t vertex_bytes = place_vertices * target.layout.stride;
            const uint32_t index_bytes = place_indices * 2;
            constexpr uint32_t kAlign = 256;

            const bool in_place = false;
            const uint32_t base =
                in_place ? kAlign : (segment + kAlign - 1) / kAlign * kAlign;

            // A buffer may not run past the end of the block it starts in.
            //
            // The resource is split into blocks of halving powers of two
            // (sub_821E57B8) and each block is allocated on its own
            // (sub_821E58F0, measured in game: the byte it branches on is zero,
            // so never one allocation). A pointer is fixed up by finding its
            // block and adding that block's delta, so bytes past the block's end
            // come from whatever else the heap put there. The game's own bodies
            // obey this: of 123 body drawables, 117 have no buffer crossing at
            // all, and the six that do cross once with a buffer bigger than a
            // block. Ours crossed nine times out of fourteen.
            const uint32_t block = offset.block_align;
            auto fits = [&](uint32_t at, uint32_t bytes) {
                if (block == 0 || bytes == 0) return true;
                return at / block == (at + bytes - 1) / block;
            };
            auto place = [&](uint32_t at, uint32_t bytes) {
                if (fits(at, bytes)) return at;
                const uint32_t next = (at / block + 1) * block;
                // A buffer larger than a block cannot be placed at all; leaving
                // it where it was keeps the waste down and the diagnosis below
                // names it.
                return fits(next, bytes) ? next : at;
            };

            const uint32_t vertex_at = place(base, vertex_bytes);
            const uint32_t index_at =
                place((vertex_at + vertex_bytes + kAlign - 1) / kAlign * kAlign, index_bytes);
            if (block != 0 && (!fits(vertex_at, vertex_bytes) || !fits(index_at, index_bytes))) {
                say("submesh does not fit in one " + std::to_string(block) +
                    "-byte block (vertex buffer " + std::to_string(vertex_bytes) +
                    " bytes, index buffer " + std::to_string(index_bytes) +
                    ") -- it will be read across two allocations");
            }

            if (in_place) {
                if (index_at + index_bytes > segment) {
                    error = "the segment has no room to move the buffers within it";
                    return false;
                }
            } else {
                std::string grow_error;
                const uint32_t grow_by = index_at + index_bytes + offset.grow_slack - segment;
                const bool grown = physical ? GrowPhysicalSegment(resource, grow_by, grow_error)
                                            : GrowVirtualSegment(resource, grow_by, grow_error);
                if (!grown) {
                    error = "cannot grow the resource for the mesh: " + grow_error;
                    return false;
                }
            }

            // Probe: grow and stop.
            //
            // Growing and repointing have always been tried together, which is
            // why the first attempt could be written off without saying which
            // of the two was at fault. Here the segment gets bigger and nothing
            // else changes -- no buffer moves, no pointer is rewritten, no count
            // is touched, and the mesh goes on to be decimated into the buffers
            // the template already owned. If that renders, a resource can be
            // made larger safely and the fault is in the repointing. If it does
            // not, growth alone is the fault and the rest never mattered.
            if (diagnose) page_note("grown ");
            if (offset.grow_probe) {
                say("grow probe " + std::to_string(offset.grow_probe) +
                    ": segment grown by " +
                    std::to_string(index_at + index_bytes + offset.grow_slack - segment) +
                    " bytes (" + std::to_string(offset.grow_slack) + " of it slack)" +
                    (offset.grow_probe == 1 ? ", nothing repointed, counts untouched"
                                            : ", buffers repointed, counts untouched"));
            }

            const uint32_t space = physical ? kPhysicalBase : kVirtualBase;
            if (offset.grow_probe == 1) {
                // Probe 1 holds back the whole second half of the change.
                (void)space;
            } else {
            const uint32_t vertex_address = space | vertex_at;
            const uint32_t index_address = space | index_at;

            // The vertex buffer states its data twice, at +8 and at +24, and the
            // draw reads neither -- it reads word 6 of the GPU descriptor, which
            // the vertex flavour ORs an endian field of 3 into.
            bool ok = view.SetU32(target.vertex_buffer + 8, vertex_address) &&
                      view.SetU32(target.vertex_buffer + 24, vertex_address) &&
                      view.SetU32(target.index_buffer + 8, index_address);
            uint32_t gpu = 0;
            ok = ok && view.U32(target.vertex_buffer + 28, gpu) && gpu != 0 &&
                 view.SetU32(gpu + kGpuHeaderAddressWord, vertex_address | 3u);
            ok = ok && view.U32(target.index_buffer + 12, gpu) && gpu != 0 &&
                 view.SetU32(gpu + kGpuHeaderAddressWord, index_address);
            if (!ok) {
                error = "cannot repoint the grown submesh at its new buffers";
                return false;
            }

            if (offset.grow_probe != 2) {
                target.vertex_count = want_vertices;
                target.index_count = want_indices;
            }
            }
        }
        have_vertices += target.vertex_count - had_vertices;
        have_indices += target.index_count - had_indices;
      }
    }

    size_t vertex_budget = 0, index_budget = 0;
    for (const GeometryRef& slot : geometries) {
        vertex_budget += slot.vertex_count;
        index_budget += slot.index_count;
    }

    const size_t before = mesh.vertices.size();
    if (!offset.decimate &&
        (mesh.vertices.size() > vertex_budget || mesh.indices.size() > index_budget)) {
        error = "mesh needs " + std::to_string(mesh.vertices.size()) + " vertices and " +
                std::to_string(mesh.indices.size()) + " indices, the drawable holds " +
                std::to_string(vertex_budget) + " and " + std::to_string(index_budget) +
                " across " + std::to_string(geometries.size()) +
                " submesh(es); decimation is off, so the shipped model is kept";
        return false;
    }

    const bool skinned = retargeted;

    // Which bone a mesh that cannot be skinned rides.
    //
    // `bone` is a request, and two of its values are sentinels rather than bone
    // numbers: kAutomaticBone asks for the weights the model brought, and
    // kSlotOneTest is a diagnostic. Either can reach here -- a glTF whose
    // primitives carry no JOINTS_0 has nothing to skin with, however complete
    // its skeleton looks -- and truncating a sentinel to sixteen bits writes
    // bone 65535 into the palette. The matrix lookup then runs off the end of a
    // 92-bone skeleton and the whole character disappears, having reported
    // nothing wrong. Bone 0, the root, is the one the lift below is written for.
    uint32_t rigid_bone = slot_test ? 1u : (bone == kAutomaticBone ? 0u : bone);

    // Lift the mesh onto that bone.
    //
    // The bone's own bind translation is the size of the correction: bone 0 sits
    // at hip height. `offset` is on top of it, so the remainder can be dialled in
    // without a rebuild.
    float lift[3] = {0.0f, 0.0f, 0.0f};
    if (!skinned) {
        uint32_t skeleton = 0, bind_matrices = 0;
        uint16_t skeleton_bones = 0;
        if (view.U32(drawable.drawable + drawable.skeleton_field, skeleton) && skeleton != 0 &&
            view.U32(skeleton + 16, bind_matrices) && bind_matrices != 0 &&
            view.U16(skeleton + 20, skeleton_bones) && skeleton_bones != 0) {
            // A bone asked for by number has to exist, for the same reason.
            if (rigid_bone >= skeleton_bones) rigid_bone = 0;
            const uint32_t row = bind_matrices + rigid_bone * 64 + 48;
            for (int i = 0; i < 3; ++i) view.Float(row + i * 4, lift[i]);
        }
    }
    for (auto& vertex : mesh.vertices) {
        vertex.px += lift[0] + offset.x;
        vertex.py += lift[1] + offset.y;
        vertex.pz += lift[2] + offset.z;
    }

    // Which bones each vertex rides, as skeleton indices. Palette slots cannot be
    // decided yet: each submesh gets its own palette, holding only the bones its
    // own share of the mesh actually uses.
    std::vector<std::array<uint16_t, 3>> vertex_bone;
    std::vector<std::array<uint8_t, 3>> vertex_weights;  // 0..255, summing to 255

    // Dealing the mesh out is a fitting problem, not just a size one: a submesh
    // caps its palette as well as its buffers, so a share can be refused for
    // needing a twenty-ninth bone even with room to spare. When that leaves
    // triangles over, the mesh is decimated a little harder and dealt again,
    // starting from the undecimated copy so the welding never compounds.
    const Mesh pristine = mesh;
    struct Chunk {
        size_t slot = 0;
        std::vector<uint32_t> vertices;  // mesh vertex ids
        std::vector<uint32_t> indices;   // into `vertices`
        std::vector<uint16_t> palette;   // skeleton bone ids
    };
    std::vector<Chunk> chunks;
    size_t target_vertices = vertex_budget;

    for (int attempt = 0;; ++attempt) {
    mesh = pristine;
    if (!SimplifyMeshToFit(mesh, target_vertices, index_budget)) {
        error = "mesh cannot be reduced to " + std::to_string(target_vertices) +
                " vertices / " + std::to_string(index_budget) + " indices";
        return false;
    }
    chunks.clear();

    if (retargeted) {
        vertex_bone.assign(mesh.vertices.size(), {0, 0, 0});
        vertex_weights.assign(mesh.vertices.size(), {0, 0, 0});
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            std::vector<std::pair<float, uint16_t>> influences;
            for (int c = 0; c < 4; ++c) {
                const MeshSkin& skin = mesh.skin[i];
                if (skin.weight[c] <= 0.0f || skin.joint[c] >= joint_to_bone.size()) continue;
                const int mapped = joint_to_bone[skin.joint[c]];
                if (mapped < 0) continue;
                influences.emplace_back(skin.weight[c], static_cast<uint16_t>(mapped));
            }
            std::sort(influences.begin(), influences.end(), std::greater<>());
            if (influences.empty()) influences.emplace_back(1.0f, static_cast<uint16_t>(0));
            if (influences.size() > 3) influences.resize(3);

            float total = 0.0f;
            for (const auto& [weight, id] : influences) total += weight;
            if (total <= 0.0f) total = 1.0f;

            int assigned = 0;
            for (size_t c = 0; c < influences.size(); ++c) {
                vertex_bone[i][c] = influences[c].second;
                vertex_weights[i][c] =
                    static_cast<uint8_t>(influences[c].first / total * 255.0f + 0.5f);
                assigned += vertex_weights[i][c];
            }
            // Hand any rounding leftovers to the dominant influence.
            vertex_weights[i][0] = static_cast<uint8_t>(
                std::min(255, std::max(0, vertex_weights[i][0] + (255 - assigned))));
        }
    } else {
        // One bone for everything, so every submesh's palette is a single entry.
        vertex_bone.assign(mesh.vertices.size(),
                           {static_cast<uint16_t>(rigid_bone), 0, 0});
        vertex_weights.assign(mesh.vertices.size(), {0xFF, 0, 0});
    }

    // Deal the triangles out across the submeshes, in bone order so a run of them
    // belongs to the same limb and a slot fills with a body part rather than a
    // scattering. A triangle that would overflow a slot's palette is left for the
    // next slot instead of being forced in.
    const size_t triangle_count = mesh.indices.size() / 3;
    std::vector<uint32_t> order(triangle_count);
    for (size_t i = 0; i < triangle_count; ++i) order[i] = static_cast<uint32_t>(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        auto dominant = [&](uint32_t triangle) {
            uint16_t lowest = 0xFFFF;
            for (int c = 0; c < 3; ++c)
                lowest = std::min(lowest, vertex_bone[mesh.indices[triangle * 3 + c]][0]);
            return lowest;
        };
        const uint16_t da = dominant(a), db = dominant(b);
        if (da != db) return da < db;
        return a < b;
    });

    std::vector<bool> placed(triangle_count, false);
    size_t remaining = triangle_count;

    for (size_t slot = 0; slot < geometries.size() && remaining > 0; ++slot) {
        const GeometryRef& target = geometries[slot];
        if (target.vertex_count == 0 || target.index_count < 3) continue;
        const size_t bone_room = std::max<size_t>(1, target.bone_count);

        Chunk chunk;
        chunk.slot = slot;
        std::map<uint32_t, uint32_t> local;   // mesh vertex -> chunk vertex
        std::map<uint16_t, uint8_t> in_palette;

        for (uint32_t triangle : order) {
            if (placed[triangle]) continue;
            if (chunk.indices.size() + 3 > target.index_count) break;

            // What this triangle would cost the slot.
            std::vector<uint32_t> fresh;
            std::vector<uint16_t> new_bones;
            for (int c = 0; c < 3; ++c) {
                const uint32_t v = mesh.indices[triangle * 3 + c];
                if (!local.count(v) &&
                    std::find(fresh.begin(), fresh.end(), v) == fresh.end()) {
                    fresh.push_back(v);
                }
                for (int b = 0; b < 3; ++b) {
                    if (vertex_weights[v][b] == 0 && b != 0) continue;
                    const uint16_t id = vertex_bone[v][b];
                    if (in_palette.count(id) ||
                        std::find(new_bones.begin(), new_bones.end(), id) != new_bones.end()) {
                        continue;
                    }
                    new_bones.push_back(id);
                }
            }
            if (chunk.vertices.size() + fresh.size() > target.vertex_count) continue;
            if (in_palette.size() + new_bones.size() > bone_room) continue;

            for (uint32_t v : fresh) {
                local.emplace(v, static_cast<uint32_t>(chunk.vertices.size()));
                chunk.vertices.push_back(v);
            }
            for (uint16_t id : new_bones) {
                in_palette.emplace(id, static_cast<uint8_t>(chunk.palette.size()));
                chunk.palette.push_back(id);
            }
            for (int c = 0; c < 3; ++c)
                chunk.indices.push_back(local[mesh.indices[triangle * 3 + c]]);

            placed[triangle] = true;
            --remaining;
        }

        if (!chunk.indices.empty()) chunks.push_back(std::move(chunk));
    }

    if (remaining == 0 && !chunks.empty()) break;

    if (!offset.decimate || attempt >= 8 || target_vertices <= 64) {
        error = "mesh does not fit the drawable's submeshes: " + std::to_string(remaining) +
                " of " + std::to_string(triangle_count) + " triangles left over" +
                (offset.decimate ? "" : " and decimation is off");
        return false;
    }
    target_vertices = target_vertices * 4 / 5;
    }  // retry

    if (stats) {
        size_t written = 0;
        for (const Chunk& chunk : chunks) written += chunk.vertices.size();
        stats->vertices = static_cast<uint32_t>(written);
        stats->triangles = static_cast<uint32_t>(mesh.indices.size() / 3);
        stats->decimated = mesh.vertices.size() != before;
        stats->submeshes = static_cast<uint32_t>(chunks.size());
        if (skinned) {
            size_t widest = 0;
            for (const Chunk& chunk : chunks) widest = std::max(widest, chunk.palette.size());
            stats->bones = static_cast<uint32_t>(widest);
            stats->first_bone = chunks.front().palette.empty() ? 0 : chunks.front().palette[0];
        }
    }

    if (diagnose) {
        say("");
        say("dealt into " + std::to_string(chunks.size()) + " of " +
            std::to_string(geometries.size()) + " writable submesh(es), " +
            std::to_string(mesh.vertices.size()) + " vertices / " +
            std::to_string(mesh.indices.size() / 3) + " triangles after " +
            (mesh.vertices.size() != before ? "welding" : "no welding") +
            (skinned ? ", skinned" : ", rigid on bone " + std::to_string(rigid_bone)));
        for (const Chunk& chunk : chunks) {
            const GeometryRef& target = geometries[chunk.slot];
            std::string palette;
            for (uint16_t id : chunk.palette) palette += (palette.empty() ? "" : ",") +
                                                         std::to_string(id);
            say("  slot " + std::to_string(chunk.slot) + " (" + Hex(target.address) + ", holds " +
                std::to_string(target.vertex_count) + "v/" + std::to_string(target.index_count) +
                "i/" + std::to_string(target.bone_count) + "b): " +
                std::to_string(chunk.vertices.size()) + " vertices, " +
                std::to_string(chunk.indices.size() / 3) + " triangles, palette [" + palette + "]");
        }

        // The mesh exactly as it is about to be written, one .obj group per
        // submesh, so a chunk that took the wrong triangles shows up as a group
        // scattered across the body rather than a body part.
        std::string obj;
        size_t emitted = 0;
        for (const Chunk& chunk : chunks) {
            for (uint32_t id : chunk.vertices) {
                const MeshVertex& vertex = mesh.vertices[id];
                obj += "v " + Number(vertex.px) + " " + Number(vertex.py) + " " +
                       Number(vertex.pz) + "\n";
            }
            obj += "g slot" + std::to_string(chunk.slot) + "\n";
            for (size_t i = 0; i + 2 < chunk.indices.size(); i += 3) {
                obj += "f";
                for (int c = 0; c < 3; ++c)
                    obj += " " + std::to_string(emitted + chunk.indices[i + c] + 1);
                obj += "\n";
            }
            emitted += chunk.vertices.size();
        }
        stats->obj_after = std::move(obj);
    }

    // Tangents and shade are per vertex of the whole mesh, so they are worked out
    // once and then read through each chunk's own vertex list.
    std::vector<Vec3> tangents;
    ComputeTangents(mesh, tangents);

    std::vector<uint32_t> shade(mesh.vertices.size(), 0xFFFFFFFFu);
    std::vector<uint32_t> shade_texcoord1(mesh.vertices.size(), 0);
    // The one texel a flooded UV points at: the value the submesh being written
    // into uses for most of itself. Read by address, like the band above, since
    // the list is sorted by size after it was gathered.
    uint32_t uniform_texcoord0 = 0;
    if (offset.uniform_uv && !shipped.empty() && !geometries.empty()) {
        const uint32_t target = geometries.front().address;
        std::map<uint32_t, size_t> seen;
        for (const Shade& entry : shipped) {
            if (entry.geometry != target) continue;
            ++seen[entry.texcoord0];
        }
        if (seen.empty()) {
            for (const Shade& entry : shipped) ++seen[entry.texcoord0];
        }
        size_t best = 0;
        for (const auto& [value, count] : seen) {
            if (count > best) {
                best = count;
                uniform_texcoord0 = value;
            }
        }
    }

    if (offset.uniform_shade && !shipped.empty()) {
        // The band the template uses for most of itself, and that band's own
        // average shade. Both come out of the template rather than being
        // invented: a wheel's colour word is 0xXX000000 and a character's is
        // 0xXXFFFFFF, and the shade lane sits at very different levels from one
        // band to the next -- 145 on the spoke face against 213 on the lip --
        // so a single made-up constant would be wrong for whichever band it was
        // not taken from.
        // Read off the submesh that is actually being written into, not the
        // resource as a whole. A car body keeps glass and chrome in submeshes of
        // their own, and taking the most-used band across all of them can hand
        // the paintwork whichever of those happened to have the most vertices --
        // which is a body panel lit as a window.
        // The submesh is identified by address, not by position: the list is
        // sorted by size after these were read, so an index would point at
        // whichever submesh happened to be first before the sort.
        const uint32_t target = geometries.front().address;
        bool from_target = false;
        for (const Shade& entry : shipped) from_target |= entry.geometry == target;

        auto counts = [&](bool restrict_to_target) {
            std::map<uint32_t, size_t> bands;
            for (const Shade& entry : shipped) {
                if (restrict_to_target && entry.geometry != target) continue;
                ++bands[entry.texcoord1];
            }
            return bands;
        };
        const std::map<uint32_t, size_t> bands = counts(from_target);

        uint32_t band = 0;
        size_t best = 0;
        for (const auto& [value, count] : bands) {
            if (count > best) {
                best = count;
                band = value;
            }
        }

        // The tint belongs to the band, so it is read from the band. The shade
        // level is not: it is one number standing in for the whole replacement,
        // and reading it off the band alone samples about half the wheel.
        // Measured over all 175, the dominant band covers 49% of the vertices
        // and averages 6.6 darker than the wheel it belongs to, and on the 350z
        // it is 145 against the wheel's 174 from a third of the vertices. That
        // difference is a rim flooded with a value a sixth darker than anything
        // the wheel ever was -- which is most of "the mod came out dark and the
        // only thing left on it is the specular".
        std::map<uint32_t, size_t> tints;
        uint64_t sum = 0;
        size_t count = 0;
        for (const Shade& entry : shipped) {
            if (from_target && entry.geometry != target) continue;
            if (entry.texcoord1 == band) ++tints[entry.colour & 0x00FFFFFFu];
            sum += entry.colour >> 24;
            ++count;
        }
        uint32_t tint = 0x00FFFFFFu;
        best = 0;
        for (const auto& [value, seen] : tints) {
            if (seen > best) {
                best = seen;
                tint = value;
            }
        }

        const uint32_t level = count ? static_cast<uint32_t>(sum / count) : 0xFFu;
        std::fill(shade.begin(), shade.end(), (level << 24) | tint);
        std::fill(shade_texcoord1.begin(), shade_texcoord1.end(), band);

        // See MeshOffset::nearest_band. Every shipped vertex of this pass is a
        // candidate, not only the submesh being written: on the Caprice the
        // headlights (2, 3) and the tail lamps (0, 6) are two submeshes and the
        // replacement is written into one of them.
        if (offset.nearest_band) {
            // Which material each vertex came from, for lamp boxes that name one.
            std::vector<const std::string*> material_of(mesh.vertices.size(), nullptr);
            for (const MeshPart& part : mesh.parts) {
                for (uint32_t v = 0; v < part.vertex_count; ++v) {
                    if (part.first_vertex + v < material_of.size())
                        material_of[part.first_vertex + v] = &part.material;
                }
            }
            for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                const MeshVertex& vertex = mesh.vertices[i];
                float nearest = std::numeric_limits<float>::max();
                for (const Shade& candidate : shipped) {
                    const float dx = candidate.x - vertex.px;
                    const float dy = candidate.y - vertex.py;
                    const float dz = candidate.z - vertex.pz;
                    const float distance = dx * dx + dy * dy + dz * dz;
                    if (distance < nearest) {
                        nearest = distance;
                        shade_texcoord1[i] = candidate.texcoord1;
                    }
                }
                for (const BandSample& candidate : offset.extra_bands) {
                    const float dx = candidate.x - vertex.px;
                    const float dy = candidate.y - vertex.py;
                    const float dz = candidate.z - vertex.pz;
                    const float distance = dx * dx + dy * dy + dz * dz;
                    if (distance < nearest) {
                        nearest = distance;
                        shade_texcoord1[i] = candidate.texcoord1;
                    }
                }
                // A stated region wins. texcoord1 is Half2, x in the high half;
                // the low half (y, 1.0 on every shipped lamp) is kept.
                const float ax = std::fabs(vertex.px);
                for (const LampBox& box : offset.lamp_boxes) {
                    if (!box.material.empty() && box.material != "*" &&
                        (!material_of[i] || *material_of[i] != box.material)) {
                        continue;
                    }
                    if (ax < box.min[0] || ax > box.max[0] || vertex.py < box.min[1] ||
                        vertex.py > box.max[1] || vertex.pz < box.min[2] ||
                        vertex.pz > box.max[2]) {
                        continue;
                    }
                    shade_texcoord1[i] =
                        (static_cast<uint32_t>(FloatToHalf(static_cast<float>(box.index))) << 16) |
                        (shade_texcoord1[i] & 0xFFFFu);
                    break;
                }
            }
        }

        // Put the template's material bands back, ring by ring.
        //
        // See MeshOffset::band_profile for the measurement this rests on. The
        // flood above stays as the floor: a vertex that matches no ring keeps
        // the dominant band, which is the safe one.
        //
        // Only bands that ARE rings are carried. Measured on two unrelated
        // shipped wheels the four bands split cleanly in two by how far they
        // spread in the (radius, depth) plane, normalised to the wheel's own
        // extent, between the 5th and 95th percentile:
        //
        //   band   bbs_ch               skyline_99
        //   0      r span 0.80          r span 0.81      the face, not a ring
        //   1      r 0.06 depth 0.05    r 0.06 depth 0.03   RING, the bead step
        //   2      r 0.07 depth 0.04    r 0.12 depth 0.11   RING, the outer lip
        //   3      r 0.82 depth 0.97    r 0.90 depth 0.96   scattered accents
        //
        // 0.12 against 0.80 is not a close call, so the cut sits at a quarter.
        // Excluding the scattered band matters for more than tidiness: it is
        // the one that carries the emissive material, and a plain nearest
        // neighbour hands it 23% of the BMW's rim -- which is the neon wheel
        // that model_mods_rim_inherit_shade produced.
        //
        // The axle is x, so a ring is a band in (radius, depth) and nothing
        // else: a wheel is rotationally symmetric, so the angle carries no
        // information and using it would make the match depend on how the two
        // meshes happen to be clocked.
        if (offset.band_profile) {
            constexpr float kRingSpread = 0.25f;   // above this it is not a ring
            constexpr float kLowQuantile = 0.05f;  // trimmed, so one stray vertex
            constexpr float kHighQuantile = 0.95f; // cannot widen a ring

            struct Sample {
                float radius, depth;
            };
            std::map<uint32_t, std::vector<Sample>> by_band;
            float t_radius = 0.0f, t_low = 0.0f, t_high = 0.0f;
            bool t_any = false;
            for (const Shade& entry : shipped) {
                if (from_target && entry.geometry != target) continue;
                const float radius = std::sqrt(entry.y * entry.y + entry.z * entry.z);
                t_radius = std::max(t_radius, radius);
                if (!t_any) {
                    t_low = t_high = entry.x;
                    t_any = true;
                } else {
                    t_low = std::min(t_low, entry.x);
                    t_high = std::max(t_high, entry.x);
                }
                by_band[entry.texcoord1].push_back({radius, entry.x});
            }

            const float t_span = t_high - t_low;
            struct Ring {
                float radius_low, radius_high, depth_low, depth_high;
                uint32_t band;
            };
            std::vector<Ring> rings;
            if (t_any && t_radius > 1e-6f && t_span > 1e-6f) {
                for (auto& [value, samples] : by_band) {
                    if (value == band || samples.size() < 8) continue;
                    std::vector<float> radii, depths;
                    radii.reserve(samples.size());
                    depths.reserve(samples.size());
                    for (const Sample& sample : samples) {
                        radii.push_back(sample.radius / t_radius);
                        depths.push_back((sample.depth - t_low) / t_span);
                    }
                    std::sort(radii.begin(), radii.end());
                    std::sort(depths.begin(), depths.end());
                    auto at = [](const std::vector<float>& sorted, float quantile) {
                        const size_t index = static_cast<size_t>(
                            quantile * static_cast<float>(sorted.size() - 1));
                        return sorted[index];
                    };
                    const Ring ring{at(radii, kLowQuantile), at(radii, kHighQuantile),
                                    at(depths, kLowQuantile), at(depths, kHighQuantile), value};
                    if (ring.radius_high - ring.radius_low > kRingSpread) continue;
                    if (ring.depth_high - ring.depth_low > kRingSpread) continue;
                    rings.push_back(ring);
                }
            }

            float m_radius = 0.0f, m_low = 0.0f, m_high = 0.0f;
            for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                const MeshVertex& vertex = mesh.vertices[i];
                m_radius = std::max(m_radius,
                                    std::sqrt(vertex.py * vertex.py + vertex.pz * vertex.pz));
                if (!i) {
                    m_low = m_high = vertex.px;
                } else {
                    m_low = std::min(m_low, vertex.px);
                    m_high = std::max(m_high, vertex.px);
                }
            }

            const float m_span = m_high - m_low;
            if (!rings.empty() && m_radius > 1e-6f && m_span > 1e-6f) {
                for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                    const MeshVertex& vertex = mesh.vertices[i];
                    const float radius =
                        std::sqrt(vertex.py * vertex.py + vertex.pz * vertex.pz) / m_radius;
                    const float depth = (vertex.px - m_low) / m_span;
                    // Nearest ring CENTRE among the rings the vertex is inside,
                    // so a vertex in the overlap of two goes to the one it sits
                    // deeper in rather than to whichever was measured first.
                    float best = std::numeric_limits<float>::max();
                    for (const Ring& ring : rings) {
                        if (radius < ring.radius_low || radius > ring.radius_high) continue;
                        if (depth < ring.depth_low || depth > ring.depth_high) continue;
                        const float dr = radius - (ring.radius_low + ring.radius_high) * 0.5f;
                        const float dd = depth - (ring.depth_low + ring.depth_high) * 0.5f;
                        const float distance = dr * dr + dd * dd;
                        if (distance < best) {
                            best = distance;
                            shade_texcoord1[i] = ring.band;
                        }
                    }
                }
            }
        }
        if (stats) {
            std::set<uint32_t> distinct(shade_texcoord1.begin(), shade_texcoord1.end());
            stats->bands = static_cast<uint32_t>(distinct.size());
        }

        // The band stays flooded either way -- that is what keeps a vertex off
        // the emissive material and is the whole reason uniform_shade exists.
        // Only the LEVEL is replaced, by occlusion measured on this mesh, and
        // it is centred on the template's own average so a wheel comes out as
        // bright overall as the one it replaces.
        if (offset.shade_occlusion) {
            std::vector<uint8_t> occlusion;
            BakeOcclusion(mesh, occlusion);
            double total = 0.0;
            for (uint8_t v : occlusion) total += v;
            const double middle = occlusion.empty() ? 0.0 : total / occlusion.size();
            double baked_sum = 0.0, baked_square = 0.0;
            uint32_t baked_low = 255, baked_high = 0;
            size_t baked = 0;
            for (size_t i = 0; i < shade.size() && i < occlusion.size(); ++i) {
                const double pulled =
                    middle + (occlusion[i] - middle) * kOcclusionContrast;
                const double centred = pulled - middle + static_cast<double>(level);
                const uint32_t value = static_cast<uint32_t>(
                    std::clamp(centred, 0.0, 255.0));
                shade[i] = (value << 24) | tint;
                baked_sum += value;
                baked_square += double(value) * value;
                baked_low = std::min(baked_low, value);
                baked_high = std::max(baked_high, value);
                ++baked;
            }
            if (stats && baked) {
                const double mean = baked_sum / baked;
                stats->shade_low = baked_low;
                stats->shade_high = baked_high;
                stats->shade_mean = static_cast<float>(mean);
                stats->shade_deviation =
                    static_cast<float>(std::sqrt(std::max(0.0, baked_square / baked - mean * mean)));
            }
        }

        // Give the shade lane its range back without giving it the old wheel's
        // spokes.
        //
        // One average for the whole mesh is what stops the old shadows being
        // printed on the new rim, and it costs everything else: measured over
        // five shipped wheels the baked lane runs the full 0..255 with a
        // deviation above fifty, and a constant accounts for none of it. What
        // is left is a rim with no cavities, so the only shading that survives
        // is the material's own specular -- which is fixed to the geometry and
        // therefore sweeps around as the wheel turns, and blows out the moment
        // the rim is painted.
        //
        // So the lane is rebuilt from the template through coordinates that
        // cannot carry a spoke: distance from the axle, depth along it, and how
        // the surface faces in each. Binned on those four the shipped lane
        // predicts itself to R^2 0.82, and the table read with the mod's own
        // coordinates gives it structure of the right kind at roughly 60% of
        // the shipped amplitude.
        //
        // What this is not is a reconstruction, and the measurement says so:
        // fit on one wheel and scored against another's bake it lands at R^2
        // -0.09, no better than the flat average, at every grid size tried
        // between 8x8 and 16x16x8x8. Half a wheel's bake is its own spokes and
        // that half is not transferable -- there is no bake for geometry the
        // game never saw. This trades a lane that is provably featureless for
        // one that is plausible, and the cvar exists because that is a choice.
        if (offset.shade_profile) {
            constexpr int kSpace = 16;  // radius and depth
            constexpr int kFacing = 8;  // the two normal components
            constexpr size_t kBins = kSpace * kSpace * kFacing * kFacing;

            float min_radius = 0.0f, max_radius = 0.0f;
            float min_depth = 0.0f, max_depth = 0.0f;
            bool first = true;
            for (const Shade& entry : shipped) {
                const float radius = std::sqrt(entry.y * entry.y + entry.z * entry.z);
                if (first) {
                    min_radius = max_radius = radius;
                    min_depth = max_depth = entry.x;
                    first = false;
                    continue;
                }
                min_radius = std::min(min_radius, radius);
                max_radius = std::max(max_radius, radius);
                min_depth = std::min(min_depth, entry.x);
                max_depth = std::max(max_depth, entry.x);
            }

            // Where a point falls, at four widths at once: the exact bin, then
            // the same bin with each facing term dropped, so an empty cell
            // answers from the coarsest table that has anything in it.
            auto locate = [&](float x, float y, float z, float nx, float ny, float nz,
                              size_t& fine, size_t& medium, size_t& coarse) {
                auto bucket = [](float value, float low, float high, int count) -> int {
                    const float span = high - low;
                    if (span <= 1e-9f) return 0;
                    const int at = static_cast<int>((value - low) / span * static_cast<float>(count));
                    return std::max(0, std::min(count - 1, at));
                };
                const float radius = std::sqrt(y * y + z * z);
                const int r = bucket(radius, min_radius, max_radius, kSpace);
                const int d = bucket(x, min_depth, max_depth, kSpace);
                // How the surface faces along the axle, and how it faces away
                // from it -- the second is what tells the outside of a spoke
                // from the wall of the pocket beside it.
                const float outward = radius > 1e-6f ? (ny * y + nz * z) / radius : 0.0f;
                const int along = bucket(nx, -1.0f, 1.0f, kFacing);
                const int out = bucket(outward, -1.0f, 1.0f, kFacing);
                coarse = static_cast<size_t>(r) * kSpace + static_cast<size_t>(d);
                medium = coarse * kFacing + static_cast<size_t>(along);
                fine = medium * kFacing + static_cast<size_t>(out);
            };

            std::vector<uint32_t> fine_sum(kBins, 0), fine_count(kBins, 0);
            std::vector<uint32_t> medium_sum(kSpace * kSpace * kFacing, 0);
            std::vector<uint32_t> medium_count(kSpace * kSpace * kFacing, 0);
            std::vector<uint32_t> coarse_sum(kSpace * kSpace, 0), coarse_count(kSpace * kSpace, 0);

            for (const Shade& entry : shipped) {
                if (from_target && entry.geometry != target) continue;
                size_t fine = 0, medium = 0, coarse = 0;
                locate(entry.x, entry.y, entry.z, entry.nx, entry.ny, entry.nz, fine, medium,
                       coarse);
                const uint32_t lane = entry.colour >> 24;
                fine_sum[fine] += lane;
                ++fine_count[fine];
                medium_sum[medium] += lane;
                ++medium_count[medium];
                coarse_sum[coarse] += lane;
                ++coarse_count[coarse];
            }

            for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                const MeshVertex& vertex = mesh.vertices[i];
                size_t fine = 0, medium = 0, coarse = 0;
                locate(vertex.px, vertex.py, vertex.pz, vertex.nx, vertex.ny, vertex.nz, fine,
                       medium, coarse);

                uint32_t lane = level;
                if (fine_count[fine] != 0) {
                    lane = fine_sum[fine] / fine_count[fine];
                } else if (medium_count[medium] != 0) {
                    lane = medium_sum[medium] / medium_count[medium];
                } else if (coarse_count[coarse] != 0) {
                    lane = coarse_sum[coarse] / coarse_count[coarse];
                }
                shade[i] = (std::min(lane, 0xFFu) << 24) | tint;
            }
        }
    } else {
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const MeshVertex& vertex = mesh.vertices[i];
            bool found = false;
            float nearest = 0.0f;
            for (const Shade& candidate : shipped) {
                const float dx = candidate.x - vertex.px;
                const float dy = candidate.y - vertex.py;
                const float dz = candidate.z - vertex.pz;
                const float distance = dx * dx + dy * dy + dz * dz;
                if (!found || distance < nearest) {
                    found = true;
                    nearest = distance;
                    shade[i] = candidate.colour;
                    shade_texcoord1[i] = candidate.texcoord1;
                }
            }
        }
    }

    Rsc5View patched(resource.data, resource.virtual_size);
    std::vector<bool> used(geometries.size(), false);

    for (const Chunk& chunk : chunks) {
        const GeometryRef& target = geometries[chunk.slot];
        used[chunk.slot] = true;

        const VertexLayout& layout = target.layout;
        std::vector<uint8_t> vertex_bytes(chunk.vertices.size() * layout.stride, 0);
        for (size_t i = 0; i < chunk.vertices.size(); ++i) {
            const uint32_t source = chunk.vertices[i];
            const MeshVertex& vertex = mesh.vertices[source];
            uint8_t* out = vertex_bytes.data() + i * layout.stride;

            uint8_t* at = out + layout.offset[kSemPosition];
            StoreBEFloat(at + 0, vertex.px);
            StoreBEFloat(at + 4, vertex.py);
            StoreBEFloat(at + 8, vertex.pz);

            // Blend weights and indices sit in the second lane, not the first: a
            // rigid vertex of the shipped models reads 00 FF 00 00 / 00 00 00 00
            // (these are D3DCOLOR-order fields, so the component the shader treats
            // as the first influence is byte 1). Writing the weight into byte 0
            // gives the bone a weight of zero and the whole mesh collapses.
            if (layout.has(kSemBlendWeights) && layout.has(kSemBlendIndices)) {
                uint8_t* weights = out + layout.offset[kSemBlendWeights];
                uint8_t* indices = out + layout.offset[kSemBlendIndices];
                weights[0] = 0x00;
                indices[0] = 0x00;
                for (int c = 0; c < 3; ++c) {
                    weights[1 + c] = vertex_weights[source][c];
                    uint8_t slot = 0;
                    if (vertex_weights[source][c] != 0 || c == 0) {
                        const auto entry = std::find(chunk.palette.begin(), chunk.palette.end(),
                                                     vertex_bone[source][c]);
                        if (entry != chunk.palette.end())
                            slot = static_cast<uint8_t>(entry - chunk.palette.begin());
                    }
                    indices[1 + c] = slot;
                }
            }

            if (layout.has(kSemNormal)) {
                StoreBE32(out + layout.offset[kSemNormal],
                          PackDec3N(vertex.nx, vertex.ny, vertex.nz));
            }
            if (layout.has(kSemColour))
                StoreBE32(out + layout.offset[kSemColour], shade[source]);

            if (layout.has(kSemTexcoord0)) {
                uint8_t* uv = out + layout.offset[kSemTexcoord0];
                if (offset.uniform_uv) {
                    // One texel for the whole submesh, taken from the template.
                    //
                    // For a shader whose picture the mod does not have. Audited
                    // against the model's own .mtl: gta_vehicle_vehglass carries
                    // no diffuse at all, so a window's UVs describe nothing, and
                    // writing them makes the glass sample wherever they happen
                    // to fall in the donor's pack -- which is why it came out
                    // red. The donor's own glass samples one place and looks
                    // like glass; pointing at the same place costs nothing,
                    // because a tint has no detail to lose.
                    StoreBE32(uv, uniform_texcoord0);
                } else {
                    StoreBE16(uv + 0, FloatToHalf(vertex.u));
                    StoreBE16(uv + 2, FloatToHalf(vertex.v));
                }
            }

            if (layout.has(kSemTexcoord1))
                StoreBE32(out + layout.offset[kSemTexcoord1], shade_texcoord1[source]);

            if (layout.has(kSemTangent)) {
                const Vec3& tangent = tangents[source];
                StoreBE32(out + layout.offset[kSemTangent],
                          PackDec3N(tangent.x, tangent.y, tangent.z));
            }
        }

        std::vector<uint8_t> index_bytes(chunk.indices.size() * 2, 0);
        for (size_t i = 0; i < chunk.indices.size(); ++i)
            StoreBE16(index_bytes.data() + i * 2, static_cast<uint16_t>(chunk.indices[i]));

        // Into the buffers the template already owns: same size, same addresses,
        // same flag. Only the bytes inside and the counts that describe them.
        uint32_t vertex_address = 0, index_address = 0;
        size_t vertex_offset = 0, index_offset = 0;
        const size_t vertex_capacity = static_cast<size_t>(target.vertex_count) * layout.stride;
        const size_t index_capacity = static_cast<size_t>(target.index_count) * 2;
        if (vertex_bytes.size() > vertex_capacity || index_bytes.size() > index_capacity ||
            !view.U32(target.vertex_buffer + 24, vertex_address) || vertex_address == 0 ||
            !view.U32(target.index_buffer + 8, index_address) || index_address == 0 ||
            !view.Offset(vertex_address, vertex_capacity, vertex_offset) ||
            !view.Offset(index_address, index_capacity, index_offset)) {
            error = "cannot write the chunk into submesh " + std::to_string(chunk.slot);
            return false;
        }
        std::memcpy(resource.data.data() + vertex_offset, vertex_bytes.data(), vertex_bytes.size());
        std::memcpy(resource.data.data() + index_offset, index_bytes.data(), index_bytes.size());

        const uint32_t vertex_count = static_cast<uint32_t>(chunk.vertices.size());
        const uint32_t index_count = static_cast<uint32_t>(chunk.indices.size());

        bool ok = true;
        ok &= patched.SetU16(target.vertex_buffer + 4, static_cast<uint16_t>(vertex_count));
        ok &= patched.SetU32(target.index_buffer + 4, index_count);
        ok &= patched.SetU32(target.address + 44, index_count);
        ok &= patched.SetU32(target.address + 48, index_count / 3);
        ok &= patched.SetU16(target.address + 52, static_cast<uint16_t>(vertex_count));
        ok &= patched.SetU16(target.address + 54, kPrimTriangleList);

        // Publish the palette the vertices index into. Its length is left alone,
        // because the renderer may size its matrix upload from that count.
        if (target.bone_palette != 0 && target.bone_count != 0) {
            for (size_t i = 0; i < chunk.palette.size() && i < target.bone_count; ++i) {
                ok &= patched.SetU16(target.bone_palette + static_cast<uint32_t>(i * 2),
                                     chunk.palette[i]);
            }
        }

        // Every chunk has to draw with the shader whose textures were replaced,
        // so the submeshes borrowed from elsewhere are pointed at it too. Without
        // this each would keep sampling whatever its own body part used to.
        if (target.shader_map != 0) {
            ok &= patched.SetU16(target.shader_map + target.slot_in_model * 2u,
                                 static_cast<uint16_t>(render_shader));
        }
        if (!ok) {
            error = "failed to patch submesh " + std::to_string(chunk.slot);
            return false;
        }

        // Now the GPU-side headers, which is where the draw really reads from.
        uint32_t vertex_gpu = 0, index_gpu = 0;
        uint32_t vertex_size_word = 0, index_size_word = 0;
        if (!patched.U32(target.vertex_buffer + 28, vertex_gpu) || vertex_gpu == 0 ||
            !patched.U32(target.index_buffer + 12, index_gpu) || index_gpu == 0 ||
            !patched.U32(vertex_gpu + kGpuHeaderSizeWord, vertex_size_word) ||
            !patched.U32(index_gpu + kGpuHeaderSizeWord, index_size_word)) {
            error = "cannot read the GPU buffer headers of submesh " + std::to_string(chunk.slot);
            return false;
        }

        // Addresses stay exactly as they were; only the fetch sizes shrink to the
        // part of each buffer the new mesh actually occupies.
        const uint32_t vertex_dwords = vertex_count * layout.stride / 4;
        ok &= patched.SetU32(vertex_gpu + kGpuHeaderSizeWord,
                             (vertex_size_word & 0xFC000003u) | ((vertex_dwords << 2) & 0x03FFFFFCu));
        ok &= patched.SetU32(index_gpu + kGpuHeaderSizeWord,
                             (index_size_word & 0xFF000000u) | ((index_count * 2) & 0x00FFFFFFu));
        if (!ok) {
            error = "failed to patch the GPU buffer headers of submesh " +
                    std::to_string(chunk.slot);
            return false;
        }
    }

    // Whatever was not needed draws nothing.
    //
    // Only the software counts are cleared. Zeroing the GPU-side fetch sizes to
    // match was tried and taken back out: the draw is skipped on an index count
    // of zero either way, so it buys nothing, and it is a change to every
    // character and wheel that already worked. A submesh left describing a
    // buffer it no longer draws from is how the shipped path has always run.
    for (size_t i = 0; i < geometries.size() && !offset.keep_unused; ++i) {
        if (used[i]) continue;
        const GeometryRef& other = geometries[i];
        patched.SetU32(other.address + 44, 0);
        patched.SetU32(other.address + 48, 0);
        patched.SetU16(other.address + 52, 0);
        if (other.index_buffer) patched.SetU32(other.index_buffer + 4, 0);
        if (other.vertex_buffer) patched.SetU16(other.vertex_buffer + 4, 0);
        say("  silenced slot " + std::to_string(i) + " (" + Hex(other.address) + ")");
    }

    if (diagnose) stats->report = std::move(report);

    // The flag and both segment sizes are deliberately untouched: the resource
    // that comes out is the same shape as the one that went in.
    return true;
}

// The coarsest page a grown segment may use.
//
// Nothing about the page COUNT needs guarding: 682 of the 12,987 resources the
// game ships declare more than 127 pages and the largest declares 1454, so the
// streamer's chunk map is not one entry per page and a grown segment is free to
// ask for as many as it needs.
//
// The page SIZE needed guarding while every resource went down the compressed
// road: a page class of 32768 or coarser can only express multiples of the
// streamer's block, so padding a segment away from an exact multiple never
// closed the deficit -- a four-megabyte vehicle body 2376 bytes over its
// budget, and a purchase that loads forever. Half a block is the coarsest page
// that still lets one padding step change the answer. Only payloads of a
// single frame take that road now; see EncodeGrownSize.
constexpr uint32_t kCoarsestGrownShift = 6;  // 256 << 6 = 16384

// Re-encodes `size`, keeping the page class the resource already uses unless it
// is one padding cannot work with.
bool EncodeGrownSize(uint32_t size, uint32_t preferred, uint32_t& mantissa, uint32_t& shift) {
    // The cap above exists to keep a padding step able to move the answer, and
    // padding only happens on the compressed road, which is the one a payload of
    // a single LZX frame or less takes. Past that the resource ships
    // uncompressed, nothing pads it, and a coarser page is not only allowed but
    // wanted: the page class sets the size of the blocks the resource is split
    // into, and a buffer has to fit inside one. See PadResourceTail and
    // MeshOffset::block_align.
    const uint32_t coarsest = size > 32768 ? 15u : kCoarsestGrownShift;
    const uint32_t wanted = (preferred == 0 || preferred > coarsest) ? coarsest : preferred;
    if (EncodeSegmentSize(size, mantissa, shift, wanted) && shift == wanted) return true;
    for (uint32_t candidate = 3; candidate <= coarsest; ++candidate) {
        if (EncodeSegmentSize(size, mantissa, shift, candidate) && shift == candidate) return true;
    }
    return EncodeSegmentSize(size, mantissa, shift, preferred);
}

bool GrowPhysicalSegment(Rsc5Resource& resource, uint32_t bytes, std::string& error) {
    if (bytes == 0) return true;
    if (resource.physical_size == 0) {
        error = "resource has no physical segment to grow";
        return false;
    }

    uint32_t mantissa = 0, shift = 0;
    const uint32_t preferred = (resource.flag >> 26) & 0xF;
    if (!EncodeGrownSize(resource.physical_size + bytes, preferred, mantissa, shift)) {
        error = "physical segment cannot be encoded at " +
                std::to_string(resource.physical_size + bytes) + " bytes";
        return false;
    }

    // The encoded size is what the game believes, and a segment size can only be
    // a mantissa shifted -- so the request is rounded up to one and the data
    // grows to match. Sizing the buffer by the request instead leaves the flag
    // promising bytes that are not in the file, and the stream ends before the
    // loader has filled the segment: the truncated tail all over again.
    const uint32_t encoded = mantissa << (shift + 8);
    resource.data.resize(static_cast<size_t>(resource.virtual_size) + encoded, 0);
    resource.physical_size = encoded;
    resource.flag = (resource.flag & ~0x3FFF8000u) | (shift << 26) | (mantissa << 15);
    return true;
}

bool GrowVirtualSegment(Rsc5Resource& resource, uint32_t bytes, std::string& error) {
    if (bytes == 0) return true;

    uint32_t mantissa = 0, shift = 0;
    const uint32_t preferred = (resource.flag >> 11) & 0xF;
    if (!EncodeGrownSize(resource.virtual_size + bytes, preferred, mantissa, shift)) {
        error = "virtual segment cannot be encoded at " +
                std::to_string(resource.virtual_size + bytes) + " bytes";
        return false;
    }

    const uint32_t encoded = mantissa << (shift + 8);
    if (encoded <= resource.virtual_size) {
        error = "virtual segment cannot be grown any further";
        return false;
    }
    resource.data.insert(resource.data.begin() + resource.virtual_size,
                         encoded - resource.virtual_size, 0);
    resource.virtual_size = encoded;
    resource.flag = (resource.flag & ~0x7FFFu) | (shift << 11) | mantissa;
    return true;
}

uint32_t VirtualBlockSize(const Rsc5Resource& resource) {
    return 4096u << ((resource.flag >> 11) & 0xFu);
}

bool RoundVirtualToBlock(Rsc5Resource& resource, std::string& error) {
    // sub_821E57B8 emits blocks of 4096 << shift for as long as they fit and
    // then HALVES, so a segment whose size is not a whole number of those ends
    // in a tail of smaller blocks -- and a buffer placed on a multiple of the
    // big block can still straddle two of the small ones. Rounding the segment
    // up to a whole number of big blocks makes every block the same size, and
    // then "aligned to the block" means what it says. Measured: without this the
    // BMW's paint buffer sat at 1048576, inside a 524288 block, and crossed.
    const uint32_t block = VirtualBlockSize(resource);
    if (block == 0 || resource.virtual_size % block == 0) return true;
    const uint64_t rounded = (uint64_t(resource.virtual_size) / block + 1) * block;
    if (rounded > 0xFFFFFFFFull) {
        error = "rounding the virtual segment to a whole block overflows";
        return false;
    }
    return GrowVirtualSegment(resource, static_cast<uint32_t>(rounded) - resource.virtual_size,
                              error);
}

bool SetVirtualPageShift(Rsc5Resource& resource, uint32_t shift, std::string& error) {
    if (shift > 15) {
        error = "page shift " + std::to_string(shift) + " is past the four bits the flag has";
        return false;
    }
    const uint32_t unit = 256u << shift;
    const uint64_t pages = (uint64_t(resource.virtual_size) + unit - 1) / unit;
    if (pages == 0 || pages > 0x7FF) {
        error = "a page of " + std::to_string(unit) + " cannot express " +
                std::to_string(resource.virtual_size) + " bytes";
        return false;
    }
    const uint32_t encoded = static_cast<uint32_t>(pages) * unit;
    if (encoded > resource.virtual_size) {
        resource.data.insert(resource.data.begin() + resource.virtual_size,
                             encoded - resource.virtual_size, 0);
    }
    resource.virtual_size = encoded;
    resource.flag = (resource.flag & ~0x7FFFu) | (shift << 11) | static_cast<uint32_t>(pages);
    return true;
}

bool PadResourceTail(Rsc5Resource& resource, uint32_t want, std::string& error,
                     uint32_t* out_room) {
    Rsc5View view(resource.data, resource.virtual_size);

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) {
        error = "resource is not a drawable";
        return false;
    }

    // The furthest byte any LOD still draws from, in each segment separately: a
    // vehicle keeps its buffers in the virtual segment, but a resource that
    // keeps them in the physical one has to be padded there instead, since
    // growing the virtual segment only pushes the physical one further along
    // and leaves the same bytes at the end of the file.
    size_t virtual_end = 0, physical_end = 0;
    for (uint32_t slot = 0; slot < drawable.lod_slots; ++slot) {
        uint32_t lod = 0;
        if (!view.U32(drawable.drawable + drawable.lod_field + slot * 4, lod) || lod == 0) continue;

        uint32_t model_array = 0;
        uint16_t model_count = 0;
        if (!view.U32(lod, model_array) || !view.U16(lod + 4, model_count)) continue;

        for (uint16_t m = 0; m < model_count; ++m) {
            uint32_t model = 0, geometry_array = 0;
            uint16_t geometry_count = 0;
            if (!view.U32(model_array + m * 4, model) || model == 0) continue;
            if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count))
                continue;

            for (uint16_t g = 0; g < geometry_count; ++g) {
                uint32_t geometry = 0, buffer = 0, stride = 0, index_count = 0;
                uint16_t vertex_count = 0;
                if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;
                view.U32(geometry + 44, index_count);
                view.U16(geometry + 52, vertex_count);

                struct { uint32_t pointer_at; size_t bytes; } spans[2] = {{0, 0}, {0, 0}};
                if (view.U32(geometry + 12, buffer) && buffer &&
                    view.U32(buffer + 12, stride) && stride) {
                    view.U32(buffer + 24, spans[0].pointer_at);
                    spans[0].bytes = static_cast<size_t>(vertex_count) * stride;
                }
                if (view.U32(geometry + 28, buffer) && buffer) {
                    view.U32(buffer + 8, spans[1].pointer_at);
                    spans[1].bytes = static_cast<size_t>(index_count) * 2;
                }

                for (const auto& span : spans) {
                    size_t at = 0;
                    if (span.pointer_at == 0 || span.bytes == 0) continue;
                    if (!view.Offset(span.pointer_at, span.bytes, at)) continue;
                    const size_t end = at + span.bytes;
                    if (end <= resource.virtual_size) {
                        virtual_end = std::max(virtual_end, end);
                    } else {
                        physical_end = std::max(physical_end, end - resource.virtual_size);
                    }
                }
            }
        }
    }

    if (physical_end != 0 && resource.physical_size != 0) {
        const size_t room = resource.physical_size - std::min<size_t>(physical_end,
                                                                     resource.physical_size);
        if (room < want &&
            !GrowPhysicalSegment(resource, want - static_cast<uint32_t>(room), error)) {
            return false;
        }
        if (out_room) {
            *out_room = static_cast<uint32_t>(resource.physical_size - physical_end);
        }
        return true;
    }
    if (virtual_end == 0) return true;
    const size_t room = resource.virtual_size - std::min<size_t>(virtual_end,
                                                                 resource.virtual_size);
    if (room < want && !GrowVirtualSegment(resource, want - static_cast<uint32_t>(room), error)) {
        return false;
    }
    if (out_room) *out_room = static_cast<uint32_t>(resource.virtual_size - virtual_end);
    return true;
}

bool ReadDrawableBounds(const Rsc5Resource& resource, float min_out[3], float max_out[3]) {
    // Rsc5View patches as well as reads, so it holds a mutable reference to the
    // buffer; nothing below writes to it.
    auto& data = const_cast<std::vector<uint8_t>&>(resource.data);
    Rsc5View view(data, resource.virtual_size);

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) return false;

    if (drawable.inline_bounds) {
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            ok &= view.Float(drawable.drawable + 32 + i * 4, min_out[i]);
            ok &= view.Float(drawable.drawable + 48 + i * 4, max_out[i]);
        }
        return ok;
    }
    return MeasureTemplateBounds(view, resource.data, drawable, min_out, max_out);
}

bool ReplaceShaderDiffuse(Rsc5Resource& resource, uint32_t shader_index, const Image& atlas,
                          std::string& error, TextureStats* stats) {
    if (atlas.empty()) {
        error = "no atlas to write";
        return false;
    }

    Rsc5View view(resource.data, resource.virtual_size);

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) {
        error = "no drawable in resource";
        return false;
    }

    uint32_t group = 0, array = 0;
    uint16_t count = 0;
    if (!ShaderGroupOf(view, drawable, group, array, count)) {
        error = "resource has no shader group this can read";
        return false;
    }
    if (shader_index >= count) {
        error = "shader " + std::to_string(shader_index) + " is outside the group of " +
                std::to_string(count);
        return false;
    }

    // Everything the drawable has allocated, so the space each texture is
    // allowed to use can be bounded by whatever comes next. Nothing here grows.
    std::vector<uint32_t> allocations;
    CollectBufferAddresses(view, drawable, allocations);
    for (uint16_t s = 0; s < count; ++s) {
        uint32_t shader = 0;
        if (!view.U32(array + s * 4u, shader) || shader == 0) continue;
        for (uint32_t parameter : ShaderTextureParams(view, shader)) {
            TextureRef texture;
            if (!ReadTexture(view, parameter, texture)) continue;
            if (texture.base) allocations.push_back(texture.base);
            if (texture.mip) allocations.push_back(texture.mip);
        }
    }
    std::sort(allocations.begin(), allocations.end());
    allocations.erase(std::unique(allocations.begin(), allocations.end()), allocations.end());

    uint32_t shader = 0;
    if (!view.U32(array + shader_index * 4u, shader) || shader == 0) {
        error = "shader " + std::to_string(shader_index) + " is null";
        return false;
    }

    // The diffuse map is the first texture parameter that is neither the normal
    // map (its name ends in _n) nor one of the tiling detail sheets. Falling
    // back to the first parameter matches every shipped character shader, where
    // the diffuse is declared first anyway.
    TextureRef target;
    bool found = false;
    const std::vector<uint32_t> parameters = ShaderTextureParams(view, shader);
    for (uint32_t parameter : parameters) {
        TextureRef texture;
        if (!ReadTexture(view, parameter, texture)) continue;
        const bool normal_map = texture.name.size() > 2 &&
                                texture.name.compare(texture.name.size() - 2, 2, "_n") == 0;
        const bool detail = texture.name.rfind("detail", 0) == 0;
        if (!found) {
            target = texture;
            found = true;
        }
        if (!normal_map && !detail) {
            target = texture;
            break;
        }
    }
    if (!found) {
        error = "shader " + std::to_string(shader_index) + " has no texture parameters";
        return false;
    }
    if (!target.supported) {
        error = "texture " + target.name + " is in a format this cannot write";
        return false;
    }

    uint32_t levels = 0;
    if (!WriteTextureInPlace(view, resource, allocations, target, atlas, levels)) {
        error = "no room to write " + target.name + " in place";
        return false;
    }

    // Flatten the normal map that goes with it.
    //
    // It is still the driver's, and the mesh no longer has the driver's UVs, so
    // every texel of it lands somewhere unrelated to the surface it is meant to
    // shape. The lighting reads that as detail and picks it out: the model comes
    // out looking bleached and polished, worst on whichever side the sun is on.
    // Replacing it with a flat one costs the mod nothing it had -- the detail
    // belonged to a different face -- and gives the shader the unperturbed
    // normal the geometry already carries.
    for (uint32_t parameter : parameters) {
        TextureRef texture;
        if (!ReadTexture(view, parameter, texture)) continue;
        if (texture.name.size() <= 2 ||
            texture.name.compare(texture.name.size() - 2, 2, "_n") != 0) {
            continue;
        }
        if (!texture.supported || texture.base == 0) continue;

        uint32_t normal_levels = 0;
        WriteTextureInPlace(view, resource, allocations, texture,
                            FlatNormalMap(texture.width, texture.height), normal_levels);
        break;
    }

    if (stats) {
        stats->name = target.name;
        stats->width = target.width;
        stats->height = target.height;
        stats->levels = levels;
    }
    return true;
}

bool TexturePackShader(const Rsc5Resource& resource, uint32_t& shader_index) {
    if (resource.type != kTypeTexturePack) return false;

    auto& data = const_cast<std::vector<uint8_t>&>(resource.data);
    Rsc5View view(data, resource.virtual_size);

    // The same texture ReplaceDictionaryTexture picks: the biggest writable one.
    TextureRef target;
    bool found = false;
    for (uint32_t address : TexturePackTextures(view)) {
        TextureRef texture;
        if (!ReadTexture(view, address, texture)) continue;
        if (!texture.supported || texture.base == 0) continue;
        if (found && static_cast<size_t>(texture.width) * texture.height <=
                         static_cast<size_t>(target.width) * target.height) {
            continue;
        }
        target = texture;
        found = true;
    }
    if (!found) return false;

    return TexturePackMaterialOf(view, target.address, shader_index);
}

bool ReplaceDictionaryTexture(Rsc5Resource& resource, const Image& image, std::string& error,
                              TextureStats* stats) {
    if (image.empty()) {
        error = "no image to write";
        return false;
    }
    if (resource.type != kTypeTexturePack) {
        error = "resource is type " + std::to_string(resource.type) + ", not a texture pack";
        return false;
    }

    Rsc5View view(resource.data, resource.virtual_size);

    const std::vector<uint32_t> entries = TexturePackTextures(view);
    if (entries.empty()) {
        error = "this wheel ships no texture of its own";
        return false;
    }

    // Only the dictionary's own textures are allocated here -- a texture pack
    // holds no geometry -- so they alone bound how far each write may run.
    std::vector<uint32_t> allocations;
    std::vector<TextureRef> textures;
    for (uint32_t address : entries) {
        TextureRef texture;
        if (!ReadTexture(view, address, texture)) continue;
        if (texture.base) allocations.push_back(texture.base);
        if (texture.mip) allocations.push_back(texture.mip);
        textures.push_back(std::move(texture));
    }
    std::sort(allocations.begin(), allocations.end());
    allocations.erase(std::unique(allocations.begin(), allocations.end()), allocations.end());

    // The biggest one the format is writable for. Every shipped wheel keeps a
    // single texture, so this only decides anything if a later one does not.
    TextureRef target;
    bool found = false;
    for (const TextureRef& texture : textures) {
        if (!texture.supported || texture.base == 0) continue;
        if (found && static_cast<size_t>(texture.width) * texture.height <=
                         static_cast<size_t>(target.width) * target.height) {
            continue;
        }
        target = texture;
        found = true;
    }
    if (!found) {
        error = "the wheel's textures are all in formats this cannot write";
        return false;
    }

    uint32_t levels = 0;
    if (!WriteTextureInPlace(view, resource, allocations, target, image, levels)) {
        error = "no room to write " + target.name + " in place";
        return false;
    }

    if (stats) {
        stats->name = target.name;
        stats->width = target.width;
        stats->height = target.height;
        stats->levels = levels;
    }
    return true;
}

bool ShaderVertexStrides(const Rsc5Resource& resource, std::vector<uint32_t>& stride_of) {
    stride_of.clear();

    auto& data = const_cast<std::vector<uint8_t>&>(resource.data);
    Rsc5View view(data, resource.virtual_size);

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) return false;

    // Sized from the shader indices the models actually name, not from the
    // shader group: a vehicle body is its own root and keeps no readable group
    // where a character's sits (ShaderGroupOf reads drawable+8, which on a type
    // 63 is the first LOD pointer), so asking for the group reports nothing and
    // every stride comes back zero.
    uint32_t lod = 0;
    if (!view.U32(drawable.drawable + drawable.lod_field, lod) || lod == 0) return false;

    uint32_t model_array = 0;
    uint16_t model_count = 0;
    if (!view.U32(lod, model_array) || !view.U16(lod + 4, model_count)) return false;

    for (uint16_t m = 0; m < model_count; ++m) {
        uint32_t model = 0;
        if (!view.U32(model_array + m * 4, model) || model == 0) continue;

        uint32_t geometry_array = 0, shader_map = 0;
        uint16_t geometry_count = 0;
        if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count)) continue;
        view.U32(model + 16, shader_map);

        for (uint16_t g = 0; g < geometry_count; ++g) {
            uint32_t geometry = 0, vertex_buffer = 0, stride = 0;
            uint16_t shader = 0;
            if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;
            if (!view.U32(geometry + 12, vertex_buffer) || vertex_buffer == 0) continue;
            if (!view.U32(vertex_buffer + 12, stride) || stride == 0) continue;
            if (!shader_map || !view.U16(shader_map + g * 2u, shader)) continue;
            if (shader >= stride_of.size()) stride_of.resize(shader + 1u, 0);
            // The widest one a shader owns: a pass is dealt into every slot of
            // its shader, and the cost of the mesh is set by the ones it fills.
            stride_of[shader] = std::max(stride_of[shader], stride);
        }
    }
    return true;
}

namespace {

// The textures one material of a pack reads, in the order its parameter block
// lists them.
//
// A material says where its parameters live (+0x10) and how big that block is
// (+0x18, the count in the high half and the byte size in the low), and the
// block mixes texture pointers with float data. Rather than guess the layout per
// effect -- there are fifty-eight of them and they disagree, from three
// parameters on BlackMatte to twenty on CarPaintCustomizable -- every word of
// the block is tested against the dictionary's own texture addresses. That is
// the same bounded scan TexturePackMaterialOf already runs from the other end,
// and it cannot mistake a float for a pointer because the only addresses it
// accepts are ones the dictionary handed out.
std::vector<uint32_t> PackMaterialTextures(const Rsc5View& view, uint32_t material,
                                           const std::vector<uint32_t>& dictionary) {
    std::vector<uint32_t> out;
    uint32_t parameters = 0, packed = 0;
    if (!view.U32(material + kMaterialParameters, parameters) || parameters == 0) return out;
    if (!view.U32(material + kMaterialParameterSize, packed)) return out;

    const uint32_t bytes = packed & 0xFFFFu;
    for (uint32_t at = 0; at + 4 <= bytes; at += 4) {
        uint32_t word = 0;
        if (!view.U32(parameters + at, word) || word == 0) continue;
        if (std::find(dictionary.begin(), dictionary.end(), word) == dictionary.end()) continue;
        if (std::find(out.begin(), out.end(), word) != out.end()) continue;
        out.push_back(word);
    }
    return out;
}

bool NameEndsWith(const std::string& name, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return name.size() > length && name.compare(name.size() - length, length, suffix) == 0;
}

// Which of a material's textures is its colour map: the first that is neither a
// normal map nor a tiling detail sheet, falling back to the first of any kind.
// The same rule ReplaceShaderDiffuse applies to a character's shader, and it
// holds here for the same reason -- the colour map is declared first on every
// shipped car material that has one.
bool PackMaterialDiffuse(const Rsc5View& view, uint32_t material,
                         const std::vector<uint32_t>& dictionary, TextureRef& out) {
    bool found = false;
    for (uint32_t address : PackMaterialTextures(view, material, dictionary)) {
        TextureRef texture;
        if (!ReadTexture(view, address, texture)) continue;
        if (!found) {
            out = texture;
            found = true;
        }
        if (!NameEndsWith(texture.name, "_n") && texture.name.rfind("detail", 0) != 0) {
            out = texture;
            return true;
        }
    }
    return found;
}

// The pack's material array, or false when the resource is not one.
bool PackMaterialArray(const Rsc5View& view, uint32_t& materials, uint16_t& count) {
    uint32_t pack = 0;
    if (!view.U32(kVirtualBase + kPackField, pack) || pack == 0) return false;
    if (!view.U32(pack + kPackMaterials, materials) || materials == 0) return false;
    return view.U16(pack + kPackMaterialCount, count) && count != 0;
}

}  // namespace

bool ReadPackMaterials(const Rsc5Resource& pack, std::vector<PackMaterial>& out) {
    out.clear();
    if (pack.type != kTypeTexturePack) return false;

    auto& data = const_cast<std::vector<uint8_t>&>(pack.data);
    Rsc5View view(data, pack.virtual_size);

    uint32_t materials = 0;
    uint16_t count = 0;
    if (!PackMaterialArray(view, materials, count)) return false;

    const std::vector<uint32_t> dictionary = TexturePackTextures(view);
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t material = 0, effect = 0;
        if (!view.U32(materials + i * 4u, material) || material == 0) return false;
        if (!view.U32(material + kMaterialEffect, effect)) return false;

        PackMaterial entry;
        entry.effect = effect;
        entry.address = material;

        TextureRef diffuse;
        if (PackMaterialDiffuse(view, material, dictionary, diffuse)) {
            entry.diffuse = diffuse.address;
            entry.diffuse_name = diffuse.name;
            entry.width = diffuse.width;
            entry.height = diffuse.height;
            entry.writable = diffuse.supported && diffuse.base != 0;
        }
        out.push_back(std::move(entry));
    }
    return true;
}

bool ReplacePackMaterialDiffuse(Rsc5Resource& pack, uint32_t material_index, const Image& image,
                                std::string& error, TextureStats* stats) {
    if (image.empty()) {
        error = "no image to write";
        return false;
    }
    if (pack.type != kTypeTexturePack) {
        error = "resource is type " + std::to_string(pack.type) + ", not a material pack";
        return false;
    }

    Rsc5View view(pack.data, pack.virtual_size);

    uint32_t materials = 0;
    uint16_t count = 0;
    if (!PackMaterialArray(view, materials, count)) {
        error = "the pack has no material array";
        return false;
    }
    if (material_index >= count) {
        error = "material " + std::to_string(material_index) + " is outside the pack's " +
                std::to_string(count);
        return false;
    }

    // A pack holds no geometry, so its dictionary's textures are the only things
    // allocated in it and they alone bound how far a write may run.
    const std::vector<uint32_t> dictionary = TexturePackTextures(view);
    std::vector<uint32_t> allocations;
    for (uint32_t address : dictionary) {
        TextureRef texture;
        if (!ReadTexture(view, address, texture)) continue;
        if (texture.base) allocations.push_back(texture.base);
        if (texture.mip) allocations.push_back(texture.mip);
    }
    std::sort(allocations.begin(), allocations.end());
    allocations.erase(std::unique(allocations.begin(), allocations.end()), allocations.end());

    uint32_t material = 0;
    if (!view.U32(materials + material_index * 4u, material) || material == 0) {
        error = "material " + std::to_string(material_index) + " is null";
        return false;
    }

    TextureRef target;
    if (!PackMaterialDiffuse(view, material, dictionary, target)) {
        uint32_t effect = 0;
        view.U32(material + kMaterialEffect, effect);
        error = std::string(CarEffectName(effect)) + " (material " +
                std::to_string(material_index) + ") reads no texture at all";
        return false;
    }
    if (!target.supported) {
        error = "texture " + target.name + " is in a format this cannot write";
        return false;
    }

    // A lamp has no colour map: what PackMaterialDiffuse found is the lens
    // normal map, and its companion _i is the glow mask. Both are built from
    // the photograph instead of receiving it. See LampNormalMap.
    uint32_t effect = 0;
    view.U32(material + kMaterialEffect, effect);
    if (std::string_view(CarEffectName(effect)) == "CarLight") {
        uint32_t levels = 0;
        if (!WriteTextureInPlace(view, pack, allocations, target, LampNormalMap(image), levels)) {
            error = "no room to write " + target.name + " in place";
            return false;
        }
        const Image glow = LampGlowMap(image);
        for (uint32_t address : PackMaterialTextures(view, material, dictionary)) {
            TextureRef texture;
            if (!ReadTexture(view, address, texture)) continue;
            if (!texture.supported || texture.base == 0 || texture.address == target.address)
                continue;
            uint32_t glow_levels = 0;
            WriteTextureInPlace(view, pack, allocations, texture, glow, glow_levels);
        }
        if (stats) {
            stats->name = target.name;
            stats->width = target.width;
            stats->height = target.height;
            stats->levels = levels;
        }
        return true;
    }

    uint32_t levels = 0;
    if (!WriteTextureInPlace(view, pack, allocations, target, image, levels)) {
        error = "no room to write " + target.name + " in place";
        return false;
    }

    // The two companions of a colour map. The illuminated variant carries the
    // same picture, so a lamp keeps its own artwork when it lights; the normal
    // map is flattened, because it is still the donor's and the mesh no longer
    // has the donor's UVs -- left as it is, the lighting reads it as detail and
    // picks out a shape that belongs to another car.
    const std::string illuminated = target.name + "_i";
    for (uint32_t address : PackMaterialTextures(view, material, dictionary)) {
        TextureRef texture;
        if (!ReadTexture(view, address, texture)) continue;
        if (!texture.supported || texture.base == 0 || texture.address == target.address) continue;

        uint32_t companion_levels = 0;
        if (texture.name == illuminated) {
            WriteTextureInPlace(view, pack, allocations, texture, image, companion_levels);
        } else if (NameEndsWith(texture.name, "_n")) {
            WriteTextureInPlace(view, pack, allocations, texture,
                                FlatNormalMap(texture.width, texture.height), companion_levels);
        }
    }

    if (stats) {
        stats->name = target.name;
        stats->width = target.width;
        stats->height = target.height;
        stats->levels = levels;
    }
    return true;
}

// The game's own car shader list, in the order shaders/cars/preload.list gives
// it -- which is the order the effect number at a material's +12 indexes.
//
// The link was proved on the police Caprice, whose seven materials name effects
// 53, 25, 9, 7, 40, 3 and 42: BumpSpecAlpha on the submesh that samples the
// car's trim texture, CarPaintCustomizable on the two largest body submeshes,
// BlackMatte on the position-only ones, CarLight on the submesh holding both
// light textures, Chrome, CarGlass, and LicensePlate on a four-vertex quad.
// Seven out of seven agree with what those submeshes are, which is what says
// the number is an index into this list and not something else.
const char* const kCarEffects[] = {
    "WheelRubber",       "WheelBrakeRotor",   "CarbonFiber",
    "CarGlass",          "BrushedMetal",      "TexturedPlastic",
    "BumpSpec",          "CarLight",          "AmbientOcclusionGenerator",
    "BlackMatte",        "BumpedGlass",       "InteriorTrim",
    "TestRimBlur",       "ShaderBlits",       "VehBlurImposter",
    "BrakeCaliper",      "SkidMark",          "BumpSpecSkinned",
    "AnisotropicBilletMetal", "Seam",         "AmbientOcclusionShadows",
    "InteriorTrimAlpha", "SpecMapGen",        "InteriorTexturedEmissive",
    "InteriorTexturedEmissiveAlpha",          "CarPaintCustomizable",
    "VinylGenerator",    "VertDamageLayoutGenerator",
    "VertDamageBlit",    "FontDraw",          "RimMain",
    "RimBadge",          "ShadowImposterCollector", "SSTTrail",
    "PowerupShootEffect", "PowerupTargetEffect",    "Grill",
    "RoarShockwave",     "Underbody",         "RimNormalMapped",
    "Chrome",            "BumpedAlphaChrome", "LicensePlate",
    "CarPaintNormalMapped", "EngineBay",      "BumperSubframe",
    "AmbientCarPaint",   "AmbientBumpSpec",   "AmbientCarGlass",
    "AmbientBlackMatte", "AmbientTire",       "AmbientCarLight",
    "PowerupIceCube",    "BumpSpecAlpha",     "GrillOpaque",
    "BlackOpaqueGlass",  "BumpedBrushedMetal", "PowerupStealthSkinned",
};

const char* CarEffectName(uint32_t index) {
    return index < std::size(kCarEffects) ? kCarEffects[index] : "";
}

int32_t CarEffectIndex(const std::string& name) {
    auto same = [](const char* a, const std::string& b) {
        if (std::strlen(a) != b.size()) return false;
        for (size_t i = 0; i < b.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };
    for (size_t i = 0; i < std::size(kCarEffects); ++i) {
        if (same(kCarEffects[i], name)) return static_cast<int32_t>(i);
    }
    return -1;
}

bool ReadPackEffects(const Rsc5Resource& pack, std::vector<uint32_t>& effects) {
    effects.clear();
    if (pack.type != kTypeTexturePack) return false;

    auto& data = const_cast<std::vector<uint8_t>&>(pack.data);
    Rsc5View view(data, pack.virtual_size);

    uint32_t root = 0, materials = 0;
    uint16_t count = 0;
    if (!view.U32(kVirtualBase + kPackField, root) || root == 0) return false;
    if (!view.U32(root + kPackMaterials, materials) || materials == 0) return false;
    if (!view.U16(root + kPackMaterialCount, count) || count == 0) return false;

    for (uint16_t i = 0; i < count; ++i) {
        uint32_t material = 0, effect = 0;
        if (!view.U32(materials + i * 4u, material) || material == 0) return false;
        if (!view.U32(material + kMaterialEffect, effect)) return false;
        effects.push_back(effect);
    }
    return true;
}

size_t SilenceShaderGeometry(Rsc5Resource& resource, int32_t shader) {
    Rsc5View view(resource.data, resource.virtual_size);

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) return 0;

    size_t silenced = 0;
    for (uint32_t slot = 0; slot < drawable.lod_slots; ++slot) {
        uint32_t lod = 0;
        if (!view.U32(drawable.drawable + drawable.lod_field + slot * 4, lod) || lod == 0) continue;

        uint32_t model_array = 0;
        uint16_t model_count = 0;
        if (!view.U32(lod, model_array) || !view.U16(lod + 4, model_count)) continue;

        for (uint16_t m = 0; m < model_count; ++m) {
            uint32_t model = 0;
            if (!view.U32(model_array + m * 4, model) || model == 0) continue;

            uint32_t geometry_array = 0, shader_map = 0;
            uint16_t geometry_count = 0;
            if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count))
                continue;
            view.U32(model + 16, shader_map);

            for (uint16_t g = 0; g < geometry_count; ++g) {
                uint32_t geometry = 0, vertex_buffer = 0, index_buffer = 0;
                if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;
                if (shader >= 0) {
                    uint16_t drawn_by = 0;
                    if (!shader_map || !view.U16(shader_map + g * 2u, drawn_by) ||
                        drawn_by != static_cast<uint16_t>(shader)) {
                        continue;
                    }
                }
                view.U32(geometry + 12, vertex_buffer);
                view.U32(geometry + 28, index_buffer);
                view.SetU32(geometry + 44, 0);
                view.SetU32(geometry + 48, 0);
                view.SetU16(geometry + 52, 0);
                if (index_buffer) view.SetU32(index_buffer + 4, 0);
                if (vertex_buffer) view.SetU16(vertex_buffer + 4, 0);
                ++silenced;
            }
        }
    }
    return silenced;
}

// Moves every submesh a shader draws by a fixed amount, in the car's own space.
//
// For the one piece of the donor worth keeping. Its licence plate is a four
// vertex quad, and in the Caprice it sits dead centre -- x exactly 0.000, 30 by
// 15 centimetres, at y 0.679 and z 2.724 -- so it is not crooked, it is simply
// at the Caprice's rear. A car fitted into the same box does not have its bumper
// face at that depth or the plate recess at that height, and there is no way to
// work the right pair out from the geometry: the recess is a dent in a panel,
// not a feature the mesh names. So it is a number the mod states and an eye
// checks.
size_t ShaderBandSamples(const Rsc5Resource& resource, int32_t shader,
                         std::vector<BandSample>& out) {
    auto& data = const_cast<std::vector<uint8_t>&>(resource.data);
    Rsc5View view(data, resource.virtual_size);

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) return 0;

    uint32_t lod = 0;
    if (!view.U32(drawable.drawable + drawable.lod_field, lod) || lod == 0) return 0;
    uint32_t model_array = 0;
    uint16_t model_count = 0;
    if (!view.U32(lod, model_array) || !view.U16(lod + 4, model_count)) return 0;

    size_t read = 0;
    for (uint16_t m = 0; m < model_count; ++m) {
        uint32_t model = 0;
        if (!view.U32(model_array + m * 4, model) || model == 0) continue;
        uint32_t geometry_array = 0, shader_map = 0;
        uint16_t geometry_count = 0;
        if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count)) continue;
        view.U32(model + 16, shader_map);
        for (uint16_t g = 0; g < geometry_count; ++g) {
            uint32_t geometry = 0;
            if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;
            uint16_t drawn_by = 0;
            if (!shader_map || !view.U16(shader_map + g * 2u, drawn_by) ||
                drawn_by != static_cast<uint16_t>(shader)) {
                continue;
            }
            uint32_t vertex_buffer = 0, address = 0;
            uint16_t vertex_count = 0;
            if (!view.U32(geometry + 12, vertex_buffer) || vertex_buffer == 0) continue;
            if (!view.U16(geometry + 52, vertex_count) || vertex_count == 0) continue;
            VertexLayout layout;
            if (!ReadVertexLayout(view, vertex_buffer, layout, false)) continue;
            if (!layout.has(kSemPosition) || !layout.has(kSemTexcoord1)) continue;
            if (!view.U32(vertex_buffer + 24, address) || address == 0) continue;
            size_t at = 0;
            const size_t bytes = static_cast<size_t>(vertex_count) * layout.stride;
            if (!view.Offset(address, bytes, at)) continue;
            for (uint32_t v = 0; v < vertex_count; ++v) {
                const uint8_t* source = resource.data.data() + at + v * layout.stride;
                BandSample sample;
                sample.x = LoadBEFloat(source + layout.offset[kSemPosition] + 0);
                sample.y = LoadBEFloat(source + layout.offset[kSemPosition] + 4);
                sample.z = LoadBEFloat(source + layout.offset[kSemPosition] + 8);
                sample.texcoord1 = LoadBE32(source + layout.offset[kSemTexcoord1]);
                out.push_back(sample);
                ++read;
            }
        }
    }
    return read;
}

size_t TranslateShaderGeometry(Rsc5Resource& resource, int32_t shader, float dx, float dy,
                               float dz) {
    Rsc5View view(resource.data, resource.virtual_size);

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) return 0;

    size_t moved = 0;
    for (uint32_t slot = 0; slot < drawable.lod_slots; ++slot) {
        uint32_t lod = 0;
        if (!view.U32(drawable.drawable + drawable.lod_field + slot * 4, lod) || lod == 0) continue;

        uint32_t model_array = 0;
        uint16_t model_count = 0;
        if (!view.U32(lod, model_array) || !view.U16(lod + 4, model_count)) continue;

        for (uint16_t m = 0; m < model_count; ++m) {
            uint32_t model = 0;
            if (!view.U32(model_array + m * 4, model) || model == 0) continue;

            uint32_t geometry_array = 0, shader_map = 0;
            uint16_t geometry_count = 0;
            if (!view.U32(model + 4, geometry_array) || !view.U16(model + 8, geometry_count))
                continue;
            view.U32(model + 16, shader_map);

            for (uint16_t g = 0; g < geometry_count; ++g) {
                uint32_t geometry = 0;
                if (!view.U32(geometry_array + g * 4, geometry) || geometry == 0) continue;

                uint16_t drawn_by = 0;
                if (!shader_map || !view.U16(shader_map + g * 2u, drawn_by) ||
                    drawn_by != static_cast<uint16_t>(shader)) {
                    continue;
                }

                uint32_t vertex_buffer = 0, address = 0;
                uint16_t vertex_count = 0;
                if (!view.U32(geometry + 12, vertex_buffer) || vertex_buffer == 0) continue;
                if (!view.U16(geometry + 52, vertex_count) || vertex_count == 0) continue;

                VertexLayout layout;
                if (!ReadVertexLayout(view, vertex_buffer, layout, false)) continue;
                if (!layout.has(kSemPosition)) continue;
                if (!view.U32(vertex_buffer + 24, address) || address == 0) continue;

                size_t at = 0;
                const size_t bytes = static_cast<size_t>(vertex_count) * layout.stride;
                if (!view.Offset(address, bytes, at)) continue;

                for (uint32_t v = 0; v < vertex_count; ++v) {
                    uint8_t* p = resource.data.data() + at + v * layout.stride +
                                 layout.offset[kSemPosition];
                    StoreBEFloat(p + 0, LoadBEFloat(p + 0) + dx);
                    StoreBEFloat(p + 4, LoadBEFloat(p + 4) + dy);
                    StoreBEFloat(p + 8, LoadBEFloat(p + 8) + dz);
                }
                ++moved;
            }
        }
    }
    return moved;
}

size_t SilenceDrawable(Rsc5Resource& resource) {
    // A whole part the mod ships empty, hidden the only way that survives the
    // loader: each LOD keeps its model array pointer, and only the count beside
    // it goes to zero.
    //
    // Emptying the geometries instead -- which is what SilenceShaderGeometry
    // does, correctly, for one shader among several -- is what asserted. A part
    // whose every geometry carries a zero vertex and index count still has its
    // buffer pointers, mcCarDrawableChunk's placer follows one of them, and the
    // fixup is handed a word nobody ever wrote: the reported address differed
    // between two runs of the same build, 0xCDCDCDCD and 0x09240002, which is
    // what reading uninitialised memory looks like. Cutting the count above the
    // models means nothing under the LOD is walked at all, and a LOD that lists
    // nothing is a shape the game already ships -- the donor's own hood has two
    // of its three LOD slots null.
    Rsc5View view(resource.data, resource.virtual_size);

    DrawableLayout drawable;
    if (!ResolveDrawable(view, resource.type, drawable)) return 0;

    size_t hidden = 0;
    for (uint32_t slot = 0; slot < drawable.lod_slots; ++slot) {
        uint32_t lod = 0;
        if (!view.U32(drawable.drawable + drawable.lod_field + slot * 4, lod) || lod == 0) continue;

        uint16_t model_count = 0;
        if (!view.U16(lod + 4, model_count) || model_count == 0) continue;
        view.SetU16(lod + 4, 0);
        hidden += model_count;
    }
    return hidden;
}

}  // namespace mc::modloader
