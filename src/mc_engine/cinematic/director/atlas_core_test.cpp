// A check of the atlas's pure half (atlas_core.h) that needs no game:
//   cmake --build <build dir> --target larecomp_atlas_test, then run <build dir>/larecomp_atlas_test (exit 0 = passed).
//
// It builds small resources in memory with the real layouts (the rail network and the light list as ATLAS-NOTES describes them), decodes them through a
// BufferView, and asks the queries. With a decompressed game resource as an argument it decodes THAT instead and prints what it found (the files stay on
// the player's PC: nothing here is game data):
//   larecomp_atlas_test --rails <traffic.xrn decompressed> [--write-rails out.json]
//   larecomp_atlas_test --lights <lights.xlm decompressed> [--write-lights out.json]

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "atlas_core.h"

using namespace mc::cinematic::atlas;

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_fail;
}

constexpr uint32_t kP = 0x50000000u;  // a resource's virtual segment

struct Buf {
    std::vector<uint8_t> d;
    explicit Buf(size_t n) : d(n, 0) {}
    void W32(size_t o, uint32_t v) {
        d[o] = uint8_t(v >> 24);
        d[o + 1] = uint8_t(v >> 16);
        d[o + 2] = uint8_t(v >> 8);
        d[o + 3] = uint8_t(v);
    }
    void W16(size_t o, uint16_t v) {
        d[o] = uint8_t(v >> 8);
        d[o + 1] = uint8_t(v);
    }
    void WF(size_t o, float f) {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        W32(o, b);
    }
};

// The same tiny rail network ports\scripts\atlas_game.py synth_railnet builds: root, two rails (CV 0..3 and 3..6, a join at CV 3), one road.
std::vector<uint8_t> SynthRailnet() {
    Buf b(0x1000);
    b.W32(0x2C, kP + 0x300);
    b.W32(0x30, kP + 0x380);
    b.W32(0x34, kP + 0x3E0);
    b.W32(0x38, kP + 0x400);
    b.W32(0x40, 7);
    b.W32(0x44, 1);
    b.W32(80 + 0, kP + 0x150);
    b.W32(136 + 0, 112);
    b.W32(0xF8 + 0, 2);
    b.W32(80 + 4, kP + 0x240);
    b.W32(136 + 4, 176);
    b.W32(0xF8 + 4, 1);
    b.W32(0x240 + 144, kP + 0x4A0);
    b.WF(0x240 + 148, 20.0f);
    b.W32(0x240 + 152, 3);
    b.WF(0x240 + 96, 1.0f);
    b.WF(0x240 + 104, 0.0f);
    std::memcpy(&b.d[0x4A0], "SYN_Test_St.", 13);
    const float xs[7] = {0, 10, 20, 30, 40, 50, 60};
    for (int i = 0; i < 7; ++i) {
        b.WF(0x300 + 16 * i, xs[i]);
        b.WF(0x300 + 16 * i + 4, 0.0f);
        b.WF(0x300 + 16 * i + 8, 0.0f);
        b.W32(0x300 + 16 * i + 12, 0x7F800001u);
    }
    b.W32(0x380 + 4, kP + 0x3A0);
    b.W32(0x380 + 8, 8);
    const float kn[8] = {0, 0, 0, 0, 1, 1, 1, 1};
    for (int j = 0; j < 8; ++j) b.WF(0x3A0 + 4 * j, kn[j]);
    const uint16_t idx[8] = {0, 1, 2, 3, 3, 4, 5, 6};
    for (int j = 0; j < 8; ++j) b.W16(0x3E0 + 2 * j, idx[j]);
    for (int r = 0; r < 2; ++r) {
        const size_t o = 0x150 + 112 * r;
        b.W32(o + 4, 1);
        b.W32(o + 8, kP + 0x240);
        b.WF(o + 12, 30.0f);
        b.W16(o + 76, 3);
        b.W16(o + 78, 3);
        b.W32(o + 80, kP + 0x3E0 + 8 * r);
        b.W16(o + 84, 0);
    }
    return b.d;
}

