#pragma once
//
// Ultrawide: the shape of the picture the game is shown in.
//
// The game draws a 1280x720 screen. For a wider window the cameras are built with the window's
// aspect ratio (the guest's own camera code, through the hooks in hooks/graphics.cpp: the vertical
// field of view stays the 16:9 one, so the view widens, "Hor+"), the 3D picture is drawn into the
// same 1280x720 image squeezed sideways, and the present stretches that image over the whole window,
// which undoes the squeeze. What the game draws on top of the picture after the scene is composed
// (the HUD and every Flash menu) would be stretched with it, so the renderer draws those squeezed by
// the same factor around the centre of the screen: they come out 16:9-correct, in the middle
// (gpu/scene.cpp, UiSqueeze). A frame with no 3D scene in it (a movie, a loading screen, a menu on
// a flat background) is not squeezed or stretched at all: the present shows it 16:9 in the middle
// of the window with black on both sides, as it always did (gpu/present.h, display aspect). The
// setting `hud_width` moves the corner pieces of the driving HUD out of that middle block toward the
// screen edges (the last section of this file).
//
// CHECK: that the four camera hooks and the constant at 0x8201E7EC are every place the game takes its
// 3D aspect ratio from (the rear-view mirror, garage, showroom, map and photo cameras especially); a
// camera that keeps 16:9 shows its picture stretched.
//
// The aspect ratio is the window's: it follows the window as it is resized, goes fullscreen or
// changes monitor, with nothing to set. It is never below 16:9 (a taller window keeps 16:9 and the
// black bars above and below) and never above 32:9 (a wider window has black bars at the sides).
// The setting `aspect_ratio` (hooks/graphics.cpp) is "auto" unless the player picks one.
//
// Threading: every function can be called from any thread.
//

#include <cstdint>

