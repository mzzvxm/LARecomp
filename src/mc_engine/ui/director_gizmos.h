#pragma once
//
// The Director panel's world gizmos: the pure half (DIRECTOR-DESIGN.md section 3, "Paths" and CHECK 2). The panel draws the routes, the camera's path, the key
// frusta, the cars' labels and the waypoint handles over the picture with ImGui's background draw list; this file is the maths under that: where a world point
// lands in the WINDOW, a world segment cut at the camera's near plane, the ray through a window pixel, the colours, and the lists of lines and dots a drawer
// only has to paint. No ImGui, no game, no log: director_panel_test.cpp builds it on its own.
//
// THE PROJECTION MATCHES THE RENDER (CHECK 2 of the design, settled by a run: R-E1 draws a marker at a known car): the picture is not the window. The picture's
// rectangle is the window's largest rectangle of the camera's aspect, centred (display/frame_fit.h FrameOf: ultrawide, 4:3 or a window with bars all follow it),
// and in a TALL picture (narrower than 16:9) the renderer rewrites the camera's vertical field of view (ultrawide.cpp CameraFov = VertPlusFov: the horizontal angle
// of the game's 16:9 picture stays), while the slot the Director reads still holds the game's own. EffectiveFov below applies the same rule, so a gizmo and the car it marks
// agree in every window shape. A wide picture keeps the vertical angle (Hor+: the sides show more).
//

#include <cstdint>
#include <string>
#include <vector>

#include "mc_engine/cinematic/director/director.h"

namespace mcla::ui::director {

namespace dir = mc::cinematic::director;

struct Vec2f {
    float x = 0.0f, y = 0.0f;
};
struct RectF {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool Contains(Vec2f p) const { return p.x >= x && p.y >= y && p.x <= x + w && p.y <= y + h; }
    bool Empty() const { return !(w > 0.0f) || !(h > 0.0f); }
};

// ImGui's packing (IM_COL32: alpha in the top byte, then blue, green, red), so a drawer passes it on as it is.
constexpr uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}
uint32_t WithAlpha(uint32_t color, uint32_t alpha);

// The largest rectangle of `aspect` (width over height) in a window of window_w x window_h, centred, without the whole-pixel rounding of FrameOf (the window of the UI
// is in logical units): the whole window when it is that shape. Empty for a window with no size.
RectF PictureRect(float window_w, float window_h, double aspect);
// The vertical field of view the renderer builds the picture with: the slot's, widened in a tall picture (frame_fit.h VertPlusFov).
double EffectiveFov(double slot_fov_degrees, double aspect);

struct Ray {
    double origin[3] = {0, 0, 0};
    double dir[3] = {0, 0, 1};  // unit length
};

// The camera of the snapshot, ready to project into a window.
struct Projector {
    bool valid = false;
    RectF picture;  // where the picture lies in the window
    double pos[3] = {0, 0, 0};
    double right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1};
    double tan_half_fov = 0.5;  // of the effective vertical angle
    double aspect = 16.0 / 9.0;
    double near_plane = 0.1;

    // `window_w` x `window_h`: ImGui's DisplaySize. Invalid (valid false) with no camera in the snapshot or an empty window.
    static Projector Make(const dir::SnapCamera& camera, float window_w, float window_h);

    // A world point in the window; false behind the camera (nothing is written). `depth` (may be null): the distance along the camera's forward.
    bool Project(const double p[3], Vec2f* out, double* depth = nullptr) const;
    // The same for a point in the camera's own frame (x right, y up, z forward): what a frustum drawn at a key needs.
    bool ProjectCameraSpace(double x, double y, double z, Vec2f* out) const;
    // The segment a-b cut at the near plane and projected: false when none of it is in front.
    bool ProjectSegment(const double a[3], const double b[3], Vec2f* pa, Vec2f* pb) const;
    // The ray through a window pixel (a pixel outside the picture is a ray outside the view: still a ray).
    Ray PixelRay(Vec2f px) const;
    // A window pixel as the picture's 0..1 coordinates (origin top left), for query.pick; false when the pixel is outside the picture (u, v are still written).
    bool ToUV(Vec2f px, double* u, double* v) const;
};

// The point where the ray meets the horizontal plane y = `plane_y`; false when the ray is parallel to it or the plane is behind.
bool RayPlaneY(const Ray& ray, double plane_y, double out[3]);

// ---------------------------------------------------------------------------------------------------------------- what the drawer paints

struct Line2 {
    Vec2f a, b;
    uint32_t color = 0;
    float width = 1.5f;
};
struct Dot2 {
    Vec2f p;
    float radius = 4.0f;
    uint32_t color = 0;
    bool filled = true;
    int id = -1;  // what the dot stands for (a waypoint's index, a car's number): the hit test reports it
};
struct Label2 {
    Vec2f p;  // the anchor; the drawer puts the text beside it
    std::string text;
    uint32_t color = 0;
};
struct Gizmos {
    std::vector<Line2> lines;
    std::vector<Dot2> dots;
    std::vector<Label2> labels;
    void Clear() {
        lines.clear();
        dots.clear();
        labels.clear();
    }
};

// A speed (metres a second) as a colour: blue when slow, teal, yellow, red at 40 m/s and more.
uint32_t SpeedColor(double speed_mps);

// A route (points [x, y, z, speed]) as a polyline coloured by speed, with an arrowhead every `arrow_px` along each segment that is that long. `id_base` -1: no dots at the
// points; else every point is a dot with id id_base + its index (the waypoint handles). Label at the first point: `label` when not empty.
void AddRoute(const Projector& view, const std::vector<std::vector<double>>& points, int id_base, const std::string& label, bool highlighted, Gizmos* out, float arrow_px = 140.0f);
// A closed-form camera path: the samples as one line (a segment that is a cut breaks it).
void AddCameraPath(const Projector& view, const std::vector<dir::SnapCamSample>& samples, uint32_t color, Gizmos* out);
// A small pyramid in front of a camera pose: where a key looks, `length` metres deep; `selected` makes it bold.
void AddFrustum(const Projector& view, const dir::SnapPose& key, double aspect, double length, uint32_t color, bool selected, Gizmos* out);
// A car's mark: a ring on its place and its label (id and model, shortened). `id` (0 or more) is what NearestDot reports when the pointer is on the ring (the car's index in the
// snapshot): the panel picks a car with it; -1 makes the ring no handle.
void AddCar(const Projector& view, const dir::SnapCar& car, bool selected, int id, Gizmos* out);

// The dot nearest to a window pixel within `radius` pixels: its index in `dots` or -1. Dots with id < 0 are skipped.
int NearestDot(const std::vector<Dot2>& dots, Vec2f px, float radius);

// A point in the camera's path: the spline the scene's `pos` keys make is drawn from these samples (the Director fills the snapshot's cam_path); this is the same maths
// for the keys the panel can read from the text of the scene (pos x y z), used when the snapshot has none: the points of the keys joined by the scene's own spline.
std::vector<dir::SnapCamSample> SplineThrough(const std::vector<dir::SnapCamSample>& keys, int per_segment = 12);

}  // namespace mcla::ui::director