// lights.xlm's own layout (96-byte lights back to back, root + 0x50 the list) or the run-time pointer array.
std::vector<uint8_t> SynthLights(bool pointers) {
    Buf b(0x600);
    const uint32_t first = 0x80;
    b.W32(0x50, kP + (pointers ? 0x400 : first));
    b.W16(0x54, pointers ? 3 : 2);
    for (int i = 0; i < 2; ++i) {
        const size_t o = first + 96 * i;
        b.WF(o, 1.0f + 3 * i);
        b.WF(o + 4, 2.0f + 3 * i);
        b.WF(o + 8, 3.0f + 3 * i);
        b.WF(o + 16, 1.0f);
        b.WF(o + 20, 0.5f);
        b.WF(o + 24, 0.25f);
        b.WF(o + 36, -1.0f);
        b.WF(o + 64, 20.0f);
        b.WF(o + 68, i == 0 ? 360.0f : 60.0f);
        b.d[o + 88] = uint8_t(i);
        b.d[o + 89] = 1;
    }
    if (pointers) {  // entry 0 = a null light elsewhere (0x8288B850 in the game), then the two real ones
        b.W32(0x400, 0x8288B850u);
        b.W32(0x404, kP + first);
        b.W32(0x408, kP + first + 96);
    }
    return b.d;
}

World SynthWorld() {
    const auto rn = SynthRailnet();
    BufferView v(rn.data(), rn.size(), kP);
    World w;
    std::string err;
    DecodeRailnet(v, kP, &w, &err);
    const auto lt = SynthLights(false);
    BufferView lv(lt.data(), lt.size(), kP);
    DecodeLights(lv, kP + 0x50, &w.lights, &err);
    w.garages = StaticGarages();
    w.source = "test";
    return w;
}

bool Near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) < eps; }

void TestDecode() {
    const auto rn = SynthRailnet();
    BufferView v(rn.data(), rn.size(), kP);
    World w;
    std::string err;
    Check(DecodeRailnet(v, kP, &w, &err), "decode: the synthetic rail network reads");
    Check(w.rails.size() == 2 && w.nodes.size() == 1, "decode: 2 rails, 1 node");
    Check(w.nodes[0].kind == NodeKind::kRoad && w.nodes[0].name == "SYN_Test_St." && Near(w.nodes[0].speed, 20.0) && w.nodes[0].cls == 3, "decode: the road's name, speed, class");
    Check(w.rails[0].next == std::vector<int>{1} && w.rails[1].prev == std::vector<int>{0} && w.rails[1].next.empty(), "decode: the join at CV 3 (rail 0 feeds rail 1)");
    Check(w.rails[0].cv.size() == 4 && w.rails[0].cv[3] == 3 && w.rails[1].cv[0] == 3, "decode: control vertex lists");
    const Vec3 a = w.rails[0].point(0), b = w.rails[0].point(w.rails[0].point_count() - 1);
    Check(Near(a.x, 0) && Near(b.x, 30), "decode: the polyline runs from the first CV to the last");
    Check(w.rails[0].dir == 1 && w.nodes[0].rails.size() == 2, "decode: side +1 along the road's direction vector, the node lists its rails");

    // a cut buffer is an error, not a crash
    BufferView cut(rn.data(), 0x350, kP);
    World w2;
    std::string err2;
    Check(!DecodeRailnet(cut, kP, &w2, &err2) && !err2.empty(), "decode: a cut buffer reports an error");
    // garbage counts are refused
    auto bad = rn;
    bad[0x40] = 0x7F;
    BufferView bv(bad.data(), bad.size(), kP);
    World w3;
    std::string err3;
    Check(!DecodeRailnet(bv, kP, &w3, &err3), "decode: a CV count of 2 billion is refused");

    for (bool pointers : {false, true}) {
        const auto lt = SynthLights(pointers);
        BufferView lv(lt.data(), lt.size(), kP);
        std::vector<Light> out;
        std::string e;
        Check(DecodeLights(lv, kP + 0x50, &out, &e), pointers ? "lights: pointer array reads" : "lights: contiguous array reads");
        Check(out.size() == 2 && out[1].spot && !out[0].spot && Near(out[0].pos[0], 1.0) && Near(out[1].pos[2], 6.0) && Near(out[0].intensity, 20.0) && out[0].on,
              pointers ? "lights: the null light is skipped" : "lights: two lights, omni then spot");
    }

    Buf cars(0x400);
    cars.W32(0x10, kP + 0x100);
    cars.W16(0x14, 3);
    cars.W32(0x100, kP + 0x200);
    cars.W32(0x100 + 104, kP + 0x220);
    cars.W32(0x100 + 208, kP + 0x240);
    std::memcpy(&cars.d[0x200], "vp_aud_r8_08", 13);
    std::memcpy(&cars.d[0x220], "vp_se_police_01", 16);
    std::memcpy(&cars.d[0x240], "va_semitruck_00", 16);
    BufferView cv(cars.d.data(), cars.d.size(), kP);
    std::vector<CarModel> cm;
    std::string e;
    Check(DecodeCars(cv, kP + 0x10, &cm, &e) && cm.size() == 3 && cm[0].cls == "player" && cm[1].cls == "police" && cm[2].cls == "ambient" && cm[0].name == "vp_aud_r8_08",
          "cars: the table's names and classes");
    Check(CarClassOf("vpd_x") == "dub" && CarClassOf("zz") == "other", "cars: dub and other");

    // the garages' constants at the addresses the career code reads them from
    Buf gimg(0xC0000);
    const uint32_t base = 0x82000000u;
    auto put = [&](uint32_t addr, float f) { gimg.WF(addr - base, f); };
    put(0x820C0000u - 23728u, 819.693f);
    put(0x820C0000u - 23720u, 17.731f);
    put(0x820C0000u - 23724u, -804.765f);
    put(0x820C0000u - 23732u, -3049.5f);
    put(0x82070000u - 12524u, -25.0f);
    put(0x82080000u + 26200u, 160.0f);
    put(0x820C0000u - 23744u, -53.6f);
    put(0x820C0000u - 23736u, -19.6f);
    put(0x820C0000u - 23740u, 1331.6f);
    BufferView giv(gimg.d.data(), gimg.d.size(), base);
    std::vector<Garage> g;
    Check(ReadGarages(giv, &g) && g.size() == 3 && Near(g[0].pos.x, 819.693, 0.01) && Near(g[1].pos.z, 160.0, 0.01) && Near(g[2].pos.y, -19.6, 0.01),
          "garages: the three constants read at their addresses");
    const auto sg = StaticGarages();
    Check(Near(sg[0].pos.x, g[0].pos.x, 0.01) && Near(sg[1].pos.y, g[1].pos.y, 0.01) && Near(sg[2].pos.z, g[2].pos.z, 0.01), "garages: the static list agrees");
}

