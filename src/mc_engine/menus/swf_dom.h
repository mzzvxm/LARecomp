#pragma once
//
// Building display objects inside the game's own Flash movies (rage::swf), from
// C++. has the layouts and how each was proved.
//
// The .xsf files export no linkage names, so attachMovie finds nothing; what
// does work is what attachMovie does after its lookup: the sprite instance's
// PlaceObject (vtable +0x1C, sub_825FC5E0) with any character of the movie's
// own table. The new instance is drawn by the swf player like any other --
// matrix, color transform, the menu's fades -- because it is one.
//
// Placing and removing allocate from the movie's instance pool, which is only
// bound while the swf lock is held: every call that changes the display list
// goes inside a Bind, the same sequence the game's own C++ -> Flash helpers use.
// Guest thread only.

#include <cstdint>

namespace larecomp::swf {

// A UI movie registered with mcUIManager.
struct Movie {
    uint32_t entry = 0;    // mcUIManager movie entry
    uint32_t ctx = 0;      // swfCONTEXT (entry +56)
    uint32_t file = 0;     // swfFILE, the .xsf resource (ctx +28)
    uint32_t root = 0;     // root instance (ctx +32)
    bool enabled = false;  // entry +68
};

bool FindMovie(const uint8_t* base, const char* name, Movie& out);

// Direct child of `parent` whose instance name (+196) is `name`; 0 if none.
uint32_t FindChild(const uint8_t* base, uint32_t parent, const char* name);

// Direct child of `parent` at `depth`; 0 if none.
uint32_t ChildAtDepth(const uint8_t* base, uint32_t parent, uint16_t depth);

// Character `index` of the movie's table, 0 when the slot is not a character
// of `type` (1 shape, 2 sprite, ...).
uint32_t Character(const uint8_t* base, uint32_t file, int index, uint8_t type);

// Free instances left in the movie's pool. An empty pool is fatal to the game.
int PoolFree(const uint8_t* base, uint32_t ctx);

// Takes the swf lock and binds the movie's pools; releases both on scope exit.
// ok() is false (and nothing may be placed) when the movie is mid-update.
class Bind {
 public:
    Bind(uint8_t* base, uint32_t movie_ctx);
    ~Bind();
    Bind(const Bind&) = delete;
    Bind& operator=(const Bind&) = delete;
    bool ok() const { return bound_; }

 private:
    uint8_t* base_;
    uint32_t ctx_;
    bool bound_ = false;
};

// Inside a Bind. Places `character` as a child of the sprite instance `parent`
// at `depth` (replacing whatever character sat there) and returns the instance,
// drawn, with an identity transform.
uint32_t Place(uint8_t* base, uint32_t parent, uint16_t depth, uint32_t character);

// Inside a Bind. Removes the child at `depth` and returns it to the pool.
void Remove(uint8_t* base, uint32_t parent, uint16_t depth);

// Local transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
void SetMatrix(uint8_t* base, uint32_t inst, float a, float b, float c, float d, float tx,
               float ty);
void GetMatrix(const uint8_t* base, uint32_t inst, float m[6]);

// Local color transform that turns white art into `rgb` (0xRRGGBB) at `alpha`.
void SetSolidColor(uint8_t* base, uint32_t inst, uint32_t rgb, uint8_t alpha = 255);

// Copies `from`'s local color transform onto `to`.
void CopyColor(uint8_t* base, uint32_t to, uint32_t from);

// Keeps the instance drawn: a looping timeline clears +177 on every child.
void KeepDrawn(uint8_t* base, uint32_t inst);

// Shows or hides an instance without touching its color. +79 is the alpha the
// draw multiplies the instance and its children by (0 also skips its update,
// so a hidden sprite's timeline holds still).
void SetVisible(uint8_t* base, uint32_t inst, bool on);

// What a movie's script writes, for C++ that does what the script would: the
// ActionScript property setters (sub_825FCD58) and Color.setRGB (sub_825FB6A8).
// _alpha: +79 = 255 * percent / 100.
void SetAlphaPercent(uint8_t* base, uint32_t inst, int percent);
// _x / _y: offsets from the base its PlaceObject2 put it at (+116 / +120); the
// next draw rebuilds the matrix from them (+81, sub_825F8C70).
void SetX(uint8_t* base, uint32_t inst, float x);
void SetY(uint8_t* base, uint32_t inst, float y);
// Color.setRGB (0xRRGGBB): the tint +76..+78 the instance's children are drawn
// with (sub_825FB348 hands it to each child's color).
void SetTint(uint8_t* base, uint32_t inst, uint32_t rgb);
// The base a PlaceObject2 matrix gives an instance (sub_8260AAD8, the call the
// command makes): unscaled and unrotated, at (tx, ty). A guest call: only
// where one may be made from C++ (the UI tick, not inside a guest call).
void SetBaseTranslation(uint8_t* base, uint32_t inst, float tx, float ty);

// Instances whose colors must reach the screen as set (the ui_theme remap
// leaves them alone). Up to kMaxRaw at once.
constexpr int kMaxRaw = 48;
void SetRaw(const uint32_t* insts, int count);
// True while the swf player is drawing one of the SetRaw instances, on the
// calling thread.
bool DrawingRaw();

}  // namespace larecomp::swf
