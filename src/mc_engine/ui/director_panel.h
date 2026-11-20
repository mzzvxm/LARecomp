#pragma once

#include <rex/ui/ui_system.h>

namespace mcla::ui {

// The Director panel (feature-parity/DIRECTOR-DESIGN.md section 3): the HUMAN side of the trailer toolkit's command layer. A thin ImGui layer over the Director's snapshot and
// commands: a timeline with the scene's keys to drag (ease per key by right click), camera keys by framing with the free camera, rigs with sliders, cars to spawn at the
// crosshair and to place, routes to record by driving or to drop as waypoints and edit, the routes and the camera's path drawn in the world, a button for the take. EVERY ACTION IS A
// COMMAND of the command layer and so a line of the scene file (the text stays the single source of truth); the panel keeps no second model of the scene.
//
// OPENING IT: the debug setting mcla_director_panel (the hidden DEBUG menu's TOOLS page has the row DIRECTOR PANEL), the key Ctrl+F12 (ui.bind.mcla_director, flips the setting:
// F2/F4/F9/F10/F11 and the other F-key menus are untouched), or an agent's `ui.open {panel:"director"}`. It needs the Director (mcla_director): the panel offers to switch it on.
//
// KEYS (the keyboard stays the game's too, so the F-key menus work while the panel is open; with the pointer on the dock the panel holds the keys, and the GAME KEYS box lets the
// game have them there as well, which a recorded drive does by itself):
//   Space play / pause   Home restart   , and . one frame back / on   K add a key at the playhead   [ and ] the previous / next key   Del delete the selected key
//   F the free camera on / off   G the gizmos in the world   Tab hide the panel for a clean look   Ctrl+Z / Ctrl+Y undo / redo   Esc ends the waypoint tool
//   CLOSE in the bar, the DEBUG menu row or Ctrl+F12 (with the pointer off the dock) close it
//   with the free camera: W A S D along the view, Q E down and up, Shift fast, Ctrl slow, Z X tilt, R levels the horizon, + - zoom, the RIGHT MOUSE BUTTON looks, the wheel steps the speed
//   the pad: the flight's own sticks, and Back + B add a key, Back + RB / LB next / previous key, Back + Start play / pause, Back + D-pad right one frame, Back + D-pad up the take
//
// THE DOCK'S SIZE follows the window (director_panel_core.h PlanDock): about a third of it, its font and paddings scaled from ImGui's display size (a small window gets a small dock, a
// 4K one a big one). SIZE in the bar steps small / normal / tall, COLLAPSE leaves the bar alone, and the transport bar wraps to a second row when the window is narrow. A click on a
// lane's title on the timeline collapses it to a thin strip (and again to open it), the "- LANES" corner does it for every lane but CAMERA, and a scene with more lanes than the dock
// has room for collapses the lower ones by itself. The inspector's tabs scroll with an arrow at each end when they do not fit. An agent or a test can set all of it with ui.open's
// `panel`: a tab's name, collapse, expand, hide, show, size-small, size-normal, size-tall, lanes-collapse, lanes-open, tabs-left, tabs-right.
//
// ImGui is touched only in the dialog's Draw on the UI thread (the 0.11 ImGuiHost resets the context after each UI frame, so the panel keeps no ImGui object between frames); it reads
// director::GetSnapshot() and posts commands (director::Post), whose replies it handles from its own Draw.
//
// The pure parts (director_timeline.h, director_gizmos.h, director_panel_core.h) have larecomp_director_panel_test.

// Adds the panel's dialog (UI thread, window open). It draws nothing while mcla_director_panel is off, and asks for no repaint then.
void StartDirectorPanel(rex::ui::IUISystem& ui);

// The key that flips mcla_director_panel: ui.bind.mcla_director, Ctrl+F12 by default.
rex::ui::OverlayConfig DirectorOverlayConfig();

}  // namespace mcla::ui