// The multi-light manager the way the game lays it out (handlers_world.cpp): a global that holds the manager, its +4 the light set, +8 the packed file; the set's +96 list is the
// run-time one and its +80 list the file's own. `list96` and `list80` say which of the two lists hold the two lights (the other is {0, 0, 0}).
std::vector<uint8_t> SynthLightManager(bool manager, bool set, bool list96, bool list80) {
    Buf b(0x800);
    const uint32_t kMgr = kP + 0x100, kSet = kP + 0x200, kArr = kP + 0x400;
    if (manager) b.W32(0x10, kMgr);
    if (manager) b.W32(0x100 + 8, kP + 0x700);
    if (manager && set) b.W32(0x100 + 4, kSet);
    for (int i = 0; i < 2; ++i) {
        const size_t o = 0x400 + 96 * i;
        b.WF(o, 10.0f + i);
        b.WF(o + 4, 5.0f);
        b.WF(o + 8, -3.0f);
        b.WF(o + 16, 1.0f);
        b.WF(o + 20, 0.9f);
        b.WF(o + 24, 0.8f);
        b.WF(o + 36, -1.0f);
        b.WF(o + 64, 20.0f);
        b.WF(o + 68, 360.0f);
        b.d[o + 89] = 1;
    }
    if (list96) {
        b.W32(0x200 + 96, kArr);
        b.W16(0x200 + 96 + 4, 2);
        b.W16(0x200 + 96 + 6, 2);
    }
    if (list80) {
        b.W32(0x200 + 80, kArr);
        b.W16(0x200 + 80 + 4, 2);
        b.W16(0x200 + 80 + 6, 2);
    }
    return b.d;
}

