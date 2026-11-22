#pragma once
// Written by merge-tools/wpl/cvar_convert.py from LARecomp's REXCVAR_DEFINE_* lines. From here on it is
// plain source: edit it like any other header. All owners share the scope "larecomp" and the members keep
// the names players know, so `larecomp.<name>` is the full name and `<name>` stays an alias (RegisterAll).

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace larecomp::settings {

class GarageSettings final : public rex::SettingOwner {
 public:
  GarageSettings() : SettingOwner("larecomp") {}

  REX_SETTING(bool, capture_vinyl_shapes, false, "Hands-free: force-load every vinyl shape a few at a time, hash each, and "
    "build the full ShapeIdx->hash map in vinyls/vinyl_shape_hashes.txt (+ .json) "
    "— no manual browsing. Enter the Vinyl editor (main menu, not a picker grid), "
    "then toggle ON; it sweeps all ~894 shapes over a few seconds and logs when "
    "done. Join the hashes to the DDS the texture dumper already wrote.", rex::kDebugOnly);

  REX_SETTING(bool, dump_vinyl_shapes, false, "Dump the vinyl shape catalog (every ShapeIdx -> source bitmap filename + "
    "texture header) to <exe dir>/vinyls/vinyl_shapes.json. Enter the Vinyl editor "
    "once so the shape library loads, then toggle ON; it fires once and flips back "
    "OFF. Used to build the image->vinyl decomposer's brush set.", rex::kDebugOnly);

  REX_SETTING(bool, export_vinyl, false, "Export the current car's vinyl layers to a .vgp file in <exe dir>/vinyls/ "
    "(auto-named vinyl_<car>_<timestamp>.vgp). Toggle ON while in the garage with "
    "a car loaded; it fires once and flips back OFF. Share the file; import later.", rex::kDebugOnly);

  REX_SETTING(bool, extra_vinyl_layers, false, "Raise the front/rear BUMPER vinyl caps from 16 to 31 layers each "
    "(64/64/64/31/31). Top and side caps stay at 64 because MCLA's vinyl "
    "composite pipeline hard-caps a single surface at 64 layers (fixed-64 "
    "work-area buffers; >64 overflows and crashes). Bumpers stay <=64 and keep "
    "their 5-bit save field, so existing garage/online cars are 100% compatible.", rex::kRequiresRestart);

  REX_SETTING(std::string, import_vinyl, "", "Import a .vgp vinyl package onto the current car. Type the file name (found "
    "in <exe dir>/vinyls/, with or without the .vgp extension) and press Enter "
    "while in the garage with a car loaded. It overwrites the car's current vinyl "
    "layers and re-composites, then clears the field. Layers past the active "
    "per-surface cap are dropped (enable extra_vinyl_layers first for 31 bumper "
    "slots).", rex::kDebugOnly);

  REX_SETTING(bool, mod_bodykits, true, "Line N of a car's `body` part shows wide body kit N-1 (widebody, spoiler_wide, "
    "hood_wide, door_l_wide, door_r_wide, trunk_wide index N-1). Shipped cars only "
    "have body_0/body_1 and are unaffected.", rex::kDebugOnly);

  REX_SETTING(bool, vinyl_auto_readback, true, "Fix: car vinyls only load outside the Vinyl Editor when GPU readback is on "
    "(the game builds the decal texture on the CPU from a GPU composite). This "
    "briefly enables the emulated GPU's resolve readback (d3d12_ or "
    "vulkan_readback_resolve) for ~1.5s around each vinyl (re)composite "
    "(garage entry / edits) so decals appear everywhere, while keeping readback "
    "OFF during racing for full performance. Leave ON.", rex::kDebugOnly);

};

inline GarageSettings g_garage;

inline auto& capture_vinyl_shapes = g_garage.capture_vinyl_shapes;
inline auto& dump_vinyl_shapes = g_garage.dump_vinyl_shapes;
inline auto& export_vinyl = g_garage.export_vinyl;
inline auto& extra_vinyl_layers = g_garage.extra_vinyl_layers;
inline auto& import_vinyl = g_garage.import_vinyl;
inline auto& mod_bodykits = g_garage.mod_bodykits;
inline auto& vinyl_auto_readback = g_garage.vinyl_auto_readback;

}  // namespace larecomp::settings
