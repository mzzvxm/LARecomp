#include "atlas_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>

namespace mc::cinematic::atlas {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// Caps on what a decode believes: garbage memory must end in an error, not in a loop or an allocation.
constexpr uint32_t kMaxLights = 200000;
constexpr uint32_t kMaxRails = 200000;
constexpr uint32_t kMaxNodes = 100000;
constexpr uint32_t kMaxCvs = 2000000;
constexpr uint32_t kMaxKnotVectors = 4096;
constexpr uint32_t kMaxKnots = 512;
constexpr uint32_t kMaxRailCvs = 512;
constexpr uint32_t kMaxCars = 1024;

constexpr double kMaxSpeed = 30.0;      // m/s, the A* heuristic (the fastest road is 29.06)
constexpr double kTurnSpeed = 9.0;      // m/s through an intersection piece
constexpr double kLatAccel = 5.0;       // m/s^2: the curve limit v = sqrt(kLatAccel * R)
constexpr double kAccel = 3.5, kDecel = 6.0;

uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }

double Dist(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
Vec3 Lerp(const Vec3& a, const Vec3& b, double u) { return {a.x + u * (b.x - a.x), a.y + u * (b.y - a.y), a.z + u * (b.z - a.z)}; }

void Fail(std::string* error, const char* what) {
    if (error && error->empty()) *error = what;
}

}  // namespace

// ================================================================================================================ guest memory

uint32_t GuestView::U32(uint32_t addr) const {
    uint8_t b[4];
    if (!Read(addr, b, 4)) {
        failed_ = true;
        return 0;
    }
    return BE32(b);
}