void TestCityLights() {
    const uint32_t kGlobal = kP + 0x10;
    struct Case {
        const char* name;
        bool manager, set, l96, l80;
    };
    const Case cases[] = {{"the run-time list (+96)", true, true, true, false}, {"the file's own list (+80) when +96 is empty", true, true, false, true}, {"both lists", true, true, true, true}};
    for (const Case& c : cases) {
        const auto mem = SynthLightManager(c.manager, c.set, c.l96, c.l80);
        BufferView v(mem.data(), mem.size(), kP);
        std::vector<Light> out;
        std::string why;
        uint32_t at = 0;
        const bool ok = ReadCityLights(v, kGlobal, &out, &why, &at);
        const uint32_t want = c.l96 ? 96u : 80u;
        std::string what = std::string("city lights: ") + c.name;
        Check(ok && out.size() == 2 && at == want && why.empty() && Near(out[0].pos[0], 10.0) && Near(out[1].pos[0], 11.0), what.c_str());
    }
    {   // no manager: the pointer is 0
        const auto mem = SynthLightManager(false, false, false, false);
        BufferView v(mem.data(), mem.size(), kP);
        std::vector<Light> out;
        std::string why;
        Check(!ReadCityLights(v, kGlobal, &out, &why) && why.find("not made yet") != std::string::npos && why.find("is 0") != std::string::npos,
              "city lights: no manager says its pointer is 0");
    }
    {   // a manager with no light set: its +4 is 0, and the packed file's pointer is shown
        const auto mem = SynthLightManager(true, false, false, false);
        BufferView v(mem.data(), mem.size(), kP);
        std::vector<Light> out;
        std::string why;
        Check(!ReadCityLights(v, kGlobal, &out, &why) && why.find("no light set") != std::string::npos && why.find("0x50000700") != std::string::npos,
              "city lights: a manager with no light set says so, with the packed file's pointer");
    }
    {   // a set with two empty lists: both are named with their words
        const auto mem = SynthLightManager(true, true, false, false);
        BufferView v(mem.data(), mem.size(), kP);
        std::vector<Light> out;
        std::string why;
        uint32_t at = 7;
        Check(!ReadCityLights(v, kGlobal, &out, &why, &at) && at == 0 && why.find("+96 list") != std::string::npos && why.find("+80 list") != std::string::npos &&
                  why.find("count 0") != std::string::npos && why.find("0x50000200") != std::string::npos,
              "city lights: a set with no lights names both lists");
    }
    {   // the manager's address cannot be read
        const auto mem = SynthLightManager(true, true, true, false);
        BufferView v(mem.data(), 0x8, kP);
        std::vector<Light> out;
        std::string why;
        Check(!ReadCityLights(v, kGlobal, &out, &why) && why.find("cannot be read") != std::string::npos, "city lights: a pointer that cannot be read is not 'not made yet'");
    }
}

// The running game's lights (ReadRuntimeLights): the manager's +8 object holds two lists of PACKED 24-byte records (the file's own lights at +8, the street lamps at +24), and the
// game's unpacker turns a record into the 96-byte light. The fake unpacker here reads the record as {u32 id, u8 kind} and builds a light whose x is the id, so the test can follow
// every record through the reader.
struct FakeCity {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x1000, 0);
    uint32_t global = kP + 0x10, mgr = kP + 0x100, obj = kP + 0x300, fill = kP + 0x400, lamps = kP + 0x600;

    FakeCity(uint32_t n_fill, uint32_t n_lamps, bool with_manager = true, bool with_object = true) {
        Buf b(0x1000);
        if (with_manager) b.W32(0x10, mgr);
        if (with_manager && with_object) b.W32(0x100 + 8, obj);   // the manager's +4 (the load-time set) stays 0
        b.W32(0x300 + 8, n_fill ? fill : 0);
        b.W16(0x300 + 12, uint16_t(n_fill));
        b.W16(0x300 + 14, uint16_t(n_fill));
        b.W32(0x300 + 24, n_lamps ? lamps : 0);
        b.W16(0x300 + 28, uint16_t(n_lamps));
        b.W16(0x300 + 30, uint16_t(n_lamps));
        for (uint32_t i = 0; i < n_fill; ++i) {
            b.W32(0x400 + 24 * i, 100 + i);
            b.d[0x400 + 24 * i + 4] = 0;   // an omni fill light
        }
        for (uint32_t i = 0; i < n_lamps; ++i) {
            b.W32(0x600 + 24 * i, 500 + i);
            b.d[0x600 + 24 * i + 4] = 1;   // a spot lamp
        }
        mem = b.d;
    }
};

// What the game's unpacker does, in the test's terms: record id -> x = id, y 4, z -3, type = kind, intensity 60 (lamp) / 20, cone 125 / 360, on 1. Id 999 comes out as NaN (implausible).
LightUnpacker FakeUnpacker(const BufferView& v, int* calls = nullptr, bool works = true) {
    return [&v, calls, works](uint32_t rec, uint8_t out[96]) {
        if (calls) ++*calls;
        if (!works) return false;
        const uint32_t id = v.U32(rec);
        const uint8_t kind = v.U8(rec + 4);
        Buf b(96);
        b.WF(0, id == 999 ? std::nanf("") : float(id));
        b.WF(4, 4.0f);
        b.WF(8, -3.0f);
        b.WF(16, 1.0f);
        b.WF(20, 0.8f);
        b.WF(24, 0.5f);
        b.WF(36, -1.0f);
        b.WF(64, kind ? 60.0f : 20.0f);
        b.WF(68, kind ? 125.0f : 360.0f);
        b.d[88] = kind;
        b.d[89] = 1;
        std::memcpy(out, b.d.data(), 96);
        return true;
    };
}

