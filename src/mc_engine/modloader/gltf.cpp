#include "gltf.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace mc::modloader {

namespace {

// ---------------------------------------------------------------------------
// Just enough JSON for glTF: objects, arrays, numbers, strings, literals.
// ---------------------------------------------------------------------------

struct Json {
    enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = Kind::kNull;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json* Find(const std::string& key) const {
        auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    }
    const Json* At(size_t index) const {
        return index < items.size() ? &items[index] : nullptr;
    }
    int Int(int fallback = -1) const {
        return kind == Kind::kNumber ? static_cast<int>(number) : fallback;
    }
    float Real(float fallback = 0.0f) const {
        return kind == Kind::kNumber ? static_cast<float>(number) : fallback;
    }
    int IntField(const std::string& key, int fallback = -1) const {
        const Json* value = Find(key);
        return value ? value->Int(fallback) : fallback;
    }
};

class JsonParser {
 public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool Parse(Json& out) {
        Skip();
        return Value(out) && true;
    }

 private:
    void Skip() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' ||
                text_[position_] == '\r')) {
            ++position_;
        }
    }

    bool Literal(const char* word) {
        const size_t length = std::strlen(word);
        if (text_.compare(position_, length, word) != 0) return false;
        position_ += length;
        return true;
    }

    bool String(std::string& out) {
        if (position_ >= text_.size() || text_[position_] != '"') return false;
        ++position_;
        out.clear();
        while (position_ < text_.size() && text_[position_] != '"') {
            char c = text_[position_++];
            if (c == '\\' && position_ < text_.size()) {
                const char escape = text_[position_++];
                switch (escape) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': position_ += 4; c = '?'; break;  // no unicode needed here
                    default: c = escape; break;
                }
            }
            out.push_back(c);
        }
        if (position_ >= text_.size()) return false;
        ++position_;
        return true;
    }

    bool Value(Json& out) {
        Skip();
        if (position_ >= text_.size()) return false;

        const char c = text_[position_];
        if (c == '{') {
            ++position_;
            out.kind = Json::Kind::kObject;
            Skip();
            if (position_ < text_.size() && text_[position_] == '}') { ++position_; return true; }
            while (true) {
                Skip();
                std::string key;
                if (!String(key)) return false;
                Skip();
                if (position_ >= text_.size() || text_[position_] != ':') return false;
                ++position_;
                Json child;
                if (!Value(child)) return false;
                out.fields.emplace(std::move(key), std::move(child));
                Skip();
                if (position_ < text_.size() && text_[position_] == ',') { ++position_; continue; }
                if (position_ < text_.size() && text_[position_] == '}') { ++position_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++position_;
            out.kind = Json::Kind::kArray;
            Skip();
            if (position_ < text_.size() && text_[position_] == ']') { ++position_; return true; }
            while (true) {
                Json child;
                if (!Value(child)) return false;
                out.items.push_back(std::move(child));
                Skip();
                if (position_ < text_.size() && text_[position_] == ',') { ++position_; continue; }
                if (position_ < text_.size() && text_[position_] == ']') { ++position_; return true; }
                return false;
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::kString;
            return String(out.text);
        }
        if (Literal("true")) { out.kind = Json::Kind::kBool; out.boolean = true; return true; }
        if (Literal("false")) { out.kind = Json::Kind::kBool; out.boolean = false; return true; }
        if (Literal("null")) { out.kind = Json::Kind::kNull; return true; }

        char* end = nullptr;
        const double parsed = std::strtod(text_.c_str() + position_, &end);
        if (end == text_.c_str() + position_) return false;
        position_ = static_cast<size_t>(end - text_.c_str());
        out.kind = Json::Kind::kNumber;
        out.number = parsed;
        return true;
    }

    const std::string& text_;
    size_t position_ = 0;
};

// ---------------------------------------------------------------------------
// Buffers and accessors
// ---------------------------------------------------------------------------

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> DecodeBase64(const std::string& text) {
    std::vector<uint8_t> out;
    int accumulator = 0, bits = 0;
    for (char c : text) {
        const int value = Base64Value(c);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

size_t ComponentSize(int component_type) {
    switch (component_type) {
        case 5120: case 5121: return 1;  // byte, unsigned byte
        case 5122: case 5123: return 2;  // short, unsigned short
        case 5125: case 5126: return 4;  // unsigned int, float
        default: return 0;
    }
}

size_t ComponentCount(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

struct Gltf {
    Json root;
    std::vector<std::vector<uint8_t>> buffers;

    // Reads accessor `index` as floats (normalizing integers when the accessor
    // says so) or as raw integers, one row of `components` at a time.
    bool ReadAccessor(int index, std::vector<float>& out, size_t& components) const;
    bool ReadAccessorInts(int index, std::vector<uint32_t>& out, size_t& components) const;

 private:
    bool AccessorLayout(int index, const uint8_t*& data, size_t& count, size_t& stride,
                        int& component_type, size_t& components, bool& normalized) const;
};

bool Gltf::AccessorLayout(int index, const uint8_t*& data, size_t& count, size_t& stride,
                          int& component_type, size_t& components, bool& normalized) const {
    const Json* accessors = root.Find("accessors");
    const Json* accessor = accessors ? accessors->At(static_cast<size_t>(index)) : nullptr;
    if (!accessor) return false;

    component_type = accessor->IntField("componentType");
    const Json* type = accessor->Find("type");
    if (!type) return false;
    components = ComponentCount(type->text);
    const size_t element = ComponentSize(component_type);
    if (!components || !element) return false;

    count = static_cast<size_t>(accessor->IntField("count", 0));
    const Json* flag = accessor->Find("normalized");
    normalized = flag && flag->kind == Json::Kind::kBool && flag->boolean;

    const int view_index = accessor->IntField("bufferView", -1);
    if (view_index < 0) return false;
    const Json* views = root.Find("bufferViews");
    const Json* view = views ? views->At(static_cast<size_t>(view_index)) : nullptr;
    if (!view) return false;

    const int buffer_index = view->IntField("buffer", -1);
    if (buffer_index < 0 || static_cast<size_t>(buffer_index) >= buffers.size()) return false;
    const std::vector<uint8_t>& buffer = buffers[static_cast<size_t>(buffer_index)];

    const size_t view_offset = static_cast<size_t>(view->IntField("byteOffset", 0));
    const size_t accessor_offset = static_cast<size_t>(accessor->IntField("byteOffset", 0));
    stride = static_cast<size_t>(view->IntField("byteStride", 0));
    if (stride == 0) stride = element * components;

    const size_t start = view_offset + accessor_offset;
    if (start + (count ? (count - 1) * stride + element * components : 0) > buffer.size()) {
        return false;
    }
    data = buffer.data() + start;
    return true;
}

bool Gltf::ReadAccessor(int index, std::vector<float>& out, size_t& components) const {
    const uint8_t* data = nullptr;
    size_t count = 0, stride = 0;
    int component_type = 0;
    bool normalized = false;
    if (!AccessorLayout(index, data, count, stride, component_type, components, normalized)) {
        return false;
    }

    out.assign(count * components, 0.0f);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* row = data + i * stride;
        for (size_t c = 0; c < components; ++c) {
            const uint8_t* element = row + c * ComponentSize(component_type);
            float value = 0.0f;
            switch (component_type) {
                case 5126: std::memcpy(&value, element, 4); break;
                case 5121: {
                    const uint8_t raw = *element;
                    value = normalized ? raw / 255.0f : static_cast<float>(raw);
                    break;
                }
                case 5123: {
                    uint16_t raw = 0; std::memcpy(&raw, element, 2);
                    value = normalized ? raw / 65535.0f : static_cast<float>(raw);
                    break;
                }
                case 5120: {
                    const int8_t raw = static_cast<int8_t>(*element);
                    value = normalized ? std::max(raw / 127.0f, -1.0f) : static_cast<float>(raw);
                    break;
                }
                case 5122: {
                    int16_t raw = 0; std::memcpy(&raw, element, 2);
                    value = normalized ? std::max(raw / 32767.0f, -1.0f) : static_cast<float>(raw);
                    break;
                }
                case 5125: {
                    uint32_t raw = 0; std::memcpy(&raw, element, 4);
                    value = static_cast<float>(raw);
                    break;
                }
                default: return false;
            }
            out[i * components + c] = value;
        }
    }
    return true;
}

bool Gltf::ReadAccessorInts(int index, std::vector<uint32_t>& out, size_t& components) const {
    const uint8_t* data = nullptr;
    size_t count = 0, stride = 0;
    int component_type = 0;
    bool normalized = false;
    if (!AccessorLayout(index, data, count, stride, component_type, components, normalized)) {
        return false;
    }

    out.assign(count * components, 0);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* row = data + i * stride;
        for (size_t c = 0; c < components; ++c) {
            const uint8_t* element = row + c * ComponentSize(component_type);
            uint32_t value = 0;
            switch (component_type) {
                case 5121: value = *element; break;
                case 5123: { uint16_t raw = 0; std::memcpy(&raw, element, 2); value = raw; break; }
                case 5125: std::memcpy(&value, element, 4); break;
                default: return false;
            }
            out[i * components + c] = value;
        }
    }
    return true;
}

// A node's local transform, row-major, so a point is m * p.
//
// Only unskinned geometry needs it, and only because a file holding a whole
// vehicle relies on it: each part is modelled about its own origin and put where
// it belongs by its node. glTF writes matrices column-major and gives the TRS
// form as an alternative; both are accepted here.
struct Matrix4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Matrix4 Multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 out;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[row * 4 + k] * b.m[k * 4 + column];
            out.m[row * 4 + column] = sum;
        }
    }
    return out;
}