uint16_t GuestView::U16(uint32_t addr) const {
    uint8_t b[2];
    if (!Read(addr, b, 2)) {
        failed_ = true;
        return 0;
    }
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

uint8_t GuestView::U8(uint32_t addr) const {
    uint8_t b = 0;
    if (!Read(addr, &b, 1)) {
        failed_ = true;
        return 0;
    }
    return b;
}

float GuestView::F32(uint32_t addr) const {
    const uint32_t bits = U32(addr);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::string GuestView::CStr(uint32_t addr, uint32_t limit) const {
    std::string s;
    for (uint32_t i = 0; i < limit; ++i) {
        uint8_t c = 0;
        if (!Read(addr + i, &c, 1)) {
            failed_ = true;
            break;
        }
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

bool BufferView::Read(uint32_t addr, void* dst, uint32_t bytes) const {
    if (addr < base_) return false;
    const uint64_t off = uint64_t(addr) - base_;
    if (off + bytes > size_) return false;
    std::memcpy(dst, data_ + off, bytes);
    return true;
}

// ================================================================================================================ decoding

const char* NodeKindName(NodeKind kind) {
    switch (kind) {
        case NodeKind::kRoad: return "road";
        case NodeKind::kIntersection: return "intersection";
        case NodeKind::kLeftTurn: return "left_turn";
        case NodeKind::kRightTurn: return "right_turn";
        case NodeKind::kUTurn: return "u_turn";
        case NodeKind::kTraffic: return "traffic";
    }
    return "?";
}

namespace {

bool LooksLikeLight(const GuestView& view, uint32_t a) {
    if (a < 0x10000) return false;
    float f[8];
    for (int i = 0; i < 3; ++i) {
        f[i] = view.F32(a + 4 * i);
        f[3 + i] = view.F32(a + 16 + 4 * i);
    }
    f[6] = view.F32(a + 64);
    f[7] = view.F32(a + 68);
    const uint8_t type = view.U8(a + 88), on = view.U8(a + 89);
    if (view.failed()) {
        view.ClearFailed();
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(f[i]) || std::fabs(f[i]) > 1.0e5f) return false;
        if (!std::isfinite(f[3 + i]) || f[3 + i] < -0.001f || f[3 + i] > 100.0f) return false;
    }
    return std::isfinite(f[6]) && f[6] >= 0.0f && f[6] < 1.0e5f && std::isfinite(f[7]) && f[7] >= 0.0f && f[7] <= 360.5f && type <= 1 && on <= 1;
}

}  // namespace

bool DecodeLights(const GuestView& view, uint32_t list_addr, std::vector<Light>* out, std::string* error) {
    out->clear();
    view.ClearFailed();
    const uint32_t ptr = view.U32(list_addr);
    const uint32_t count = view.U16(list_addr + 4);
    if (view.failed() || ptr == 0) {
        Fail(error, "the light list is not readable");
        return false;
    }
    if (count == 0 || count > kMaxLights) {
        Fail(error, "the light count is out of range");
        return false;
    }
    // Pointer array or 96-byte records? Told by looking at the first light as a record: a float coordinate and a pointer cannot be told apart by range
    // (a pointer into the heap reads as a float too), a record can: finite position, a type and an on byte that are 0 or 1, an intensity in range.
    const bool pointers = !LooksLikeLight(view, ptr) && count > 1 && LooksLikeLight(view, view.U32(ptr + 4));
    if (!pointers && !LooksLikeLight(view, ptr)) {
        Fail(error, "the light list is neither 96-byte lights nor an array of pointers to them");
        return false;
    }
    const uint32_t w0 = view.U32(ptr);
    out->reserve(count);
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t a = ptr + 96 * k;
        if (pointers) {
            a = view.U32(ptr + 4 * k);
            if (k == 0 || a == w0 || a == 0) continue;  // entry 0 is the null light every empty grid cell points at
        }
        Light l;
        l.i = static_cast<int>(out->size());
        for (int c = 0; c < 3; ++c) {
            l.pos[c] = view.F32(a + 4 * c);
            l.color[c] = view.F32(a + 16 + 4 * c);
            l.dir[c] = view.F32(a + 32 + 4 * c);
        }
        l.intensity = view.F32(a + 64);
        l.cone = view.F32(a + 68);
        l.spread = view.F32(a + 72);
        l.spot = view.U8(a + 88) == 1;
        l.on = view.U8(a + 89) != 0;
        if (view.failed()) {
            Fail(error, "a light is not readable");
            return false;
        }
        out->push_back(l);
    }
    return !out->empty();
}

Vec3 EvalBSpline(const std::vector<Vec3>& cvs, const std::vector<float>& knots, int deg, double t) {
    const int n = static_cast<int>(cvs.size()) - 1;
    int k;
    if (t >= knots[n + 1]) {
        k = n;
    } else {
        k = deg;
        while (k < n && knots[k + 1] <= t) ++k;
    }
    Vec3 d[8];
    const int dg = std::min(deg, 7);
    for (int j = 0; j <= dg; ++j) d[j] = cvs[j + k - dg];
    for (int r = 1; r <= dg; ++r) {
        for (int j = dg; j >= r; --j) {
            const double den = knots[j + 1 + k - r] - knots[j + k - dg];
            const double a = den == 0.0 ? 0.0 : (t - knots[j + k - dg]) / den;
            d[j] = {(1 - a) * d[j - 1].x + a * d[j].x, (1 - a) * d[j - 1].y + a * d[j].y, (1 - a) * d[j - 1].z + a * d[j].z};
        }
    }
    return d[dg];
}

bool DecodeRailnet(const GuestView& view, uint32_t root, World* world, std::string* error, double step_m, int max_pts) {
    view.ClearFailed();
    max_pts = std::clamp(max_pts, 2, 255);
    const uint32_t cv_ptr = view.U32(root + 0x2C), kv_ptr = view.U32(root + 0x30);
    const uint32_t n_cv = view.U32(root + 0x40), n_kv = view.U32(root + 0x44);
    if (view.failed() || !cv_ptr || !kv_ptr || n_cv == 0 || n_cv > kMaxCvs || n_kv == 0 || n_kv > kMaxKnotVectors) {
        Fail(error, "the rail network's header is not what the reader knows");
        return false;
    }
    struct Array {
        uint32_t ptr = 0, stride = 0, count = 0;
    };
    Array arrays[14];
    for (int k = 0; k < 14; ++k) {
        arrays[k].ptr = view.U32(root + 80 + 4 * k);
        arrays[k].stride = view.U32(root + 136 + 4 * k);
        arrays[k].count = view.U32(root + 0xF8 + 4 * k);
    }
    const Array& rails = arrays[0];
    if (view.failed() || !rails.ptr || rails.stride < 88 || rails.stride > 1024 || rails.count == 0 || rails.count > kMaxRails) {
        Fail(error, "the rail array is not what the reader knows");
        return false;
    }

    world->nodes.clear();
    world->rails.clear();
    std::unordered_map<uint32_t, int> node_of;
    struct RoadDir {
        float x = 0.0f, z = 0.0f;
    };
    std::vector<RoadDir> road_dirs;
    const struct {
        int k;
        NodeKind kind;
    } order[] = {{1, NodeKind::kRoad}, {2, NodeKind::kIntersection}, {4, NodeKind::kLeftTurn},
                 {5, NodeKind::kRightTurn}, {6, NodeKind::kUTurn}, {7, NodeKind::kTraffic}};
    for (const auto& o : order) {
        const Array& a = arrays[o.k];
        if (!a.ptr) continue;
        if (a.count > kMaxNodes || a.stride < 36 || (o.kind == NodeKind::kRoad && a.stride < 176)) {
            Fail(error, "a node array is not what the reader knows");
            return false;
        }
        for (uint32_t i = 0; i < a.count; ++i) {
            const uint32_t addr = a.ptr + a.stride * i;
            Node nd;
            nd.id = static_cast<int>(world->nodes.size());
            nd.kind = o.kind;
            RoadDir rd;
            if (o.kind == NodeKind::kRoad) {
                const uint32_t np = view.U32(addr + 144);
                if (np) nd.name = view.CStr(np, 96);
                nd.speed = view.F32(addr + 148);
                nd.cls = static_cast<int32_t>(view.U32(addr + 152));
                rd.x = view.F32(addr + 96);
                rd.z = view.F32(addr + 104);
            }
            node_of[addr] = nd.id;
            world->nodes.push_back(std::move(nd));
            road_dirs.push_back(rd);
        }
    }

    std::vector<std::vector<float>> knot_cache(n_kv);
    std::vector<char> knot_read(n_kv, 0);
    world->rails.reserve(rails.count);
    for (uint32_t i = 0; i < rails.count; ++i) {
        const uint32_t o = rails.ptr + rails.stride * i;
        Rail r;
        r.id = static_cast<int>(i);
        const uint32_t n1 = view.U16(o + 76);
        r.deg = view.U16(o + 78);
        const uint32_t kvi = view.U16(o + 84);
        const uint32_t idxp = view.U32(o + 80);
        r.flags = view.U32(o + 4);
        r.len = view.F32(o + 12);
        const uint32_t owner = view.U32(o + 8);
        if (view.failed() || n1 + 1 > kMaxRailCvs || n1 < 1 || kvi >= n_kv || r.deg < 1 || r.deg > 7 || !idxp) {
            Fail(error, "a rail is not what the reader knows");
            return false;
        }
        std::vector<Vec3> cvs;
        cvs.reserve(n1 + 1);
        for (uint32_t j = 0; j <= n1; ++j) {
            const uint16_t ci = view.U16(idxp + 2 * j);
            if (ci >= n_cv) {
                Fail(error, "a rail names a control vertex that does not exist");
                return false;
            }
            r.cv.push_back(ci);
            const uint32_t a = cv_ptr + 16 * ci;
            cvs.push_back({view.F32(a), view.F32(a + 4), view.F32(a + 8)});
        }
        if (!knot_read[kvi]) {
            const uint32_t o2 = kv_ptr + 12 * kvi;
            const uint32_t kp = view.U32(o2 + 4), kn = view.U32(o2 + 8);
            if (view.failed() || kn == 0 || kn > kMaxKnots) {
                Fail(error, "a knot vector is not what the reader knows");
                return false;
            }
            for (uint32_t j = 0; j < kn; ++j) knot_cache[kvi].push_back(view.F32(kp + 4 * j));
            knot_read[kvi] = 1;
        }
        const std::vector<float>& kn = knot_cache[kvi];
        const int ncv = static_cast<int>(cvs.size());
        const bool spline = ncv > r.deg && static_cast<int>(kn.size()) >= ncv + r.deg + 1;
        const int npts = std::clamp(static_cast<int>(r.len / step_m) + 2, 2, max_pts);
        r.pts.reserve(3 * npts);
        for (int s = 0; s < npts; ++s) {
            Vec3 p;
            if (spline) {
                const double t0 = kn[r.deg], t1 = kn[ncv];
                p = EvalBSpline(cvs, kn, r.deg, t0 + (t1 - t0) * s / (npts - 1));
            } else {
                p = cvs[std::min<size_t>(s, cvs.size() - 1)];
            }
            r.pts.push_back(static_cast<float>(p.x));
            r.pts.push_back(static_cast<float>(p.y));
            r.pts.push_back(static_cast<float>(p.z));
        }
        const auto it = node_of.find(owner);
        r.node = it == node_of.end() ? -1 : it->second;
        if (r.node >= 0) world->nodes[r.node].rails.push_back(r.id);
        world->rails.push_back(std::move(r));
    }
    if (view.failed()) {
        Fail(error, "the rail network ended in memory the reader could not read");
        return false;
    }

    // The lane graph: a rail that ENDS at a control vertex feeds every rail that STARTS there.
    std::unordered_map<uint16_t, std::vector<int>> starts;
    for (const Rail& r : world->rails) starts[r.cv.front()].push_back(r.id);
    for (Rail& a : world->rails) {
        const auto it = starts.find(a.cv.back());
        if (it == starts.end()) continue;
        for (int b : it->second) {
            if (b == a.id) continue;
            a.next.push_back(b);
            world->rails[b].prev.push_back(a.id);
        }
    }
    // Side: a road rail's direction against its road's reference direction.
    for (Rail& r : world->rails) {
        if (r.node < 0 || world->nodes[r.node].kind != NodeKind::kRoad || r.point_count() < 2) continue;
        const RoadDir& rd = road_dirs[r.node];
        const Vec3 a = r.point(0), b = r.point(r.point_count() - 1);
        r.dir = ((b.x - a.x) * rd.x + (b.z - a.z) * rd.z) >= 0.0 ? 1 : -1;
    }
    return true;
}

std::string CarClassOf(std::string_view name) {
    auto starts = [&](std::string_view p) { return name.substr(0, p.size()) == p; };
    if (starts("vp_se_")) return "police";
    if (starts("vpd_")) return "dub";
    if (starts("vp_")) return "player";
    if (starts("va_")) return "ambient";
    return "other";
}

bool DecodeCars(const GuestView& view, uint32_t table, std::vector<CarModel>* out, std::string* error) {
    out->clear();
    view.ClearFailed();
    const uint32_t recs = view.U32(table);
    const uint32_t count = view.U16(table + 4);
    if (view.failed() || !recs || count == 0 || count > kMaxCars) {
        Fail(error, "the car table is not readable");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t np = view.U32(recs + 104 * i);
        if (!np) continue;
        CarModel c;
        c.index = static_cast<int>(i);
        c.name = view.CStr(np, 64);
        c.cls = CarClassOf(c.name);
        if (!c.name.empty()) out->push_back(std::move(c));
    }
    return !view.failed() && !out->empty();
}

std::vector<Garage> StaticGarages() {
    return {{"hollywood", {819.693, 17.731, -804.765}}, {"beaches", {-3049.5, -25.0, 160.0}}, {"sc", {-53.6, -19.6, 1331.6}}};
}

bool ReadGarages(const GuestView& view, std::vector<Garage>* out) {
    view.ClearFailed();
    // The float constants sub_826AB070 subtracts from the player's position (lis hi / lfs lo(hi): hi << 16 + signed lo).
    constexpr uint32_t k_c = 0x820C0000u, k_7 = 0x82070000u, k_8 = 0x82080000u;
    out->clear();
    out->push_back({"hollywood", {view.F32(k_c - 23728u), view.F32(k_c - 23720u), view.F32(k_c - 23724u)}});
    out->push_back({"beaches", {view.F32(k_c - 23732u), view.F32(k_7 - 12524u), view.F32(k_8 + 26200u)}});
    out->push_back({"sc", {view.F32(k_c - 23744u), view.F32(k_c - 23736u), view.F32(k_c - 23740u)}});
    return !view.failed();
}

// ================================================================================================================ queries

namespace {

int64_t CellKey(int32_t cx, int32_t cz) { return (int64_t(cx) << 32) ^ int64_t(uint32_t(cz)); }
int32_t CellOf(double v) { return static_cast<int32_t>(std::floor(v / Index::kCell)); }

double Heading(double dx, double dz) {
    double h = std::atan2(dx, dz) / kDeg;
    if (h < 0.0) h += 360.0;
    return h;
}

}  // namespace

Index::Index(const World& world) : world_(&world) {
    for (const Rail& r : world.rails) {
        const size_t n = r.point_count();
        for (size_t s = 0; s + 1 < n && s < 255; ++s) {
            const Vec3 a = r.point(s), b = r.point(s + 1);
            for (int32_t cx = CellOf(std::min(a.x, b.x)); cx <= CellOf(std::max(a.x, b.x)); ++cx)
                for (int32_t cz = CellOf(std::min(a.z, b.z)); cz <= CellOf(std::max(a.z, b.z)); ++cz)
                    seg_cells_[CellKey(cx, cz)].push_back((static_cast<uint32_t>(r.id) << 8) | static_cast<uint32_t>(s));
        }
    }
    for (size_t i = 0; i < world.lights.size(); ++i)
        light_cells_[CellKey(CellOf(world.lights[i].pos[0]), CellOf(world.lights[i].pos[2]))].push_back(static_cast<uint32_t>(i));
}

void Index::SegmentsNear(double x, double z, double r, std::vector<uint32_t>* out) const {
    out->clear();
    for (int32_t cx = CellOf(x - r); cx <= CellOf(x + r); ++cx) {
        for (int32_t cz = CellOf(z - r); cz <= CellOf(z + r); ++cz) {
            const auto it = seg_cells_.find(CellKey(cx, cz));
            if (it != seg_cells_.end()) out->insert(out->end(), it->second.begin(), it->second.end());
        }
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
}

void Index::LightsNear(double x, double z, double r, std::vector<int>* out) const {
    out->clear();
    for (int32_t cx = CellOf(x - r); cx <= CellOf(x + r); ++cx) {
        for (int32_t cz = CellOf(z - r); cz <= CellOf(z + r); ++cz) {
            const auto it = light_cells_.find(CellKey(cx, cz));
            if (it != light_cells_.end()) out->insert(out->end(), it->second.begin(), it->second.end());
        }
    }
}

double SpeedOf(const World& world, const Rail& rail) {
    if (rail.node >= 0 && rail.node < static_cast<int>(world.nodes.size())) {
        const Node& nd = world.nodes[rail.node];
        if (nd.speed > 0.0f) return nd.speed;
    }
    return kTurnSpeed;
}

std::string_view NameOf(const World& world, const Rail& rail) {
    if (rail.node >= 0 && rail.node < static_cast<int>(world.nodes.size())) return world.nodes[rail.node].name;
    return {};
}

double RailT(const Rail& rail, int seg, double u) {
    const size_t n = rail.point_count();
    if (n < 2) return 0.0;
    double total = 0.0, before = 0.0, seg_len = 0.0;
    for (size_t i = 0; i + 1 < n; ++i) {
        const double d = Dist(rail.point(i), rail.point(i + 1));
        if (static_cast<int>(i) < seg) before += d;
        if (static_cast<int>(i) == seg) seg_len = d;
        total += d;
    }
    return total <= 0.0 ? 0.0 : (before + u * seg_len) / total;
}

std::vector<Projection> ProjectOnRails(const Index& index, double x, double z, bool has_y, double y, double r, bool has_heading, double heading,
                                       double max_heading_err) {
    std::vector<uint32_t> segs;
    index.SegmentsNear(x, z, r, &segs);
    const World& w = index.world();
    std::unordered_map<int, Projection> best;
    for (uint32_t packed : segs) {
        const int rid = static_cast<int>(packed >> 8);
        const int s = static_cast<int>(packed & 255u);
        const Rail& rail = w.rails[rid];
        const Vec3 a = rail.point(s), b = rail.point(s + 1);
        const double dx = b.x - a.x, dz = b.z - a.z;
        const double l2 = dx * dx + dz * dz;
        const double u = l2 < 1e-12 ? 0.0 : std::clamp(((x - a.x) * dx + (z - a.z) * dz) / l2, 0.0, 1.0);
        const Vec3 q = Lerp(a, b, u);
        const double dxz = std::hypot(x - q.x, z - q.z);
        if (dxz > r) continue;
        const double d = has_y ? std::hypot(dxz, q.y - y) : dxz;
        const double h = Heading(dx, dz);
        if (has_heading) {
            double e = std::fabs(std::fmod(h - heading + 540.0, 360.0) - 180.0);
            if (e > max_heading_err) continue;
        }
        const auto it = best.find(rid);
        if (it == best.end() || d < it->second.dist) {
            Projection p;
            p.rail = rid;
            p.seg = s;
            p.u = u;
            p.dist = d;
            p.point = q;
            p.heading = h;
            best[rid] = p;
        }
    }
    std::vector<Projection> out;
    out.reserve(best.size());
    for (auto& kv : best) {
        kv.second.t = RailT(w.rails[kv.first], kv.second.seg, kv.second.u);
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const Projection& a, const Projection& b) { return a.dist < b.dist || (a.dist == b.dist && a.rail < b.rail); });
    return out;
}

LightsAnswer NearLights(const Index& index, double x, double z, bool has_y, double y, double r, int n, double min_intensity, std::string_view kind) {
    std::vector<int> cand;
    index.LightsNear(x, z, r, &cand);
    const World& w = index.world();
    std::vector<LightHit> hits;
    for (int i : cand) {
        const Light& l = w.lights[i];
        const double dx = l.pos[0] - x, dz = l.pos[2] - z;
        const double d = has_y ? std::sqrt(dx * dx + dz * dz + (l.pos[1] - y) * (l.pos[1] - y)) : std::hypot(dx, dz);
        if (d > r || l.intensity < min_intensity) continue;
        if (!kind.empty() && kind != (l.spot ? "spot" : "omni")) continue;
        hits.push_back({i, d, std::sqrt(100.0 * std::max(0.0, double(l.intensity)))});
    }
    LightsAnswer a;
    a.in_radius = static_cast<int>(hits.size());
    std::sort(hits.begin(), hits.end(), [](const LightHit& p, const LightHit& q) { return p.dist < q.dist || (p.dist == q.dist && p.light < q.light); });
    if (n >= 0 && hits.size() > static_cast<size_t>(n)) hits.resize(n);
    a.hits = std::move(hits);
    return a;
}

bool GroundFromRails(const Index& index, double x, double z, double* y, double* dist) {
    const auto p = ProjectOnRails(index, x, z, false, 0.0, 25.0, false, 0.0);
    if (p.empty()) return false;
    *y = p[0].point.y;
    *dist = p[0].dist;
    return true;
}

// ---- routes

namespace {

struct EndPoint {
    int rail = -1;
    int seg = 0;
    double u = 0.0;
    Vec3 point;
    double dist = 0.0;
};

bool Resolve(const Index& index, const RouteEnd& e, EndPoint* out, std::string* error) {
    const World& w = index.world();
    if (e.by_rail) {
        if (e.rail < 0 || e.rail >= static_cast<int>(w.rails.size())) {
            *error = "no such rail";
            return false;
        }
        const Rail& r = w.rails[e.rail];
        double total = 0.0;
        for (size_t i = 0; i + 1 < r.point_count(); ++i) total += Dist(r.point(i), r.point(i + 1));
        const double want = std::clamp(e.t, 0.0, 1.0) * total;
        double acc = 0.0;
        const int last = static_cast<int>(r.point_count()) - 2;
        for (int s = 0; s <= last; ++s) {
            const double d = Dist(r.point(s), r.point(s + 1));
            if (want <= acc + d || s == last) {
                out->rail = e.rail;
                out->seg = s;
                out->u = d <= 0.0 ? 0.0 : std::clamp((want - acc) / d, 0.0, 1.0);
                out->point = Lerp(r.point(s), r.point(s + 1), out->u);
                out->dist = 0.0;
                return true;
            }
            acc += d;
        }
        *error = "the rail has no length";
        return false;
    }
    auto hits = ProjectOnRails(index, e.x, e.z, e.has_y, e.y, 80.0, e.has_heading, e.heading);
    if (hits.empty()) hits = ProjectOnRails(index, e.x, e.z, e.has_y, e.y, 400.0, false, 0.0);
    if (hits.empty()) {
        *error = "no road near the point";
        return false;
    }
    out->rail = hits[0].rail;
    out->seg = hits[0].seg;
    out->u = hits[0].u;
    out->point = hits[0].point;
    out->dist = hits[0].dist;
    return true;
}

std::vector<Vec3> Slice(const Rail& r, int seg0, double u0, int seg1, double u1) {
    std::vector<Vec3> out;
    out.push_back(Lerp(r.point(seg0), r.point(seg0 + 1), u0));
    if (seg0 != seg1) {
        for (int i = seg0 + 1; i <= seg1; ++i) out.push_back(r.point(i));
    }
    out.push_back(Lerp(r.point(seg1), r.point(seg1 + 1), u1));
    return out;
}

// A* over rails: `starts` are the rails the path may begin with (and their cost); returns the rails start..goal, or empty.
std::vector<int> Search(const World& w, const std::unordered_map<int, double>& starts, int goal) {
    const Rail& gr = w.rails[goal];
    const Vec3 gp = gr.point(gr.point_count() / 2);
    using Item = std::pair<double, int>;  // priority, rail
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    std::unordered_map<int, double> cost;
    std::unordered_map<int, int> prev;
    for (const auto& s : starts) {
        cost[s.first] = s.second;
        heap.push({s.second + Dist(w.rails[s.first].point(w.rails[s.first].point_count() - 1), gp) / kMaxSpeed, s.first});
    }
    while (!heap.empty()) {
        const int rid = heap.top().second;
        heap.pop();
        const double c = cost[rid];
        if (rid == goal) {
            std::vector<int> path{rid};
            while (prev.count(path.back())) path.push_back(prev[path.back()]);
            std::reverse(path.begin(), path.end());
            return path;
        }
        for (int nx : w.rails[rid].next) {
            const Rail& n = w.rails[nx];
            const double nc = c + n.len / std::max(1.0, SpeedOf(w, n));
            const auto it = cost.find(nx);
            if (it == cost.end() || nc < it->second) {
                cost[nx] = nc;
                prev[nx] = rid;
                heap.push({nc + Dist(n.point(n.point_count() - 1), gp) / kMaxSpeed, nx});
            }
        }
    }
    return {};
}

bool Leg(const World& w, const EndPoint& a, const EndPoint& b, std::vector<std::pair<int, std::vector<Vec3>>>* pieces, std::string* error) {
    const Rail& ra = w.rails[a.rail];
    const Rail& rb = w.rails[b.rail];
    if (a.rail == b.rail && (b.seg > a.seg || (b.seg == a.seg && b.u >= a.u))) {
        pieces->push_back({a.rail, Slice(ra, a.seg, a.u, b.seg, b.u)});
        return true;
    }
    std::unordered_map<int, double> c0;
    for (int nx : ra.next) c0[nx] = w.rails[nx].len / std::max(1.0, SpeedOf(w, w.rails[nx]));
    if (c0.empty()) {
        *error = "no road continues from rail " + std::to_string(a.rail);
        return false;
    }
    const std::vector<int> path = Search(w, c0, b.rail);
    if (path.empty()) {
        *error = "no drivable way from rail " + std::to_string(a.rail) + " to rail " + std::to_string(b.rail);
        return false;
    }
    pieces->push_back({a.rail, Slice(ra, a.seg, a.u, static_cast<int>(ra.point_count()) - 2, 1.0)});
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Rail& r = w.rails[path[i]];
        std::vector<Vec3> all;
        for (size_t k = 0; k < r.point_count(); ++k) all.push_back(r.point(k));
        pieces->push_back({path[i], std::move(all)});
    }
    pieces->push_back({b.rail, Slice(rb, 0, 0.0, b.seg, b.u)});
    return true;
}

void Profile(const std::vector<Vec3>& pts, const std::vector<double>& limit, double cap, std::vector<double>* out) {
    const size_t n = pts.size();
    std::vector<double>& v = *out;
    v.resize(n);
    for (size_t i = 0; i < n; ++i) v[i] = std::min(limit[i], cap);
    for (size_t i = 1; i + 1 < n; ++i) {
        const double ab = Dist(pts[i - 1], pts[i]), bc = Dist(pts[i], pts[i + 1]), ac = Dist(pts[i - 1], pts[i + 1]);
        if (ab < 0.05 || bc < 0.05 || ac < 0.05) continue;
        const double s = (ab + bc + ac) / 2.0;
        const double area2 = std::max(0.0, s * (s - ab) * (s - bc) * (s - ac));
        const double k = 4.0 * std::sqrt(area2) / (ab * bc * ac);  // Menger curvature
        if (k > 1e-4) v[i] = std::min(v[i], std::max(3.0, std::sqrt(kLatAccel / k)));
    }
    for (size_t i = 1; i < n; ++i) v[i] = std::min(v[i], std::sqrt(v[i - 1] * v[i - 1] + 2.0 * kAccel * Dist(pts[i - 1], pts[i])));
    for (size_t i = n - 1; i-- > 0;) v[i] = std::min(v[i], std::sqrt(v[i + 1] * v[i + 1] + 2.0 * kDecel * Dist(pts[i], pts[i + 1])));
}

}  // namespace

RouteAnswer PlanRoute(const Index& index, const RouteEnd& from, const std::vector<RouteEnd>& via, const RouteEnd& to, double speed_cap) {
    RouteAnswer ans;
    const World& w = index.world();
    if (w.rails.empty()) {
        ans.error = "this atlas has no rails";
        return ans;
    }
    std::vector<EndPoint> ends;
    std::vector<const RouteEnd*> specs{&from};
    for (const RouteEnd& v : via) specs.push_back(&v);
    specs.push_back(&to);
    for (const RouteEnd* s : specs) {
        EndPoint e;
        if (!Resolve(index, *s, &e, &ans.error)) return ans;
        ends.push_back(e);
    }
    std::vector<std::pair<int, std::vector<Vec3>>> pieces;
    for (size_t i = 0; i + 1 < ends.size(); ++i) {
        if (!Leg(w, ends[i], ends[i + 1], &pieces, &ans.error)) return ans;
    }
    std::vector<Vec3> pts;
    std::vector<double> lim;
    std::vector<int> rail_ids;
    for (const auto& pc : pieces) {
        rail_ids.push_back(pc.first);
        const double sp = SpeedOf(w, w.rails[pc.first]);
        for (const Vec3& p : pc.second) {
            if (!pts.empty() && Dist(pts.back(), p) < 0.05) continue;
            pts.push_back(p);
            lim.push_back(sp);
        }
    }
    if (pts.size() < 2) {
        ans.error = "the route has no length";
        return ans;
    }
    std::vector<double> v;
    Profile(pts, lim, speed_cap > 0.0 ? speed_cap : kMaxSpeed, &v);
    for (size_t i = 0; i < pts.size(); ++i) ans.points.push_back({pts[i].x, pts[i].y, pts[i].z, v[i]});
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const double d = Dist(pts[i], pts[i + 1]);
        ans.length_m += d;
        ans.duration_s += d / std::max(0.5, 0.5 * (v[i] + v[i + 1]));
    }
    for (size_t i = 0; i < rail_ids.size(); ++i) {
        if (i == 0 || rail_ids[i - 1] != rail_ids[i]) ans.rails.push_back(rail_ids[i]);
    }
    for (int rid : ans.rails) {
        const std::string_view nm = NameOf(w, w.rails[rid]);
        if (!nm.empty() && (ans.names.empty() || ans.names.back() != nm)) ans.names.emplace_back(nm);
    }
    ans.start_rail = ends.front().rail;
    ans.start_dist = ends.front().dist;
    ans.end_rail = ends.back().rail;
    ans.end_dist = ends.back().dist;
    ans.ok = true;
    return ans;
}