void TestRuntimeLights() {
    {   // 3 file lights then 2 lamps: the file's keep the offline atlas's indices, the lamps follow
        FakeCity c(3, 2);
        BufferView v(c.mem.data(), c.mem.size(), kP);
        std::vector<Light> out;
        std::string why;
        RuntimeLightsInfo info;
        int calls = 0;
        const bool ok = ReadRuntimeLights(v, c.global, FakeUnpacker(v, &calls), &out, &why, &info);
        Check(ok && out.size() == 5 && why.empty() && calls == 5 && info.fill == 3 && info.lamps == 2 && info.skipped == 0 && info.object == c.obj,
              "runtime lights: 3 file lights and 2 lamps read, 5 unpacker calls");
        Check(ok && out[0].i == 0 && Near(out[0].pos[0], 100.0) && !out[0].lamp && !out[0].spot && out[2].i == 2 && Near(out[2].pos[0], 102.0) && !out[2].lamp,
              "runtime lights: the file's lights first, indices 0..2, omni");
        Check(ok && out[3].i == 3 && Near(out[3].pos[0], 500.0) && out[3].lamp && out[3].spot && Near(out[3].intensity, 60.0) && Near(out[3].cone, 125.0) && out[3].on &&
                  out[4].lamp && Near(out[4].pos[0], 501.0),
              "runtime lights: the lamps after them, flagged lamp, spot, on");
    }
    {   // the manager's +4 set is 0 (the state of the running game) and that does not matter
        FakeCity c(0, 4);
        BufferView v(c.mem.data(), c.mem.size(), kP);
        std::vector<Light> out;
        std::string why;
        RuntimeLightsInfo info;
        Check(v.U32(c.mgr + 4) == 0 && ReadRuntimeLights(v, c.global, FakeUnpacker(v), &out, &why, &info) && out.size() == 4 && info.fill == 0 && info.lamps == 4,
              "runtime lights: only the lamp list filled, +4 is 0");
        std::vector<Light> legacy;
        std::string lwhy;
        Check(!ReadCityLights(v, c.global, &legacy, &lwhy) && lwhy.find("no light set") != std::string::npos, "runtime lights: the load-time set reader still says 'no light set' on the same memory");
    }
    {   // a record that unpacks to garbage is left out and counted
        FakeCity c(2, 2);
        c.mem[0x400 + 24 * 1 + 3] = uint8_t(999 & 0xFF);   // fill light 1: id 999 = 0x03E7 -> bytes 03 E7
        c.mem[0x400 + 24 * 1 + 2] = uint8_t(999 >> 8);
        c.mem[0x400 + 24 * 1 + 0] = 0;
        c.mem[0x400 + 24 * 1 + 1] = 0;
        BufferView v(c.mem.data(), c.mem.size(), kP);
        std::vector<Light> out;
        std::string why;
        RuntimeLightsInfo info;
        Check(ReadRuntimeLights(v, c.global, FakeUnpacker(v), &out, &why, &info) && out.size() == 3 && info.skipped == 1 && info.fill == 1 && info.lamps == 2 && out[1].i == 1 && out[1].lamp,
              "runtime lights: an implausible unpacked record is skipped and counted, indices stay dense");
    }
    {   // the unpacker cannot run (no guest thread)
        FakeCity c(2, 2);
        BufferView v(c.mem.data(), c.mem.size(), kP);
        std::vector<Light> out;
        std::string why;
        Check(!ReadRuntimeLights(v, c.global, FakeUnpacker(v, nullptr, false), &out, &why) && out.empty() && why.find("unpacker could not run") != std::string::npos,
              "runtime lights: an unpacker that cannot run is an error, nothing half read is kept");
    }
    {   // no manager, no object, empty lists, absurd count, an array that ends outside the memory
        std::vector<Light> out;
        std::string why;
        FakeCity none(2, 2, false);
        BufferView v0(none.mem.data(), none.mem.size(), kP);
        Check(!ReadRuntimeLights(v0, none.global, FakeUnpacker(v0), &out, &why) && why.find("not made yet") != std::string::npos, "runtime lights: no manager is 'not made yet'");
        FakeCity noobj(2, 2, true, false);
        BufferView v1(noobj.mem.data(), noobj.mem.size(), kP);
        why.clear();
        Check(!ReadRuntimeLights(v1, noobj.global, FakeUnpacker(v1), &out, &why) && why.find("no light object") != std::string::npos && why.find("+8 is 0") != std::string::npos,
              "runtime lights: a manager with no +8 object says so");
        FakeCity empty(0, 0);
        BufferView v2(empty.mem.data(), empty.mem.size(), kP);
        why.clear();
        Check(!ReadRuntimeLights(v2, empty.global, FakeUnpacker(v2), &out, &why) && why.find("holds no lights") != std::string::npos && why.find("count 0") != std::string::npos &&
                  why.find("0x50000100") != std::string::npos,
              "runtime lights: two empty lists name both and the manager");
        FakeCity huge(2, 2);
        huge.mem[0x300 + 28] = 0xFF;   // 65,535 lamps: above the cap of 200,000? no: within, but the array cannot be read to its end
        huge.mem[0x300 + 29] = 0xFF;
        BufferView v3(huge.mem.data(), huge.mem.size(), kP);
        why.clear();
        Check(!ReadRuntimeLights(v3, huge.global, FakeUnpacker(v3), &out, &why) && why.find("cannot be read to its end") != std::string::npos,
              "runtime lights: a count whose records end outside the memory is refused, not read");
        FakeCity bad(2, 2);
        bad.mem[0x300 + 24] = 0;   // the lamp list's array pointer is 0 with a count of 2
        bad.mem[0x300 + 25] = 0;
        bad.mem[0x300 + 26] = 0;
        bad.mem[0x300 + 27] = 0;
        BufferView v4(bad.mem.data(), bad.mem.size(), kP);
        why.clear();
        Check(!ReadRuntimeLights(v4, bad.global, FakeUnpacker(v4), &out, &why) && why.find("not a light list") != std::string::npos, "runtime lights: a null array with a count is refused");
    }
    {   // the world's files: the lamps do not change the content hash (one city, one folder), carry src, and the src filter selects them
        FakeCity c(3, 2);
        BufferView v(c.mem.data(), c.mem.size(), kP);
        World withl = SynthWorld(), without = SynthWorld();
        std::string why;
        std::vector<Light> all;
        ReadRuntimeLights(v, c.global, FakeUnpacker(v), &all, &why);
        withl.lights = all;
        without.lights.clear();
        for (const Light& l : all)
            if (!l.lamp) without.lights.push_back(l);
        Check(!all.empty() && ContentHash(withl) == ContentHash(without), "lights files: the street lamps are not in the content hash");
        const std::string j = LightsJson(withl);
        Check(j.find("\"src\":\"lamp\"") != std::string::npos && j.find("\"on\":true,\"src\":\"lamp\"}") != std::string::npos, "lights files: a lamp carries src lamp");
        size_t n = 0;
        for (size_t at = j.find("\"src\""); at != std::string::npos; at = j.find("\"src\"", at + 1)) ++n;
        Check(n == 2, "lights files: only the lamps carry src (2)");
        Check(MetaJson(withl).find("\"lamps\":2") != std::string::npos, "lights files: meta counts the lamps");
        const Index index(withl);
        const LightsAnswer all_a = NearLights(index, 101.0, -3.0, false, 0.0, 600.0, 20, 0.0, "");
        const LightsAnswer lamp_a = NearLights(index, 101.0, -3.0, false, 0.0, 600.0, 20, 0.0, "", "lamp");
        const LightsAnswer file_a = NearLights(index, 101.0, -3.0, false, 0.0, 600.0, 20, 0.0, "", "file");
        Check(all_a.in_radius == 5 && lamp_a.in_radius == 2 && file_a.in_radius == 3, "lights query: src selects the lamps (2) or the file's lights (3) of 5");
    }
}

