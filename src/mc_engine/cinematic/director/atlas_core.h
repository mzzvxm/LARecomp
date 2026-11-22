#pragma once
//
// The Director's atlas, the pure half (package C, DIR-ATLAS; DIRECTOR-DESIGN.md 4.2, feature-parity/DIRECTOR-ATLAS-NOTES.md has the layouts and the checks).
//
// World knowledge for scenes, tools and agents: the city's lights and street lamps, the traffic rail network (roads, lanes, directions, how the lanes join),
// garages, the car table, ground height, a route along the roads. It reads game memory through a GuestView (so a unit test feeds it a byte buffer and the
// game feeds it guest memory), decodes it into plain structs, answers questions about them and writes them as the JSON files ports\atlas\<hash>\ holds
// (the same files ports\scripts\atlas.py builds offline from the player's own game files and reads: the shapes are one contract).
//
// No game, no SDK, no JSON library, no log: larecomp_atlas_test builds this alone. Everything here runs on the game thread when the game uses it, and on
// any thread when it does not touch a GuestView.
//
// Coordinates are the game's (x east, y up, z south), a heading is in degrees with 0 along +Z and 90 along +X (the scene's convention), speeds in m/s.
//

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mc::cinematic::atlas {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

// ---------------------------------------------------------------------------------------------------------------- game memory

// Big-endian reads of guest memory (the game is a PowerPC title: every word in its memory is big endian). A failed read (an address the view cannot
// give) returns 0 and sets failed(): a decoder checks it once at the end, and caps every count it reads, so garbage cannot make it run away.
class GuestView {
  public:
    virtual ~GuestView() = default;
    virtual bool Read(uint32_t addr, void* dst, uint32_t bytes) const = 0;

    uint32_t U32(uint32_t addr) const;
    uint16_t U16(uint32_t addr) const;
    uint8_t U8(uint32_t addr) const;
    float F32(uint32_t addr) const;
    std::string CStr(uint32_t addr, uint32_t limit = 96) const;  // up to the first NUL (or `limit` bytes)
    bool failed() const { return failed_; }
    void ClearFailed() const { failed_ = false; }

  private:
    mutable bool failed_ = false;
};

// A byte buffer standing for guest memory at `base`: a decompressed resource (base 0x50000000), or a test's. Not owned.
class BufferView final : public GuestView {
  public:
    BufferView(const uint8_t* data, size_t size, uint32_t base) : data_(data), size_(size), base_(base) {}
    bool Read(uint32_t addr, void* dst, uint32_t bytes) const override;

  private:
    const uint8_t* data_;
    size_t size_;
    uint32_t base_;
};

// ---------------------------------------------------------------------------------------------------------------- the data

struct Light {
    int i = 0;
    float pos[3] = {};
    float color[3] = {};
    float dir[3] = {};
    float intensity = 0.0f;
    float cone = 360.0f;   // degrees, 360 = omni
    float spread = 0.0f;
    bool spot = false;
    bool on = true;
    // Which of the game's two light lists it came from: false = the file's own list (lights.xlm, the 15,174 omni and spot lights the offline atlas decodes), true = the street-lamp list
    // (about 6,300 spot lamps the game builds at the city's load, one per prop that has lamp points, from sc_props.xck and tune/lighting/<model>.xml: the offline atlas decodes them the same
    // way, atlas_game.py; the lamp pass's, lit at night only). `on` is the light's own switch (every street lamp has it set: the lamp pass lights them between the evening and
    // morning hours), not whether it is lit at this hour.
    bool lamp = false;
};

enum class NodeKind : uint8_t { kRoad, kIntersection, kLeftTurn, kRightTurn, kUTurn, kTraffic };
const char* NodeKindName(NodeKind kind);

struct Node {
    int id = 0;
    NodeKind kind = NodeKind::kRoad;
    std::string name;     // roads: "SC_wa_S. Vermont Ave." (district code, street), freeways "FW_CY_105 FREEWAY"
    float speed = 0.0f;   // roads: the speed limit in m/s
    int32_t cls = 0;      // roads: a class code (3 streets, 5 freeways, -1 unset)
    std::vector<int> rails;
};

// One lane piece: a B-spline of `deg` through control vertices, sampled to a polyline. It runs from its first control vertex to its last, which is
// the direction of travel; the rails that END at a control vertex feed the rails that START there (next / prev).
struct Rail {
    int id = 0;
    int node = -1;      // index into World::nodes, -1 when the owner is not one the reader knows
    uint32_t flags = 0;
    float len = 0.0f;   // m, as the game stores it
    int deg = 3;
    std::vector<uint16_t> cv;     // control vertex indices
    std::vector<float> pts;       // x, y, z, x, y, z ... the sampled polyline
    std::vector<int> next, prev;
    int dir = 0;        // road rails: +1 / -1 = along / against the road's own direction vector, 0 = none

    size_t point_count() const { return pts.size() / 3; }
    Vec3 point(size_t i) const { return {pts[3 * i], pts[3 * i + 1], pts[3 * i + 2]}; }
};

struct Garage {
    std::string name;
    Vec3 pos;
};

struct CarModel {
    int index = 0;
    std::string name;
    std::string cls;   // player | dub | police | ambient | other (from the name's prefix)
};

struct World {
    std::string source;      // "live" | "file" | "test"
    std::vector<Light> lights;
    std::vector<Node> nodes;
    std::vector<Rail> rails;
    std::vector<Garage> garages;
    std::vector<CarModel> cars;
    std::string error;       // what stopped a decode (an empty string: all of it was read)
};

// ---------------------------------------------------------------------------------------------------------------- decoding

// The light list: {u32 ptr, u16 count, u16 capacity} at `list_addr`. The array behind it is either 96-byte lights back to back (the resource file's
// own layout, lights.xlm at root + 0x50) or an array of pointers to them (the run-time list the grid builder takes, entry 0 the null light): told
// apart by the first word. Light: +0 position, +16 colour, +32 direction, +64 intensity, +68 cone, +72 spread, +88 type (1 spot), +89 on.
bool DecodeLights(const GuestView& view, uint32_t list_addr, std::vector<Light>* out, std::string* error);

// The city's lamps from the multi-light manager: `*manager_global` (0x8287E2F4) is the manager, its +4 the light set (sub_822F9648 loads lights.xlm into it; sub_822F9268 makes an empty one),
// whose +96 list is the run-time one (sub_822F87E8 appends the packed lights to it and builds the grid from it) and whose +80 list is the file's own (the 15,174 the offline atlas decodes);
// the manager's +8 is the packed light file. Tries +96, then +80; `list_at` (when given) says which one answered. ONLY WHILE THE CITY LOADS: sub_822F87E8 deletes the set and zeroes +4 once the
// grid is built, so a running game answers "no light set" here: ReadRuntimeLights below is the reader of the running game. On failure `why` names what was not there with the raw words
// (the manager, +4, +8, each list's head), so that a run's log says which of them it was: the first real run (11-20) only had "not made yet" for six reads over 30 s of free roam.
bool ReadCityLights(const GuestView& view, uint32_t manager_global, std::vector<Light>* out, std::string* why, uint32_t* list_at = nullptr);

// The game's own unpacker for one packed light (sub_822F1B08): the 24-byte record at guest `record` becomes the 96-byte light, written to `out` in the guest's byte order. False when the
// unpacker could not run (no guest thread). The host's one is a guest call (handlers_world.cpp); a test gives a function of its own.
using LightUnpacker = std::function<bool(uint32_t record, uint8_t out[96])>;

struct RuntimeLightsInfo {
    uint32_t object = 0;       // the manager's +8 object (guest address)
    uint32_t fill = 0;         // lights read from its list at +8 (lights.xlm's own: the "file" lights)
    uint32_t lamps = 0;        // lights read from its list at +24 (the street lamps)
    uint32_t skipped = 0;      // records whose unpacked light did not look like one (kept out)
};

// The city's lights as the RUNNING game keeps them. sub_822F87E8 builds a light set from the packed file at load, builds the grid textures from it, deletes the set and zeroes the manager's +4 (so
// the first live read of 11-20 found "no light set" in every state of the game, day or night), and replaces the manager's +8 with a 56-byte object whose lists hold the same lights PACKED (24-byte
// records, sub_822FE0C8 repacks them; the lamp pass's AddLight unpacks them again each frame): the list at +8 {u32 array, u16 count at +12} is lights.xlm's own (the fill lights) and the list at
// +24 {array, count at +28} the street lamps. Both are read, in that order (the file's lights keep the offline atlas's indices), each record unpacked by `unpack`. Not time-of-day dependent:
// the lists exist from the city's load on, so the answer is the same by day and by night. The positions are the game's packed ones (16-bit steps of the grid's extent: about 0.1 m in x, 0.07 m
// in z; colours and intensities as half floats), so a lights.xlm light read this way is the offline atlas's to 0.15 m, not exactly. The pack also CLAMPS a position to the box of its steps (the first
// full run, 11-21: 295 of the 15,174 file lights differed, every one of them beyond the box: 294 above y 78.998, up to y 377 m, and one west of x -3467.0), so those lights are read where the game keeps
// them, at the box's edge, and atlas.py compare accepts that (lights_equal). 375 of the 6,298 street lamps have intensity 0 (no light, reach 0). On failure `why` says which words were not what the reader knows.
bool ReadRuntimeLights(const GuestView& view, uint32_t manager_global, const LightUnpacker& unpack, std::vector<Light>* out, std::string* why, RuntimeLightsInfo* info = nullptr);

// The traffic rail network: `root` is the mcRailnetData (the resource's first object; in the running game `*(railnet + 8)`). `step_m` is the spacing
// of the polyline samples, at most `max_pts` a rail.
bool DecodeRailnet(const GuestView& view, uint32_t root, World* world, std::string* error, double step_m = 4.0, int max_pts = 40);

// The car table at `table` (u32 record array at +0, u16 count at +4, 104-byte records, +0 the name pointer).
bool DecodeCars(const GuestView& view, uint32_t table, std::vector<CarModel>* out, std::string* error);

// "player" / "dub" / "police" / "ambient" / "other" from a vehicle name.
std::string CarClassOf(std::string_view name);

// The three garages' trigger points (constants of the career code, sub_826AB070; ATLAS-NOTES 1.4), as they read in the image.
std::vector<Garage> StaticGarages();
// The same three read from the image at their addresses (false when a read failed).
bool ReadGarages(const GuestView& view, std::vector<Garage>* out);

// A B-spline point (de Boor, non-rational). `knots` has cvs.size() + deg + 1 entries; t in [knots[deg], knots[cvs.size()]].
Vec3 EvalBSpline(const std::vector<Vec3>& cvs, const std::vector<float>& knots, int deg, double t);

// ---------------------------------------------------------------------------------------------------------------- queries

// A spatial index over a World's rail segments and lights (a grid of kCell metre cells). Build once per World; the World must outlive it and not change.
class Index {
  public:
    static constexpr double kCell = 64.0;
    explicit Index(const World& world);
    const World& world() const { return *world_; }
    // The rail segments whose cells touch the circle (x, z, r), each once, packed (rail << 8) | segment; a rail has at most 255 segments.
    void SegmentsNear(double x, double z, double r, std::vector<uint32_t>* out) const;
    // The lights whose cells touch the circle, as indices into World::lights.
    void LightsNear(double x, double z, double r, std::vector<int>* out) const;

  private:
    const World* world_;
    std::unordered_map<int64_t, std::vector<uint32_t>> seg_cells_;    // cell -> packed (rail, segment)
    std::unordered_map<int64_t, std::vector<uint32_t>> light_cells_;  // cell -> light index
};

struct Projection {
    int rail = -1;
    int seg = 0;
    double u = 0.0;       // 0..1 along that segment
    double dist = 0.0;    // metres: horizontal, or 3-D when a height was given
    Vec3 point;
    double heading = 0.0; // degrees, the travel direction of the segment
    double t = 0.0;       // 0..1 along the whole rail
};

// The nearest points of rails to (x, z) (and y when has_y), one per rail, closest first, within `r`. A heading (degrees) drops the rails that point more
// than `max_heading_err` away from it.
std::vector<Projection> ProjectOnRails(const Index& index, double x, double z, bool has_y, double y, double r, bool has_heading, double heading,
                                       double max_heading_err = 75.0);

struct LightHit {
    int light = 0;
    double dist = 0.0;
    double reach = 0.0;  // metres the lamp lights: sqrt(100 * intensity)
};
struct LightsAnswer {
    std::vector<LightHit> hits;  // closest first, at most n
    int in_radius = 0;           // all that matched
};
// Lights within r of (x, z) (3-D when has_y). kind: "" any, "spot" or "omni". src: "" any, "lamp" the street lamps (Light::lamp), "file" the file's own lights.
LightsAnswer NearLights(const Index& index, double x, double z, bool has_y, double y, double r, int n, double min_intensity, std::string_view kind, std::string_view src = {});

// What the road under a point is.
double SpeedOf(const World& world, const Rail& rail);              // m/s: the road's limit, or a turn's default
std::string_view NameOf(const World& world, const Rail& rail);     // the street, "" for a turn
double RailT(const Rail& rail, int seg, double u);

struct RoutePoint {
    double x, y, z, speed;
};
struct RouteEnd {
    // a position (x, z, optional y, optional heading), or a rail and a fraction of it
    bool by_rail = false;
    int rail = -1;
    double t = 0.0;
    double x = 0.0, y = 0.0, z = 0.0;
    bool has_y = false;
    bool has_heading = false;
    double heading = 0.0;
};
struct RouteAnswer {
    bool ok = false;
    std::string error;
    std::vector<RoutePoint> points;
    std::vector<int> rails;
    std::vector<std::string> names;
    double length_m = 0.0, duration_s = 0.0;
    int start_rail = -1, end_rail = -1;
    double start_dist = 0.0, end_dist = 0.0;
};
// A route along the lane graph: A* over the rails (cost = time at the road's limit), the pieces joined into one polyline, a speed profile (the road's
// limit, a curve limit, acceleration and braking). `speed_cap` m/s (0 = the roads' limits).
RouteAnswer PlanRoute(const Index& index, const RouteEnd& from, const std::vector<RouteEnd>& via, const RouteEnd& to, double speed_cap);

// The ground height from the roads: the height of the nearest road point within 25 m (roads follow the ground; a hill beside one does not). False when
// no road is that near.
bool GroundFromRails(const Index& index, double x, double z, double* y, double* dist);

// ---------------------------------------------------------------------------------------------------------------- the camera (query.visible, query.pick)

struct CameraPose {
    Vec3 pos;
    double yaw = 0.0, pitch = 0.0, roll = 0.0;  // degrees, the scene's meaning (yaw 0 looks along +Z, 90 along +X, pitch + up, roll + lifts the right side)
    double fov = 50.0;                           // vertical degrees
    double aspect = 16.0 / 9.0;
    double near_plane = 0.1;
};
struct CameraAxes {
    Vec3 right, up, forward;
};
CameraAxes AxesOf(const CameraPose& pose);

struct Projected {
    bool in_front = false;   // depth > near plane
    bool in_frame = false;   // the centre is inside the picture (and in front)
    double u = 0.0, v = 0.0; // 0..1, origin top left (may lie outside 0..1)
    double depth = 0.0;      // metres along the view direction
    double dist = 0.0;       // metres from the camera
    double size_frac = 0.0;  // the sphere's diameter as a fraction of the picture's height
};
Projected ProjectSphere(const CameraPose& pose, Vec3 centre, double radius);
// The ray through the point (u, v) of the picture (0..1, origin top left): origin = the camera, unit direction.
void RayThrough(const CameraPose& pose, double u, double v, Vec3* origin, Vec3* dir);

// ---------------------------------------------------------------------------------------------------------------- files

// Content hash of a world: 12 hex digits of FNV-1a (64 bit) over "ATLAS1", the light and rail counts (u32 LE) and every rail's control vertex indices
// (u16 count, u16 each, LE). ports\scripts\atlas.py computes the same, so one city has one folder name offline and live.
std::string ContentHash(const World& world);

// The JSON text of each file (compact, UTF-8). The shapes are atlas.py's loaders'.
std::string LightsJson(const World& world);
std::string RailsJson(const World& world);
std::string GaragesJson(const World& world);
std::string CarsJson(const World& world);
std::string MetaJson(const World& world);

// `text` as a JSON string literal appended to *out (quotes included).
void AppendJsonString(std::string_view text, std::string* out);

}  // namespace mc::cinematic::atlas