namespace mc::ultrawide {

// The game's own picture and the widest one built (a 32:9 monitor, 5120x1440 or 3840x1080).
inline constexpr double kBaseAspect = 16.0 / 9.0;
inline constexpr double kMaxAspect = 32.0 / 9.0;

// The window's size in pixels: from the app on every size change (UI thread), once the window
// exists. 0 in either is ignored (a minimised window keeps the last size).
void SetWindowSize(uint32_t width, uint32_t height);

// The aspect ratio the cameras are built with and the picture is shown at, between kBaseAspect and
// kMaxAspect; kBaseAspect when there is nothing to widen.
double DisplayAspect();

// DisplayAspect() is wider than 16:9.
inline bool Active() { return DisplayAspect() > kBaseAspect * 1.001; }

// What fraction of the screen's width the UI keeps (16:9 over the aspect ratio): 1 when Active() is
// false, 3/4 at 21:9 (2560x1080 is 0.75; 3440x1440 is 0.744), 1/2 at 32:9.
double UiWidthFraction();

// From InitHooks, once, before any guest code: reads the setting, follows its changes while the
// game runs and writes the aspect ratio into the constant the game's cameras load it from (guest
// memory). The camera hooks read DisplayAspect() themselves.
void Init();

// --- markers the game places on the picture itself ----------------------------------------------
//
// Two kinds of marker are placed from the game's own 3D view (the widened camera):
//
// 1. A Flash clip the game moves to a projected position (the floating gamertag above a car,
//    sub_82289530): its x is a 1280-wide stage coordinate taken from the widened camera's NDC,
//    x = 640 + 640 * ndc_x. The UI pass draws the stage squeezed around the middle by
//    k = UiWidthFraction(), so x ends up at 640 + (x - 640) * k in the image and the present's
//    stretch puts the clip nearer the middle than the car. The hook gives the game
//    x' = 640 + (x - 640) / k, which the squeeze brings back to x. (MarkerStageX.)
//
// 2. A world icon the game draws as a 3D movie with the camera's own view and projection while it
//    draws the UI (the queued icon list, sub_82236558: racer, checkpoint and place icons): its
//    position is already the widened camera's, so the renderer must not squeeze those draws (and
//    their depth test against the scene's depth needs the unmoved position). The two hooks around
//    the list's draw tell the renderer which draws they are (NoteWorldIcons, gpu/scene.h OnUiMessage).
//
// CHECK: both are from reading the guest code only; see ULTRAWIDE-TEST.md items 6 and 8.

// Half the width of the stage the game's Flash movies are laid out on (the game's screen is 1280).
inline constexpr float kStageHalfWidth = 640.0f;

// A Flash stage x the game worked out from the widened camera, as the game must be given it so
// that the squeezed UI draws the clip where the 3D point is (the same x when the picture is 16:9).
float MarkerStageX(float stage_x);

// The values of a composite record's bucket that mean "the draws that follow, until the end value,
// are world icons" to the renderer (Renderer::kComposite): a side channel on the guest-to-render
// thread record stream that needs no new record type. Real buckets are single bits (0x10000000).
inline constexpr uint32_t kUiWorldBegin = 0x7F000001u;
inline constexpr uint32_t kUiWorldEnd = 0x7F000002u;

// Around the guest's draw of the queued world icons (guest render thread). A no-op unless the
// aspect ratio is wider than 16:9 and the Plume renderer is on.
void NoteWorldIcons(bool begin);

// --- the nav map (the pause menu's full map) ---------------------------------------------------------
//
// The map is not a second camera to widen. Read from the guest code: the map screen (sub_82625AC0,
// sub_82622FF0) flies the UI camera to the pose at screen+1056, the scene camera update (sub_822C0320,
// through sub_8220AA68) takes the UI camera's pose and vertical field of view and sub_822E5E10 builds
// the projection with the aspect ratio the camera hooks already widen. So the 3D picture of the map is
// as wide as every other one, and what the game draws over it in Flash is squeezed into the middle
// with the rest of the UI.
//
// The map keeps a pair of constants of its own (0x8201FAC8 = 1.7778, taken when the console is in
// widescreen, and 0x8201FACC = 1.33333) that three functions read:
//
// - sub_82627738 (the placement of an icon: a local viewport with the map camera's 25 degree vertical
//   field of view and the 16:9 aspect ratio projects the icon's world position, and the result is
//   the stage x and y the icon's Flash movie is given). Left as it is on purpose: with the 3D
//   picture drawn at aspect ratio A the 16:9 projection gives the stage x = 640 + 640 * ndc_A / k
//   (k = 16:9 over A), which the UI squeeze maps to 640 + 640 * ndc_A, the point's own place on
//   the wide picture. Widening this constant too would need MarkerStageX and give the same place.
// - sub_8221BE98 (mcWarper::Update): turns the horizontal field of view of the camera flight's end pose
//   (radians) into the vertical one in degrees by dividing by this aspect ratio. The vertical field of
//   view is what stays the game's 16:9 one, so this stays 16:9 as well.
// - sub_826206B0: the ground the map camera sees, as a box in world x and z (the cursor's box and the
//   map's edges at screen+400, +416 and +572..+584, written by sub_82623BF8). This one has to follow
//   the picture: with 16:9 in it the cursor could not leave the middle 16:9 of a wide picture and the
//   icons outside it would not be placed. MapVisibleAspect().
//
// CHECK: all of it is read from the guest code, nothing was run: that the map camera's vertical field
// of view is the 25 degrees sub_82627738 projects with, and the places in ULTRAWIDE-TEST.md item 4.

// The aspect ratio to give sub_826206B0 where it loads 0x8201FAC8; the constant's own value while the
// picture is 16:9. The game works the half width of the ground it sees as tan(aspect * the half
// vertical field of view in radians) times the distance, which is 3.6 % wider than the exact
// aspect * tan(half field of view) at 16:9 and drifts away from it as the aspect ratio grows (25 % at
// 32:9). The value returned keeps the box as wide, against the picture, as the game has it at 16:9.
double MapVisibleAspect();

// --- cutscene cameras ------------------------------------------------------------------------------
//
// The engine's own cutscenes (not movies) are taken by sub_822C0320 from the cutscene director's camera,
// whose angle is a horizontal one in radians that the game turns into the vertical angle by dividing
// by the viewport's aspect ratio (0x822C04E8, `fdivs f12,f28,f1`). The viewport's aspect ratio is the
// widened one, so the vertical angle would shrink and the picture would zoom into the middle. The hook
// Patch_CutsceneFovAspect_822C04E8 (ultrawide.cpp) gives the division kBaseAspect while the picture is
// wider than 16:9: the vertical angle is the 16:9 one and the sides show more, as in gameplay. The
// gameplay cameras do not go through that division.
// CHECK: from the generated code only; ULTRAWIDE-TEST.md, item 13.

// --- the HUD's corner pieces (the setting hud_width) --------------------------------------------------
//
// The squeeze puts the whole HUD in the middle 16:9 block. With `hud_width` above 0 the corner pieces of
// the driving HUD move out toward the screen edges instead, each as one unit. Read from the game's own
// data (the two HUD movies, resources/ui/hud/hud.xsf and hudmap/hudmap.xsf, and the ActionScript of the
// first) and from the guest code; nothing was run.
//
// - The driving HUD is two Flash movies on a 1280x720 stage: hud.swf (the HUD manager's gauge movie, drawn
//   by sub_8223D780 through sub_82721258, not named in the UI manager's map) and hudmap.swf (HUDMAPMOVIE).
//   Each has one clip named "HUD" under its root (hud.swf's root also holds `gamertags`, the floating
//   names: not touched here), and the pieces are the children of that clip.
// - hud.swf: the game already lays its pieces out for a 16:9 screen: the script of the first frame
//   (hud.xsf, the action block at 0xB982, `_global.aspect` not 0) moves them out with `_x` offsets, +135
//   for info, glows, speedometer, posmeter, playerList, delivery and CTF_score, -135 for flagTime,
//   flagsDelivered, bomb, flag, lapmeter, laptime and totaltime. Those two lists are the pieces and their
//   sides. `chat` (the text at the top left) has no offset in the script and is anchored left with the
//   others. Left in the middle: screenSafe, camera_indicator, Splash0, Splash1, Splash1_wide,
//   Splash1_overlap, Splash2 (the messages of SplashManager_Splash*, the rep and cash gains among them
//   (mcRewardManager_BuildRewardMoney): wide text boxes, and a background strip that is symmetric about
//   the middle of the stage) and countdownOffline: centred, or spanning the middle.
// - hudmap.swf: the minimap is the clips miniBG, ring, ringNorth and renderMe (a bitmap of the minimap's
//   render target that the script places with `viewportWindow`: it has to move with the other three), on the
//   left. The same movie holds the expanded map (mapBorder, title_bar and renderMe again), which is
//   centred: the script makes mapBorder transparent and ring, ringNorth and miniBG as opaque as the HUD in
//   the minimap's state, and the reverse in the expanded map's. So the four are anchored only while ring
//   is not transparent (its alpha byte, +79).
// - A piece is identified by its instance name and its place in the clip tree, read from guest memory when
//   the game's Flash player draws it: sub_825FB348 draws one instance and, recursively, its children;
//   the instance has its parent at +184, its name at +196. A child of a clip named "HUD" whose name is in the
//   lists is a piece (only the two HUD movies, and the developer HUD, have a clip of that name).
//
// The mechanism is the world icons' (above): the guest side only names the draws and the renderer moves
// them. Two hooks, at the entry of sub_825FB348 and at its one exit, send the renderer which piece the draws
// that follow belong to (kUiPieceLeft or kUiPieceRight, then kUiPieceEnd) through the composite record's
// bucket, and the renderer adds the shift to the squeeze of each of those draws (gpu/scene.cpp,
// Scene::UiShiftOf). All of a piece is drawn between the two hooks (the instance, its children, the
// masks and the text), so it moves as one unit: the geometry, the scissor and the depth-only mask draws
// are all moved by the same formula. Chosen over moving the piece's stage x in the guest (as
// Patch_UiMarkerX does the gamertag) because the renderer squeezes the scissor too and a guest-side move
// would need the renderer's help with that anyway, the amount depends on k and hud_width, which the
// renderer has at hand, and the guest would have to put every piece back after its draw; and over a
// position rule in the renderer (the draws left or right of the middle) because that acts on single draws
// and cannot tell a left piece from a message that spans the middle.
//
// The shift is, in the image the guest draws (1280 wide), t * 640 * (1 - k) pixels for t = hud_width / 100
// and k = UiWidthFraction(): to the left for a left piece (its left edge ends up t of the way to the screen
// edge, and at t = 1 as far from it as on a 16:9 screen), to the right for a right piece. In stage units
// that is t * 640 * (1 - k) / k, as a guest-side move would need.
//
// CHECK: all of this is read from the data and the code: the piece names (the clips' own names are in the
// movies, the sides are the game's own script's), the +184 / +196 offsets (sub_825FC5E0 stores the parent
// when it places a child, sub_8260B250 the name from the command) and the exit at 0x825FB698 (every
// path of sub_825FB348 ends there), that the minimap's bitmap is renderMe, and that no piece of
// either movie is drawn outside sub_825FB348 (a draw by another path would stay in the middle and tear), that
// `chat` is a left piece (a text box at the top left that the game's script does not offset), that the rep and
// cash pop-ups are the Splash clips (messages, centred: the natives are SplashManager_Splash*, not which clip
// shows which) and not a piece of the list, and that the renderer
// prepares each draw in record order on the render thread, as the world icons' bracket needs too.
// Not moved: the GPS arrow in the HUD (a 3D arrow in a viewport of its own, not a clip's draw).
// The log lists the pieces found (ULTRAWIDE-TEST.md, item 14).

// hud_width / 100 while the aspect ratio is wider than 16:9 (Active()); 0 otherwise.
double HudWidthFraction();

// More messages on the composite record's side channel (see kUiWorldBegin): the draws that follow, until
// kUiPieceEnd, are the whole of one HUD piece, which is anchored to the left or the right edge.
inline constexpr uint32_t kUiPieceLeft = 0x7F000003u;
inline constexpr uint32_t kUiPieceRight = 0x7F000004u;
inline constexpr uint32_t kUiPieceEnd = 0x7F000005u;
// The same for a clip that always sits at its edge on a wide picture, whatever hud_width is (the title
// screen's prompt bar, "PRESS START"): moved as a piece is at hud_width 100. Closed by kUiPieceEnd.
inline constexpr uint32_t kUiEdgeLeft = 0x7F000006u;
inline constexpr uint32_t kUiEdgeRight = 0x7F000007u;

// Whether a composite record's bucket is one of this side channel's messages (not a real bucket: those are
// single bits).
inline bool IsUiMessage(uint32_t bucket) { return (bucket & 0xFF000000u) == 0x7F000000u; }

// Around the guest's draw of one instance of the Flash player (the guest render thread): `begin` at the
// entry of sub_825FB348 with the instance (r3), the end at its exit with the same instance (r30). Sends the
// renderer kUiPieceLeft / kUiPieceRight / kUiPieceEnd when the instance is a piece and hud_width is above 0,
// and kUiEdgeRight for the title screen's prompt bar (PROMPTMOVIE's root until the game is entered, ultrawide.cpp);
// a no-op otherwise (the picture is not wider than 16:9, the Xenia renderer).
void NoteUiPiece(uint32_t instance, bool begin);

}  // namespace mc::ultrawide