void TestQueries() {
    const World w = SynthWorld();
    const Index index(w);
    auto p = ProjectOnRails(index, 15.0, 3.0, false, 0.0, 40.0, false, 0.0);
    Check(!p.empty() && p[0].rail == 0 && Near(p[0].dist, 3.0) && Near(p[0].heading, 90.0, 0.5), "road: the nearest rail, 3 m away, heading 90 (+X)");
    Check(p[0].t > 0.4 && p[0].t < 0.6, "road: the fraction along the rail");
    Check(ProjectOnRails(index, 15.0, 0.0, false, 0.0, 40.0, true, 270.0).empty(), "road: a heading filter drops the opposite direction");
    Check(!ProjectOnRails(index, 15.0, 0.0, false, 0.0, 40.0, true, 80.0).empty(), "road: a heading near the lane's keeps it");
    Check(ProjectOnRails(index, 15.0, 30.0, false, 0.0, 10.0, false, 0.0).empty(), "road: nothing within 10 m at 30 m");
    Check(NameOf(w, w.rails[0]) == "SYN_Test_St." && Near(SpeedOf(w, w.rails[0]), 20.0), "road: name and speed of a rail");

    const LightsAnswer la = NearLights(index, 2.0, 3.0, true, 2.0, 20.0, 5, 0.0, "");
    Check(la.in_radius == 2 && la.hits.size() == 2 && Near(la.hits[0].dist, 1.0) && Near(la.hits[0].reach, 44.72, 0.05), "lights: two in 20 m, the closest 1 m, reach 44.7 m");
    Check(NearLights(index, 2.0, 3.0, true, 2.0, 20.0, 5, 0.0, "spot").in_radius == 1, "lights: kind spot");
    Check(NearLights(index, 2.0, 3.0, true, 2.0, 20.0, 1, 0.0, "").hits.size() == 1, "lights: n caps the list");
    Check(NearLights(index, 2.0, 3.0, true, 2.0, 20.0, 5, 50.0, "").in_radius == 0, "lights: min intensity");

    RouteEnd a, b;
    a.x = 2.0;
    a.z = 1.0;
    b.x = 58.0;
    b.z = -1.0;
    const RouteAnswer r = PlanRoute(index, a, {}, b, 0.0);
    Check(r.ok && r.rails == std::vector<int>{0, 1} && Near(r.length_m, 56.0, 1.0), "route: across the join, 56 m");
    bool capped = true;
    for (const RoutePoint& pt : r.points) capped = capped && pt.speed <= 20.0 + 1e-6;
    Check(capped && r.points.size() > 6 && Near(r.points.back().x, 58.0, 0.5), "route: the road's limit caps the speeds, the end is the goal");
    Check(r.names.size() == 1 && r.names[0] == "SYN_Test_St.", "route: street names");
    RouteEnd back_a = b, back_b = a;
    Check(!PlanRoute(index, back_a, {}, back_b, 0.0).ok, "route: no way backwards");
    RouteEnd by_rail;
    by_rail.by_rail = true;
    by_rail.rail = 1;
    by_rail.t = 1.0;
    Check(PlanRoute(index, a, {}, by_rail, 15.0).ok, "route: an end given as a rail and a fraction, with a speed cap");

    double y = 0, d = 0;
    Check(GroundFromRails(index, 12.0, 2.0, &y, &d) && Near(y, 0.0) && Near(d, 2.0), "ground: from the road height");
    Check(!GroundFromRails(index, 12.0, 90.0, &y, &d), "ground: nothing within 25 m");
}