Matrix4 NodeMatrix(const Json& node) {
    Matrix4 out;
    if (const Json* matrix = node.Find("matrix"); matrix && matrix->items.size() == 16) {
        float column_major[16];
        for (size_t i = 0; i < 16; ++i) column_major[i] = matrix->items[i].Real(0.0f);
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                out.m[row * 4 + column] = column_major[column * 4 + row];
        return out;
    }

    float translation[3] = {0, 0, 0}, rotation[4] = {0, 0, 0, 1}, scale[3] = {1, 1, 1};
    if (const Json* value = node.Find("translation"); value && value->items.size() == 3)
        for (size_t i = 0; i < 3; ++i) translation[i] = value->items[i].Real(0.0f);
    if (const Json* value = node.Find("rotation"); value && value->items.size() == 4)
        for (size_t i = 0; i < 4; ++i) rotation[i] = value->items[i].Real(0.0f);
    if (const Json* value = node.Find("scale"); value && value->items.size() == 3)
        for (size_t i = 0; i < 3; ++i) scale[i] = value->items[i].Real(1.0f);

    const float x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
    const float basis[9] = {
        1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
        2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
        2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            out.m[row * 4 + column] = basis[row * 3 + column] * scale[column];
        out.m[row * 4 + 3] = translation[row];
    }
    return out;
}