// ================================================================================================================ the camera

CameraAxes AxesOf(const CameraPose& pose) {
    const double p = std::clamp(pose.pitch, -89.0, 89.0) * kDeg, y = pose.yaw * kDeg;
    const Vec3 f{std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)};
    // right = normalize(forward x up), up = right x forward (camera.cpp BasisFromYawPitch)
    Vec3 r{-f.z, 0.0, f.x};
    const double rl = std::sqrt(r.x * r.x + r.z * r.z);
    r = rl < 1e-9 ? Vec3{-1.0, 0.0, 0.0} : Vec3{r.x / rl, 0.0, r.z / rl};
    Vec3 u{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
    if (pose.roll != 0.0) {  // + lifts the right side: turn right and up about forward
        const double c = std::cos(pose.roll * kDeg), s = std::sin(pose.roll * kDeg);
        const Vec3 r2{r.x * c + u.x * s, r.y * c + u.y * s, r.z * c + u.z * s};
        const Vec3 u2{u.x * c - r.x * s, u.y * c - r.y * s, u.z * c - r.z * s};
        r = r2;
        u = u2;
    }
    return {r, u, f};
}

Projected ProjectSphere(const CameraPose& pose, Vec3 centre, double radius) {
    const CameraAxes ax = AxesOf(pose);
    const Vec3 d{centre.x - pose.pos.x, centre.y - pose.pos.y, centre.z - pose.pos.z};
    Projected out;
    out.dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    out.depth = d.x * ax.forward.x + d.y * ax.forward.y + d.z * ax.forward.z;
    out.in_front = out.depth > pose.near_plane;
    if (!out.in_front) return out;
    const double th = std::tan(pose.fov * kDeg / 2.0);
    const double rx = d.x * ax.right.x + d.y * ax.right.y + d.z * ax.right.z;
    const double uy = d.x * ax.up.x + d.y * ax.up.y + d.z * ax.up.z;
    const double nx = rx / out.depth / (th * pose.aspect), ny = uy / out.depth / th;
    out.u = (nx + 1.0) / 2.0;
    out.v = (1.0 - ny) / 2.0;
    out.size_frac = radius / (out.depth * th);
    out.in_frame = nx >= -1.0 && nx <= 1.0 && ny >= -1.0 && ny <= 1.0;
    return out;
}