void TestCamera() {
    CameraPose pose;
    pose.pos = {0, 2, 0};
    pose.yaw = 90.0;  // looks along +X
    pose.fov = 60.0;
    pose.aspect = 2.0;
    const CameraAxes ax = AxesOf(pose);
    Check(Near(ax.forward.x, 1.0) && Near(ax.right.z, 1.0) && Near(ax.up.y, 1.0), "camera: yaw 90 looks along +X, right is +Z (the scene's convention)");
    const Projected centre = ProjectSphere(pose, {10, 2, 0}, 1.0);
    Check(centre.in_frame && Near(centre.u, 0.5) && Near(centre.v, 0.5) && Near(centre.depth, 10.0), "camera: a point ahead is the picture's centre");
    const Projected right = ProjectSphere(pose, {10, 2, 5}, 1.0);
    Check(right.in_frame && right.u > 0.5, "camera: a point to the right (+Z) is right of centre");
    const Projected above = ProjectSphere(pose, {10, 6, 0}, 1.0);
    Check(above.in_frame && above.v < 0.5, "camera: a point above is above centre (v counts down)");
    Check(!ProjectSphere(pose, {-10, 2, 0}, 1.0).in_front, "camera: behind is not in front");
    Check(!ProjectSphere(pose, {10, 2, 100}, 1.0).in_frame, "camera: far to the side is out of frame");
    Check(Near(centre.size_frac, 1.0 / (10.0 * std::tan(30.0 * 3.14159265358979 / 180.0)), 1e-3), "camera: size fraction of a 1 m radius at 10 m");
    Vec3 o, dir;
    RayThrough(pose, 0.5, 0.5, &o, &dir);
    Check(Near(dir.x, 1.0) && Near(o.y, 2.0), "ray: through the centre is the forward axis");
    // a ray through a projected point comes back to that point
    RayThrough(pose, right.u, right.v, &o, &dir);
    const double t = 10.0 / dir.x;
    Check(Near(o.z + dir.z * t, 5.0, 0.01) && Near(o.y + dir.y * t, 2.0, 0.01), "ray: through the projection of a point hits the point");
    CameraPose rolled = pose;
    rolled.roll = 90.0;  // + lifts the right side (camera.cpp WithRoll): the world's up is then toward the picture's right
    const Projected r2 = ProjectSphere(rolled, {10, 6, 0}, 1.0);
    Check(r2.in_frame && r2.u > 0.5 && Near(r2.v, 0.5, 0.2), "camera: a 90 degree roll puts the world's up toward the right");
}