void TransformPoint(const Matrix4& m, float& x, float& y, float& z) {
    const float px = x, py = y, pz = z;
    x = m.m[0] * px + m.m[1] * py + m.m[2] * pz + m.m[3];
    y = m.m[4] * px + m.m[5] * py + m.m[6] * pz + m.m[7];
    z = m.m[8] * px + m.m[9] * py + m.m[10] * pz + m.m[11];
}

// Normals ride the same basis without the translation. A node that scales
// non-uniformly would want the inverse transpose, but a vehicle's parts are
// placed with rotation and translation only, and renormalising covers uniform
// scale, so the cheap version is the honest one here.
void TransformDirection(const Matrix4& m, float& x, float& y, float& z) {
    const float px = x, py = y, pz = z;
    x = m.m[0] * px + m.m[1] * py + m.m[2] * pz;
    y = m.m[4] * px + m.m[5] * py + m.m[6] * pz;
    z = m.m[8] * px + m.m[9] * py + m.m[10] * pz;
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length > 1e-8f) {
        x /= length; y /= length; z /= length;
    }
}

// Inverts an affine 4x4 stored column-major (the glTF convention) and returns
// the translation of the result -- that is, where the joint sits in bind pose.
void InverseBindTranslation(const float m[16], float out[3]) {
    const float a[3][3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
    const float t[3] = {m[12], m[13], m[14]};

    const float determinant =
        a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
        a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::fabs(determinant) < 1e-12f) {
        out[0] = -t[0]; out[1] = -t[1]; out[2] = -t[2];
        return;
    }
    const float inverse = 1.0f / determinant;

    float b[3][3];
    b[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * inverse;
    b[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inverse;
    b[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inverse;
    b[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * inverse;
    b[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inverse;
    b[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inverse;
    b[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * inverse;
    b[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inverse;
    b[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inverse;

    for (int i = 0; i < 3; ++i) {
        out[i] = -(b[i][0] * t[0] + b[i][1] * t[1] + b[i][2] * t[2]);
    }
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    in.seekg(0);
    out.resize(static_cast<size_t>(size));
    return in.read(reinterpret_cast<char*>(out.data()), size).good() || size == 0;
}

}  // namespace

bool LoadGltf(const std::filesystem::path& path, Mesh& out, std::string& error) {
    std::vector<uint8_t> file;
    if (!ReadFile(path, file)) {
        error = "cannot open " + path.string();
        return false;
    }

    Gltf gltf;
    std::string json_text;
    std::vector<uint8_t> glb_chunk;

    if (file.size() >= 12 && std::memcmp(file.data(), "glTF", 4) == 0) {
        // GLB: 12-byte header, then chunks of {length, type, payload}.
        size_t cursor = 12;
        while (cursor + 8 <= file.size()) {
            uint32_t length = 0, type = 0;
            std::memcpy(&length, file.data() + cursor, 4);
            std::memcpy(&type, file.data() + cursor + 4, 4);
            cursor += 8;
            if (cursor + length > file.size()) break;
            if (type == 0x4E4F534A) {  // 'JSON'
                json_text.assign(reinterpret_cast<const char*>(file.data() + cursor), length);
            } else if (type == 0x004E4942) {  // 'BIN'
                glb_chunk.assign(file.data() + cursor, file.data() + cursor + length);
            }
            cursor += (length + 3) & ~3u;
        }
        if (json_text.empty()) {
            error = "GLB has no JSON chunk";
            return false;
        }
    } else {
        json_text.assign(reinterpret_cast<const char*>(file.data()), file.size());
    }

    JsonParser parser(json_text);
    if (!parser.Parse(gltf.root) || gltf.root.kind != Json::Kind::kObject) {
        error = "malformed glTF JSON";
        return false;
    }

    // Resolve buffers: the GLB chunk, data: URIs, or a sibling .bin.
    if (const Json* buffers = gltf.root.Find("buffers")) {
        for (const Json& buffer : buffers->items) {
            const Json* uri = buffer.Find("uri");
            if (!uri) {
                gltf.buffers.push_back(glb_chunk);
                continue;
            }
            const std::string& text = uri->text;
            const size_t marker = text.find("base64,");
            if (text.rfind("data:", 0) == 0 && marker != std::string::npos) {
                gltf.buffers.push_back(DecodeBase64(text.substr(marker + 7)));
            } else {
                std::vector<uint8_t> side;
                if (!ReadFile(path.parent_path() / text, side)) {
                    error = "cannot open buffer " + text;
                    return false;
                }
                gltf.buffers.push_back(std::move(side));
            }
        }
    }

    // Collect every primitive worth taking. Exporters routinely split a
    // character into one mesh per material -- body, head, shoes, legs -- so
    // reading only the first one leaves a torso floating on its own. Meshes
    // reached through a skinned node win; if nothing is skinned, everything is
    // taken and the result is treated as rigid.
    const Json* meshes = gltf.root.Find("meshes");
    if (!meshes || meshes->items.empty()) {
        error = "glTF has no meshes";
        return false;
    }

    // A primitive, plus what the node it hangs off says about it: where it sits
    // in the scene, and which named group owns it.
    struct PrimitiveRef {
        const Json* primitive = nullptr;
        Matrix4 world;         // identity unless the file places its nodes
        bool placed = false;   // whether `world` is anything but identity
        std::string group;
    };

    const Json* nodes = gltf.root.Find("nodes");
    std::map<int, int> parent_of;
    if (nodes) {
        for (size_t n = 0; n < nodes->items.size(); ++n) {
            const Json* children = nodes->items[n].Find("children");
            if (!children) continue;
            for (const Json& child : children->items)
                parent_of[child.Int(-1)] = static_cast<int>(n);
        }
    }

    // The name to file a primitive under: the closest ancestor carrying a real
    // one. Exporters name the node holding a mesh after its material, so that
    // one is skipped -- the part name lives one level up ("bbme38_hood" over
    // "bbme38_color-material"). Scene roots are not names either.
    auto group_name = [&](int node_index) -> std::string {
        auto usable = [](const std::string& name) {
            if (name.empty()) return false;
            if (name.size() > 9 && name.compare(name.size() - 9, 9, "-material") == 0)
                return false;
            return name != "Sketchfab_model" && name != "RootNode" &&
                   name != "Collada visual scene group" && name != "Scene";
        };
        int current = node_index;
        for (int guard = 0; guard < 64 && current >= 0 && nodes; ++guard) {
            const Json* node = nodes->At(static_cast<size_t>(current));
            if (!node) break;
            if (const Json* name = node->Find("name"); name && usable(name->text))
                return name->text;
            auto parent = parent_of.find(current);
            if (parent == parent_of.end()) break;
            current = parent->second;
        }
        return {};
    };

    std::vector<PrimitiveRef> primitives;
    int skin_index = -1;
    if (nodes) {
        for (size_t n = 0; n < nodes->items.size(); ++n) {
            const Json& node = nodes->items[n];
            const int mesh_index = node.IntField("mesh", -1);
            const int node_skin = node.IntField("skin", -1);
            if (mesh_index < 0 || node_skin < 0) continue;
            if (skin_index >= 0 && node_skin != skin_index) continue;
            const Json* mesh = meshes->At(static_cast<size_t>(mesh_index));
            const Json* list = mesh ? mesh->Find("primitives") : nullptr;
            if (!list) continue;
            skin_index = node_skin;
            // A skinned primitive is posed by its joints; glTF says the node's
            // own transform does not apply to it.
            for (const Json& p : list->items)
                primitives.push_back(PrimitiveRef{&p, Matrix4{}, false,
                                                  group_name(static_cast<int>(n))});
        }
    }
    if (primitives.empty() && nodes) {
        for (size_t n = 0; n < nodes->items.size(); ++n) {
            const Json& node = nodes->items[n];
            const int mesh_index = node.IntField("mesh", -1);
            if (mesh_index < 0) continue;
            const Json* mesh = meshes->At(static_cast<size_t>(mesh_index));
            const Json* list = mesh ? mesh->Find("primitives") : nullptr;
            if (!list) continue;

            // Unskinned geometry IS placed by the scene, and a car file leans on
            // it entirely: every part is modelled at the origin and moved into
            // position by its node. Reading the primitives without the transform
            // piles the whole vehicle on top of itself.
            Matrix4 world = NodeMatrix(*nodes->At(static_cast<size_t>(n)));
            bool placed = true;
            int current = static_cast<int>(n);
            for (int guard = 0; guard < 64; ++guard) {
                auto parent = parent_of.find(current);
                if (parent == parent_of.end()) break;
                current = parent->second;
                const Json* ancestor = nodes->At(static_cast<size_t>(current));
                if (!ancestor) break;
                world = Multiply(NodeMatrix(*ancestor), world);
            }
            for (const Json& p : list->items)
                primitives.push_back(PrimitiveRef{&p, world, placed,
                                                  group_name(static_cast<int>(n))});
        }
    }
    if (primitives.empty()) {
        for (const Json& mesh : meshes->items) {
            const Json* list = mesh.Find("primitives");
            if (!list) continue;
            for (const Json& p : list->items)
                primitives.push_back(PrimitiveRef{&p, Matrix4{}, false, {}});
        }
    }
    if (primitives.empty()) {
        error = "glTF has no primitives";
        return false;
    }

    // Images, kept encoded: decoding belongs to the atlas builder, which is the
    // only thing that knows what size it wants them at. An image can live in a
    // buffer view (the GLB case), in a data: URI, or in a file alongside.
    if (const Json* images = gltf.root.Find("images")) {
        out.images.resize(images->items.size());
        for (size_t i = 0; i < images->items.size(); ++i) {
            const Json& image = images->items[i];
            const int view_index = image.IntField("bufferView", -1);
            if (view_index >= 0) {
                const Json* views = gltf.root.Find("bufferViews");
                const Json* view = views ? views->At(static_cast<size_t>(view_index)) : nullptr;
                if (!view) continue;
                const int buffer_index = view->IntField("buffer", -1);
                if (buffer_index < 0 || static_cast<size_t>(buffer_index) >= gltf.buffers.size())
                    continue;
                const std::vector<uint8_t>& buffer = gltf.buffers[static_cast<size_t>(buffer_index)];
                const size_t offset = static_cast<size_t>(view->IntField("byteOffset", 0));
                const size_t length = static_cast<size_t>(view->IntField("byteLength", 0));
                if (offset + length > buffer.size()) continue;
                out.images[i].assign(buffer.begin() + offset, buffer.begin() + offset + length);
                continue;
            }

            const Json* uri = image.Find("uri");
            if (!uri) continue;
            const std::string& text = uri->text;
            const size_t marker = text.find("base64,");
            if (text.rfind("data:", 0) == 0 && marker != std::string::npos) {
                out.images[i] = DecodeBase64(text.substr(marker + 7));
            } else {
                std::vector<uint8_t> side;
                if (ReadFile(path.parent_path() / text, side)) out.images[i] = std::move(side);
            }
        }
    }

    // material -> its colour texture -> image, flattened once so the primitive
    // loop is a single lookup.
    //
    // Where that texture is named depends on the workflow the exporter used.
    // Metallic-roughness, the core one, calls it baseColorTexture. Anything
    // converted out of an older engine tends to arrive as specular-glossiness
    // instead, an extension whose colour map is diffuseTexture and which leaves
    // pbrMetallicRoughness out entirely -- so looking only for the core name
    // finds nothing at all and the model silently keeps the driver's skin.
    std::vector<int> material_image;
    if (const Json* materials = gltf.root.Find("materials")) {
        material_image.assign(materials->items.size(), -1);
        const Json* textures = gltf.root.Find("textures");
        for (size_t i = 0; i < materials->items.size(); ++i) {
            const Json& material = materials->items[i];

            const Json* slot = nullptr;
            if (const Json* pbr = material.Find("pbrMetallicRoughness"))
                slot = pbr->Find("baseColorTexture");
            if (!slot) {
                if (const Json* extensions = material.Find("extensions")) {
                    if (const Json* gloss =
                            extensions->Find("KHR_materials_pbrSpecularGlossiness")) {
                        slot = gloss->Find("diffuseTexture");
                    }
                }
            }

            const int texture_index = slot ? slot->IntField("index", -1) : -1;
            const Json* texture =
                (textures && texture_index >= 0) ? textures->At(static_cast<size_t>(texture_index))
                                                 : nullptr;
            if (texture) material_image[i] = texture->IntField("source", -1);
        }
    }

    // Joint bind positions come from the skin and are shared by every
    // primitive, so they are read once.
    std::vector<float> matrices;
    size_t joint_count = 0;
    if (skin_index >= 0) {
        const Json* skins = gltf.root.Find("skins");
        const Json* skin = skins ? skins->At(static_cast<size_t>(skin_index)) : nullptr;
        const Json* joint_list = skin ? skin->Find("joints") : nullptr;
        const int matrices_accessor = skin ? skin->IntField("inverseBindMatrices", -1) : -1;
        size_t components = 0;
        if (joint_list && matrices_accessor >= 0 &&
            gltf.ReadAccessor(matrices_accessor, matrices, components) && components == 16 &&
            matrices.size() / 16 >= joint_list->items.size()) {
            joint_count = joint_list->items.size();
            out.joint_bind.assign(joint_count * 3, 0.0f);
            out.joint_inverse_bind.assign(matrices.begin(), matrices.begin() + joint_count * 16);
            for (size_t j = 0; j < joint_count; ++j) {
                InverseBindTranslation(&matrices[j * 16], &out.joint_bind[j * 3]);
            }

            // Joint hierarchy, in joint numbering. glTF records children, so
            // the parent link is recovered by inverting that, and node indices
            // are then translated into joint indices.
            std::map<int, int> joint_of_node;
            for (size_t j = 0; j < joint_count; ++j)
                joint_of_node.emplace(joint_list->items[j].Int(-1), static_cast<int>(j));

            out.joint_parent.assign(joint_count, -1);
            if (const Json* nodes = gltf.root.Find("nodes")) {
                for (size_t n = 0; n < nodes->items.size(); ++n) {
                    const Json* children = nodes->items[n].Find("children");
                    if (!children) continue;
                    auto parent = joint_of_node.find(static_cast<int>(n));
                    if (parent == joint_of_node.end()) continue;
                    for (const Json& child : children->items) {
                        auto entry = joint_of_node.find(child.Int(-1));
                        if (entry != joint_of_node.end())
                            out.joint_parent[static_cast<size_t>(entry->second)] = parent->second;
                    }
                }
            }
        }
    }

    bool any_skin = false;
    for (const PrimitiveRef& reference : primitives) {
        const Json* primitive = reference.primitive;
        const Json* attributes = primitive->Find("attributes");
        if (!attributes) continue;

        std::vector<float> positions, normals, uvs, weights;
        std::vector<uint32_t> joints, indices;
        size_t components = 0;

        const int position_accessor = attributes->IntField("POSITION", -1);
        if (position_accessor < 0 || !gltf.ReadAccessor(position_accessor, positions, components) ||
            components != 3) {
            continue;
        }
        const size_t vertex_count = positions.size() / 3;
        const uint32_t base = static_cast<uint32_t>(out.vertices.size());

        if (const int accessor = attributes->IntField("NORMAL", -1); accessor >= 0) {
            gltf.ReadAccessor(accessor, normals, components);
            if (components != 3) normals.clear();
        }
        if (const int accessor = attributes->IntField("TEXCOORD_0", -1); accessor >= 0) {
            gltf.ReadAccessor(accessor, uvs, components);
            if (components != 2) uvs.clear();
        }
        if (const int accessor = attributes->IntField("JOINTS_0", -1); accessor >= 0) {
            gltf.ReadAccessorInts(accessor, joints, components);
            if (components != 4) joints.clear();
        }
        if (const int accessor = attributes->IntField("WEIGHTS_0", -1); accessor >= 0) {
            gltf.ReadAccessor(accessor, weights, components);
            if (components != 4) weights.clear();
        }
        if (const int accessor = primitive->IntField("indices", -1); accessor >= 0) {
            gltf.ReadAccessorInts(accessor, indices, components);
            if (components != 1) indices.clear();
        }

        for (size_t i = 0; i < vertex_count; ++i) {
            MeshVertex vertex;
            vertex.px = positions[i * 3 + 0];
            vertex.py = positions[i * 3 + 1];
            vertex.pz = positions[i * 3 + 2];
            if (normals.size() >= (i + 1) * 3) {
                vertex.nx = normals[i * 3 + 0];
                vertex.ny = normals[i * 3 + 1];
                vertex.nz = normals[i * 3 + 2];
            }
            if (uvs.size() >= (i + 1) * 2) {
                vertex.u = uvs[i * 2 + 0];
                vertex.v = uvs[i * 2 + 1];
            }
            if (reference.placed) {
                TransformPoint(reference.world, vertex.px, vertex.py, vertex.pz);
                TransformDirection(reference.world, vertex.nx, vertex.ny, vertex.nz);
            }
            out.vertices.push_back(vertex);

            MeshSkin skin;
            if (joint_count && joints.size() >= (i + 1) * 4 && weights.size() >= (i + 1) * 4) {
                float total = 0.0f;
                for (int c = 0; c < 4; ++c) total += weights[i * 4 + c];
                if (total <= 0.0f) total = 1.0f;
                for (int c = 0; c < 4; ++c) {
                    skin.joint[c] = static_cast<uint16_t>(joints[i * 4 + c]);
                    skin.weight[c] = weights[i * 4 + c] / total;
                }
                any_skin = true;
            }
            out.skin.push_back(skin);
        }

        MeshPart part;
        part.first_vertex = base;
        part.vertex_count = static_cast<uint32_t>(vertex_count);
        part.group = reference.group;
        const int material = primitive->IntField("material", -1);
        if (material >= 0 && static_cast<size_t>(material) < material_image.size())
            part.image = material_image[static_cast<size_t>(material)];
        out.parts.push_back(part);

        if (!indices.empty()) {
            for (uint32_t index : indices) out.indices.push_back(base + index);
        } else {
            for (size_t i = 0; i < vertex_count; ++i)
                out.indices.push_back(base + static_cast<uint32_t>(i));
        }
    }

    if (!any_skin) {
        out.skin.clear();
        out.joint_bind.clear();
    }

    if (out.vertices.empty() || out.indices.empty()) {
        error = "glTF primitive has no geometry";
        return false;
    }
    return true;
}

}  // namespace mc::modloader