void RayThrough(const CameraPose& pose, double u, double v, Vec3* origin, Vec3* dir) {
    const CameraAxes ax = AxesOf(pose);
    const double th = std::tan(pose.fov * kDeg / 2.0);
    const double sx = (2.0 * u - 1.0) * th * pose.aspect, sy = (1.0 - 2.0 * v) * th;
    Vec3 d{ax.forward.x + ax.right.x * sx + ax.up.x * sy, ax.forward.y + ax.right.y * sx + ax.up.y * sy, ax.forward.z + ax.right.z * sx + ax.up.z * sy};
    const double l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    *origin = pose.pos;
    *dir = {d.x / l, d.y / l, d.z / l};
}

// ================================================================================================================ files

namespace {

void AppendNum(std::string* out, double v, int decimals) {
    char buf[48];
    if (!std::isfinite(v)) {
        out->append("null");
        return;
    }
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    out->append(buf);
}

void AppendVec(std::string* out, const float* v, int decimals) {
    out->push_back('[');
    for (int i = 0; i < 3; ++i) {
        if (i) out->push_back(',');
        AppendNum(out, v[i], decimals);
    }
    out->push_back(']');
}

}  // namespace

void AppendJsonString(std::string_view text, std::string* out) {
    out->push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out->append(buf);
                } else if (c >= 0x80) {
                    // names are ASCII in practice; anything else becomes U+00xx so the file stays valid UTF-8
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out->append(buf);
                } else {
                    out->push_back(static_cast<char>(c));
                }
        }
    }
    out->push_back('"');
}