void TestFiles() {
    const World w = SynthWorld();
    const std::string rails = RailsJson(w);
    Check(rails.find("\"name\":\"SYN_Test_St.\"") != std::string::npos && rails.find("\"next\":[1]") != std::string::npos && rails.find("\"dir\":1") != std::string::npos,
          "files: rails.json holds the road, the join and the side");
    Check(rails.front() == '{' && rails.back() == '}', "files: rails.json is one object");
    const std::string lights = LightsJson(w);
    Check(lights.find("\"type\":\"spot\"") != std::string::npos && lights.find("\"intensity\":20.000") != std::string::npos, "files: lights.json");
    Check(GaragesJson(w).find("\"name\":\"hollywood\"") != std::string::npos, "files: garages.json");
    World w2 = w;
    w2.cars = {{0, "vp_a", "player"}};
    Check(CarsJson(w2).find("\"class\":\"player\"") != std::string::npos, "files: cars.json");
    Check(MetaJson(w).find("\"hash\":\"") != std::string::npos && MetaJson(w).find("\"rails\":2") != std::string::npos, "files: meta.json");
    // ports\scripts\atlas.py computes the same hash for the same world (atlas.py content_hash on the synthetic railnet + lights): one city, one folder name
    Check(ContentHash(w) == "53449146e329", "files: the content hash equals atlas.py's for the same world (53449146e329)");
    std::string s;
    AppendJsonString("a\"b\\c\n\x01", &s);
    Check(s == "\"a\\\"b\\\\c\\n\\u0001\"", "files: JSON string escapes");
}

std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int Summarise(const char* kind, const char* path, const char* write_to) {
    const auto data = ReadFile(path);
    if (data.empty()) {
        std::printf("cannot read %s\n", path);
        return 2;
    }
    BufferView v(data.data(), data.size(), kP);
    World w;
    std::string err;
    if (std::string(kind) == "rails") {
        if (!DecodeRailnet(v, kP, &w, &err)) {
            std::printf("decode failed: %s\n", err.c_str());
            return 1;
        }
        size_t pts = 0, links = 0;
        double worst = 0.0;
        for (const Rail& r : w.rails) {
            pts += r.point_count();
            links += r.next.size();
            double l = 0;
            for (size_t i = 0; i + 1 < r.point_count(); ++i) {
                const Vec3 a = r.point(i), b = r.point(i + 1);
                l += std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
            }
            if (r.len > 0) worst = std::max(worst, std::fabs(l - r.len) / r.len);
        }
        std::printf("rails %zu, nodes %zu, polyline points %zu, links %zu, worst length error %.3f, hash %s\n", w.rails.size(), w.nodes.size(), pts, links, worst,
                    ContentHash(w).c_str());
        const Index index(w);
        RouteEnd a, b;
        a.x = 819.6;
        a.z = -808.4;
        b.x = -53.6;
        b.z = 1331.6;
        const RouteAnswer r = PlanRoute(index, a, {}, b, 0.0);
        std::printf("route lot -> SC garage: %s, %.0f m, %.0f s, %zu rails, %zu points\n", r.ok ? "ok" : r.error.c_str(), r.length_m, r.duration_s, r.rails.size(), r.points.size());
        if (write_to) {
            std::ofstream o(write_to, std::ios::binary);
            const std::string j = RailsJson(w);
            o.write(j.data(), static_cast<std::streamsize>(j.size()));
        }
        return 0;
    }
    if (!DecodeLights(v, kP + 0x50, &w.lights, &err)) {
        std::printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    size_t spots = 0;
    for (const Light& l : w.lights) spots += l.spot ? 1 : 0;
    std::printf("lights %zu (%zu spots)\n", w.lights.size(), spots);
    if (write_to) {
        std::ofstream o(write_to, std::ios::binary);
        const std::string j = LightsJson(w);
        o.write(j.data(), static_cast<std::streamsize>(j.size()));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && (std::strcmp(argv[1], "--rails") == 0 || std::strcmp(argv[1], "--lights") == 0)) {
        const char* write_to = (argc >= 5 && (std::strncmp(argv[3], "--write", 7) == 0)) ? argv[4] : nullptr;
        return Summarise(argv[1] + 2, argv[2], write_to);
    }
    TestDecode();
    TestCityLights();
    TestRuntimeLights();
    TestQueries();
    TestCamera();
    TestFiles();
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}
