#pragma once
// ===========================================================================
// LARecomp city pack -- the merge of the static lamp table with the running game's lights (CITY-RT M3 G2b; lamp_table.h says why there are two lists)
// ===========================================================================
// THE INPUT. The running game keeps its lights in the lighting manager's packed object (the Director's ReadRuntimeLights reads it with the game's own unpacker, sub_822F1B08):
//   the +8 list   lights.xlm's own lights again, packed: the SAME 15,174 in the same order, but the packing costs precision. A position is a 16 bit step of the grid's extent
//                 (x -3467 .. 2853 m: 0.096 m; z -2358 .. 2106 m: 0.068 m; y -31 .. 79 m: 0.002 m), colour, intensity, cone and spread are half floats (0.05 per cent).
//   the +24 list  the street lamps (6,298 spots), only here.
// Both lists are static once the city is loaded, so one read is enough for the session (the lists never change; the per-frame list at 0x8288B8D8 only picks lamps out of the
// second one). The Director's `Light` has these fields (`lamp` = from the +24 list): LiveLight is the plain copy, so that this library needs nothing of the guest.
//
// THE RULE.
//   1. A live FILL light (not from the +24 list) is matched to a file light 1:1: the same type, within `match_metres` (a few packing steps) in 3-D, the intensity within a relative
//      `intensity_tolerance`, colour channels within `colour_tolerance`, cone and spread within `angle_tolerance_deg`. The file light at the same ORDINAL (the k-th live fill light
//      against file index k) is tried first, which is the usual case and costs nothing; otherwise the nearest unmatched candidate wins. A matched pair is ONE light: the table keeps
//      the FILE's record (exact numbers, the Director's index) and marks it kLampSeenLive.
//   2. A live fill light no file light matches is kept as kLiveFill (a mod's light, another game build's file): the game lights it, so does the table.
//   3. A file light no live fill light matched stays (source kFile, not marked): the table is the file's, the live read only adds.
//   4. A street lamp is added as kStreetLamp with the game's numbers (never matched against the file: there are no duplicates, lamp_table.h; if a build ever lists one in both lists the
//      game's grid lights it twice, and so does the table).
// The result does not depend on the order the live lights come in (the table's order is a total order of the lights' own numbers; the tests shuffle a complete read), and merging the
// result's own live view again changes nothing (lamp_table_test.cpp holds both). A near duplicate in the file (two lights a few centimetres apart) can be matched either way round when the
// live read is incomplete; both are the same light to the eye.
//
// THE CONSUMER'S RULE (SelectLamps): the game's per-frame list holds the street lamps within 300 m of the eye with the game's own fade, so a consumer that reads that list (the volumetric
// light, the lamp shadows' near lamps) must not also take those lamps from the table: SelectLamps leaves out the street lamps the list covers while the list is fresh, and nothing else.
// ===========================================================================

#include <string>
#include <vector>

#include "citypack/lamp_table.h"

namespace larecomp::citypack {

struct LiveLight {
  float pos[3] = {0, 0, 0};
  float colour[3] = {1, 1, 1};
  float dir[3] = {0.0f, -1.0f, 0.0f};
  float intensity = 0.0f;
  float cone_deg = 360.0f;
  float spread_deg = 0.0f;
  uint8_t spot = 0;
  uint8_t on = 1;
  bool street = false;  // from the object's +24 list (the street lamps)
};

struct MergeOptions {
  float match_metres = 0.35f;          // the packing's worst error is 0.1 m (x): three steps of margin
  float intensity_tolerance = 0.01f;   // relative to the file light's intensity (half floats: 0.05 per cent); an absolute 0.01 is added so that 0 matches 0
  float colour_tolerance = 0.01f;      // each channel, 0..1 (half floats: 0.0005)
  float angle_tolerance_deg = 0.5f;    // cone and spread (half floats of up to 360: 0.25)
};

struct MergeReport {
  uint32_t file_lights = 0;        // in the static table
  uint32_t live_fill = 0;          // live lights not from the street list
  uint32_t live_street = 0;
  uint32_t matched = 0;            // live fill lights that are a file light
  uint32_t matched_by_ordinal = 0; // ... found at the same ordinal
  uint32_t live_only_fill = 0;     // live fill lights the file does not have
  uint32_t file_only = 0;          // file lights no live fill light matched
  uint32_t street_added = 0;
  uint32_t merged = 0;             // lights in the result
  double worst_match_metres = 0.0; // the largest position error of a match (the game's packing step, 0.1 m)
  double mean_match_metres = 0.0;
  std::string Summary() const;     // one line for the log
};

// Merges `file` (the pack's lamps) with `live` (see the rule above). The result carries kLampTableHasLive and the file table's other flags. An empty `live` returns the file table.
LampTable MergeLive(const LampTable& file, const std::vector<LiveLight>& live, const MergeOptions& options = MergeOptions{}, MergeReport* report = nullptr);

// The live view of a table (what a running game would hand over for the lights it has seen, unpacked): the seen file lights in the file's order, then the live-only fill lights, then the
// street lamps. Used by the tests and by a tool that wants to check a merge against itself.
std::vector<LiveLight> LiveViewOf(const LampTable& table);

// A MODEL of what the game's packing does to a light (the Director's reading of sub_822FE0C8 / sub_822F1B08: a position is a 16 bit step of a box, every other number a half float). The
// box is the packer's own and not known offline (CHECK: the grid builder's box is x -3467 .. 2853, z -2358 .. 2106 and the street lamps reach y 111 and x -3470: another box); a wider box
// only makes the steps coarser, so the model takes any box and the tests and the tool's `lamps --simulate-live` prove the merge rule for the box they are given.
struct PackingBox {
  float lo[3];
  float hi[3];
};
inline constexpr PackingBox kDefaultPackingBox = {{-3467.0f, -31.0f, -2358.0f}, {2853.0f, 79.0f, 2105.8f}};
float RoundThroughHalf(float v);  // to the nearest half float and back (the normal range; below 2^-14 the error is under 6e-8)
LiveLight SimulateGamePacking(const Lamp& lamp, bool street = false, const PackingBox& box = kDefaultPackingBox);

struct LampSelection {
  float eye[3] = {0, 0, 0};
  float radius_m = 300.0f;            // the consumer's reach around the eye
  bool live_list_fresh = false;       // the per-frame list was copied recently (vol_lamp_list.h Table::Latest said not stale)
  float live_radius_m = 300.0f;       // what the game's list covers: AddLight keeps the lamps within this of the eye (float 0x827E2CAC)
  float hour = -1.0f;                 // a negative hour takes every light whatever the hours
  float night_factor = -1.0f;
  const LampHours* hours = nullptr;   // null: the table's own
};
// The lamps of `table` a consumer should take for this frame: within the radius (3-D), shining at the hour, minus the street lamps the live list covers while it is fresh. Indices ascending.
void SelectLamps(const LampTable& table, const LampSelection& selection, std::vector<uint32_t>* out);

}  // namespace larecomp::citypack