std::string ContentHash(const World& world) {
    uint64_t h = 1469598103934665603ull;
    auto feed = [&h](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    auto u32 = [&feed](uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        feed(b, 4);
    };
    auto u16 = [&feed](uint16_t v) {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        feed(b, 2);
    };
    feed("ATLAS1", 6);
    u32(static_cast<uint32_t>(world.lights.size()));
    u32(static_cast<uint32_t>(world.rails.size()));
    for (const Rail& r : world.rails) {
        u16(static_cast<uint16_t>(r.cv.size()));
        for (uint16_t c : r.cv) u16(c);
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf, 12);
}

std::string LightsJson(const World& world) {
    std::string s = "{\"lights\":[";
    for (size_t i = 0; i < world.lights.size(); ++i) {
        const Light& l = world.lights[i];
        if (i) s.push_back(',');
        s += "{\"i\":" + std::to_string(l.i) + ",\"pos\":";
        AppendVec(&s, l.pos, 3);
        s += ",\"color\":";
        AppendVec(&s, l.color, 4);
        s += ",\"dir\":";
        AppendVec(&s, l.dir, 4);
        s += ",\"intensity\":";
        AppendNum(&s, l.intensity, 3);
        s += ",\"cone\":";
        AppendNum(&s, l.cone, 2);
        s += ",\"spread\":";
        AppendNum(&s, l.spread, 2);
        s += l.spot ? ",\"type\":\"spot\"" : ",\"type\":\"omni\"";
        s += l.on ? ",\"on\":true}" : ",\"on\":false}";
    }
    s += "]}";
    return s;
}

std::string RailsJson(const World& world) {
    std::string s = "{\"nodes\":[";
    for (size_t i = 0; i < world.nodes.size(); ++i) {
        const Node& n = world.nodes[i];
        if (i) s.push_back(',');
        s += "{\"id\":" + std::to_string(n.id) + ",\"kind\":\"" + NodeKindName(n.kind) + "\"";
        if (n.kind == NodeKind::kRoad) {
            s += ",\"name\":";
            AppendJsonString(n.name, &s);
            s += ",\"speed\":";
            AppendNum(&s, n.speed, 3);
            s += ",\"cls\":" + std::to_string(n.cls < 0 ? 4294967295ll + n.cls + 1 : n.cls);
        }
        s += ",\"rails\":[";
        for (size_t k = 0; k < n.rails.size(); ++k) s += (k ? "," : "") + std::to_string(n.rails[k]);
        s += "]}";
    }
    s += "],\"rails\":[";
    for (size_t i = 0; i < world.rails.size(); ++i) {
        const Rail& r = world.rails[i];
        if (i) s.push_back(',');
        s += "{\"id\":" + std::to_string(r.id) + ",\"node\":" + std::to_string(r.node) + ",\"kind\":\"";
        s += r.node >= 0 ? NodeKindName(world.nodes[r.node].kind) : "?";
        s += "\",\"flags\":" + std::to_string(r.flags) + ",\"len\":";
        AppendNum(&s, r.len, 2);
        s += ",\"deg\":" + std::to_string(r.deg) + ",\"cv\":[";
        for (size_t k = 0; k < r.cv.size(); ++k) s += (k ? "," : "") + std::to_string(r.cv[k]);
        s += "],\"pts\":[";
        for (size_t k = 0; k < r.point_count(); ++k) {
            if (k) s.push_back(',');
            AppendVec(&s, &r.pts[3 * k], 2);
        }
        s += "],\"next\":[";
        for (size_t k = 0; k < r.next.size(); ++k) s += (k ? "," : "") + std::to_string(r.next[k]);
        s += "],\"prev\":[";
        for (size_t k = 0; k < r.prev.size(); ++k) s += (k ? "," : "") + std::to_string(r.prev[k]);
        s += "]";
        if (r.dir != 0) s += ",\"dir\":" + std::to_string(r.dir);
        s += "}";
    }
    s += "]}";
    return s;
}

std::string GaragesJson(const World& world) {
    std::string s = "{\"garages\":[";
    for (size_t i = 0; i < world.garages.size(); ++i) {
        const Garage& g = world.garages[i];
        if (i) s.push_back(',');
        s += "{\"name\":";
        AppendJsonString(g.name, &s);
        s += ",\"pos\":[";
        AppendNum(&s, g.pos.x, 3);
        s += ",";
        AppendNum(&s, g.pos.y, 3);
        s += ",";
        AppendNum(&s, g.pos.z, 3);
        s += "]}";
    }
    s += "]}";
    return s;
}

std::string CarsJson(const World& world) {
    std::string s = "{\"cars\":[";
    for (size_t i = 0; i < world.cars.size(); ++i) {
        const CarModel& c = world.cars[i];
        if (i) s.push_back(',');
        s += "{\"index\":" + std::to_string(c.index) + ",\"name\":";
        AppendJsonString(c.name, &s);
        s += ",\"class\":";
        AppendJsonString(c.cls, &s);
        s += "}";
    }
    s += "]}";
    return s;
}

std::string MetaJson(const World& world) {
    std::string s = "{\"version\":1,\"source\":";
    AppendJsonString(world.source, &s);
    s += ",\"hash\":\"" + ContentHash(world) + "\",\"counts\":{\"lights\":" + std::to_string(world.lights.size()) + ",\"rails\":" +
         std::to_string(world.rails.size()) + ",\"nodes\":" + std::to_string(world.nodes.size()) + ",\"garages\":" + std::to_string(world.garages.size()) +
         ",\"cars\":" + std::to_string(world.cars.size()) + "}";
    if (!world.error.empty()) {
        s += ",\"partial\":";
        AppendJsonString(world.error, &s);
    }
    s += "}";
    return s;
}

}  // namespace mc::cinematic::atlas
