// MCLA GPU renderer -- the car model walk: every car in the game from its own model data, checked against the frame's draws. See car_model_walk.h.

#include "car_model_walk.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_set>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "car_dump_rules.h"
#include "car_mesh_read.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "guest_device.h"
#include "native_gfx/guest/guest_resources.h"
#include "native_gfx/vertex_declaration.h"
#include "records.h"

REXCVAR_DEFINE_INT32(
    mcla_gpu_car_model_walk, 0, "MCLA/GPU",
    "Debug: every N-th guest frame (0 = off) reads the model data of every car the game has (the player's vehicle, the renderer's list of vehicles, every vehicle "
    "the game's draw call is made for: the cars the camera does not see too) straight from guest memory, down to each geometry's vertex and index buffer "
    "(address, stride, declaration, count, position element), bone index and bone palette, part matrix source and paint colours, and logs under [car model walk] "
    "how it compares with the draws of the frame: for a car the camera sees, how many of its walked geometries are a draw of the frame (matched), have no draw "
    "(walked-only) or how many draws of its have no geometry (drawn-only); for a car it does not see, how many geometries the walk found and whether their "
    "buffers look resident. Reads only, nothing is built from it and nothing is written in the guest. With this on the car draws are collected in every frame, "
    "whether a car effect asks for them or not. Never saved. Live.")
    .debug_only()
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu::car_model_walk {

namespace nfx = mcla::native_gfx;

namespace {

constexpr const char* kPrefix = "[car model walk]";

// The sources a vehicle was found from.
constexpr uint32_t kFromPlayer = 1;
constexpr uint32_t kFromList = 2;
constexpr uint32_t kFromTap = 4;

// The renderer's vehicle client (rear_view.cpp Patch_RearViewDiag reads it the same way): [0x8287E064] the renderer, + 128 + 4 * 7 the client of slot 7
// (mcCarModelClass, mask 0x12E183F3), +8 its list of render types (next at +12), each type's +20 its list of instances (next at +8).
constexpr uint32_t kRendererGlobal = 0x8287E064u;
constexpr uint32_t kRendererClients = 128u;
constexpr uint32_t kVehicleClientSlot = 7u;
constexpr uint32_t kClientMask = 4u;
constexpr uint32_t kClientTypes = 8u;
constexpr uint32_t kTypeNext = 12u;
constexpr uint32_t kTypeInstances = 20u;
constexpr uint32_t kInstanceNext = 8u;
constexpr uint32_t kMaxTypes = 64;
constexpr uint32_t kMaxInstances = 256;
// The vtable of mcCarModel (Vehicle::Draw is its slot 2 at 0x82047B1C): a candidate that has it is a vehicle for certain.
constexpr uint32_t kVehicleVtable = 0x82047B14u;

constexpr uint32_t kMaxCars = 24;
constexpr uint32_t kMaxDraws = 6000;
// A walk frame that logs the per-part lines of each car (the first time a vehicle is walked) and the samples of the first walks that did not match.
constexpr uint32_t kSampleFrames = 6;
// A tap slot that was not drawn in this many walks is given up.
constexpr uint8_t kIdleWalks = 4;
// A racer is told from the vehicle by the distance between their matrices.
constexpr double kRacerMetres = 3.0;
// The most draws and geometries a sample lists.
constexpr uint32_t kSamplesPerCar = 3;

const char* const kPartNames[kPartSlots] = {
    "conv_top", "conv_topup", "hood", "bumper_r", "trunk_swap", "intercooler", "bumper_f", "fenders", "skirts", "spoiler", "spoiler_b", "headlight", "taillight",
    "taillight_b", "frontgrill", "wheeliebar", "blower", "bike_tail", "bike_tank", "bike_cowl", "door_l", "door_r", "conv_topdown", "exh_pipe", "exh_side", "armfl",
    "armfr", "armrl", "armrr", "shockfl", "shockfr", "shockrl", "shockrr", "guiderl", "guiderr", "axle", "subbumf", "subbumr", "widebody", "spoiler_wide", "hood_wide",
    "door_l_wide", "door_r_wide", "trunk_wide", "interior", "seats_f", "seats_p", "door_l_int", "door_r_int", "stereo", "stereo_l", "stereo_r", "steer_whl",
    "post_gauge", "occluder_"};

const char* PartName(uint32_t part) { return part < kPartSlots ? kPartNames[part] : part == kBodyPart ? "body" : "?"; }

const uint8_t* GuestBase() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->virtual_membase() : nullptr;
}

// Guest memory for the walk (car_model_walk_rules.h `Mem`): every range is checked with IsGuestRangeReadable before a byte of it is touched; nothing is
// written. The guest is held back while a walk runs, so a page found readable stays so: the answers are cached by 4 KiB page for the walk's duration.
class GuestReader {
 public:
  GuestReader() : base_(GuestBase()) {}
  bool valid() const { return base_ != nullptr; }
  // A new walk: forget the pages.
  void Forget() { tags_.fill(0); }

  const uint8_t* Window(uint32_t va, uint32_t bytes) const {
    if (!base_ || va < 0x1000u || bytes == 0 || uint64_t(va) + bytes > 0x100000000ull) {
      return nullptr;
    }
    if (bytes <= kPageBytes) {
      const uint32_t first = va >> 12;
      const uint32_t last = uint32_t((uint64_t(va) + bytes - 1) >> 12);
      if (!PageReadable(first) || (last != first && !PageReadable(last))) {
        return nullptr;
      }
    } else if (!nfx::IsGuestRangeReadable(va, bytes)) {
      return nullptr;
    }
    return rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base_), va);
  }
  bool Word(uint32_t va, uint32_t* out) const {
    const uint8_t* p = (va & 3u) == 0 ? Window(va, 4) : nullptr;
    if (!p) {
      return false;
    }
    *out = car_dump::LoadBe32(p);
    return true;
  }
  bool Half(uint32_t va, uint32_t* out) const {
    const uint8_t* p = (va & 1u) == 0 ? Window(va, 2) : nullptr;
    if (!p) {
      return false;
    }
    *out = car_dump::LoadBe16(p);
    return true;
  }
  bool Byte(uint32_t va, uint32_t* out) const {
    const uint8_t* p = Window(va, 1);
    if (!p) {
      return false;
    }
    *out = *p;
    return true;
  }

 private:
  static constexpr uint32_t kPageBytes = 4096;
  static constexpr uint32_t kCachePages = 256;
  bool PageReadable(uint32_t page) const {
    const uint32_t slot = page & (kCachePages - 1);
    if (tags_[slot] == page + 1) {
      return readable_[slot] != 0;
    }
    const bool ok = nfx::IsGuestRangeReadable(page << 12, kPageBytes);
    tags_[slot] = page + 1;
    readable_[slot] = ok ? 1 : 0;
    return ok;
  }

  const uint8_t* base_;
  mutable std::array<uint32_t, kCachePages> tags_{};
  mutable std::array<uint8_t, kCachePages> readable_{};
};

// The tables of the retail image (the parts' paint regions, the parts the main pass draws); not read when the image is not what they expect.
void ReadOptions(const GuestReader& mem, Options* options) {
  *options = Options{};
  const uint8_t* regions = mem.Window(kRegionTableAddress, 4u * kPartSlots);
  const uint8_t* normal = mem.Window(kDrawTableAddress, kPartSlots);
  const uint8_t* alt = mem.Window(kDrawTableAltAddress, kPartSlots);
  if (!regions || !normal || !alt) {
    return;
  }
  bool plausible = true;
  for (uint32_t p = 0; p < kPartSlots; ++p) {
    options->region_of_part[p] = Be32(regions + 4u * p);
    options->draws_part[p] = normal[p];
    options->draws_part_alt[p] = alt[p];
    plausible = plausible && options->region_of_part[p] < kPaintRegions && normal[p] <= 1 && alt[p] <= 1;
  }
  options->tables_read = plausible;
}

bool PlausibleObject(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }

std::string PhasesText(uint32_t phases) {
  std::string out;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    if ((phases >> bit) & 1u) {
      out += std::format("{}{}", out.empty() ? "" : ",", bit == 31 ? std::string("other") : std::format("{:#x}", 1u << bit));
    }
  }
  return out.empty() ? std::string("none") : out;
}

std::string SourcesText(uint32_t sources) {
  std::string out;
  if (sources & kFromPlayer) {
    out += "player global";
  }
  if (sources & kFromList) {
    out += std::string(out.empty() ? "" : "+") + "client list";
  }
  if (sources & kFromTap) {
    out += std::string(out.empty() ? "" : "+") + "tap";
  }
  return out;
}

double Distance(const double a[3], const double b[3]) {
  const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

}  // namespace

uint32_t Interval() { return uint32_t(std::max<int32_t>(0, REXCVAR_GET(mcla_gpu_car_model_walk))); }

bool Walker::Wanted() {
  const bool on = Interval() != 0;
  tap::g_enabled.store(on, std::memory_order_relaxed);
  return on;
}

void Walker::StartFrame(uint64_t guest_frame, bool collecting) {
  const uint32_t n = Interval();
  frame_ = guest_frame;
  active_ = n != 0 && collecting && guest_frame % n == 0;
  walked_ = false;
  draws_.clear();
  draws_proxy_ = draws_no_key_ = draws_dropped_ = 0;
  found_.clear();
  cars_.clear();
  racers_.clear();
  list_ = ListScan{};
  note_.clear();
  if (active_ && draws_.capacity() < 1024) {
    draws_.reserve(1024);
  }
}

void Walker::Shutdown() {
  tap::g_enabled.store(false, std::memory_order_relaxed);
  active_ = false;
  walked_ = false;
  draws_.clear();
  cars_.clear();
  found_.clear();
}

// ---------------------------------------------------------------------------
// the draws
// ---------------------------------------------------------------------------

void Walker::NoteDraw(const DrawView& view, const XenosState& state, uint32_t id) {
  const DrawRecord& r = view.record();
  // The wheel proxies (a stencil-only draw per wheel with an ID of its own) are no model's geometry.
  uint32_t colour_mask = 0;
  for (uint32_t t = 0; t < kColorTargetCount; ++t) {
    if (state.color[t].bound) {
      colour_mask |= state.color[t].write_mask & 0xFu;
    }
  }
  if (car_dump::WheelProxyOf(r.vs_identity, r.ps_identity, colour_mask) != car_dump::ProxyWhy::kNo) {
    ++draws_proxy_;
    return;
  }
  if (draws_.size() >= kMaxDraws) {
    ++draws_dropped_;
    return;
  }
  nfx::DeclarationElement position;
  if (r.kind == DrawKind::kInline || !car_mesh::FindElement(view, r, car_mesh::kUsagePosition, &position) || position.stream >= guest::kStreamCount) {
    ++draws_no_key_;
    return;
  }
  const uint32_t slot = nfx::FetchSlotForStream(position.stream);
  if (slot >= guest::kFetchGroupCount * 3) {
    ++draws_no_key_;
    return;
  }
  DrawKey key;
  key.id = id;
  view.VertexFetch(slot, &key.vertex_d0, &key.vertex_d1);
  if ((key.vertex_d0 & kFetchTypeMask) != 3u) {
    ++draws_no_key_;
    return;
  }
  key.stride = uint32_t(r.stream_strides[position.stream]) * 4u;
  key.position_offset = position.offset;
  key.indexed = r.kind == DrawKind::kIndexed;
  key.index_object = r.index_object;
  key.index_dword0 = r.index_dword0;
  key.index_address = r.index_address;
  key.count = r.count;
  key.start = r.start;
  draws_.push_back(key);
}

void Walker::OnVehicleDraw(const DrawView& view, const XenosState& state, uint32_t id) {
  if (!active_) {
    return;
  }
  if (!walked_) {
    RunWalk();  // the first vehicle draw of the frame: the guest is held back inside this call
  }
  NoteDraw(view, state, id);
}

// ---------------------------------------------------------------------------
// the walk
// ---------------------------------------------------------------------------

void Walker::RunWalk() {
  walked_ = true;
  const auto started = std::chrono::steady_clock::now();
  GuestReader mem;
  if (!mem.valid()) {
    note_ = "no guest memory";
    return;
  }
  mem.Forget();
  if (!options_read_) {
    ReadOptions(mem, &options_);
    options_read_ = true;
  }

  // The candidates: the player's global, the renderer's vehicle client, the tap.
  const auto add = [&](uint32_t vehicle, uint32_t source, uint32_t phases) {
    for (Found& f : found_) {
      if (f.vehicle == vehicle) {
        f.sources |= source;
        f.phases |= phases;
        return;
      }
    }
    found_.push_back({vehicle, source, phases});
  };
  uint32_t player = 0;
  mem.Word(car_mesh::kPlayerVehicleGlobal, &player);
  player_vehicle_ = PlausibleObject(player) ? player : 0;
  if (player_vehicle_ != 0) {
    add(player_vehicle_, kFromPlayer, 0);
  }
  uint32_t renderer = 0, client = 0;
  if (mem.Word(kRendererGlobal, &renderer) && PlausibleObject(renderer)) {
    list_.renderer = true;
    if (mem.Word(renderer + kRendererClients + 4u * kVehicleClientSlot, &client) && PlausibleObject(client)) {
      list_.client = true;
      list_.client_object = client;
      mem.Word(client + kClientMask, &list_.client_mask);
      uint32_t type = 0;
      mem.Word(client + kClientTypes, &type);
      for (; PlausibleObject(type) && list_.types < kMaxTypes; mem.Word(type + kTypeNext, &type)) {
        ++list_.types;
        uint32_t instance = 0;
        mem.Word(type + kTypeInstances, &instance);
        for (; PlausibleObject(instance) && list_.instances < kMaxInstances; mem.Word(instance + kInstanceNext, &instance)) {
          ++list_.instances;
          add(instance, kFromList, 0);
        }
      }
    }
  }
  tap_vehicles_ = 0;
  tap_calls_total_ = tap::g_calls.load(std::memory_order_relaxed);
  tap_dropped_ = tap::g_dropped.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < tap::kSlots; ++i) {
    const uint32_t vehicle = tap::g_slots[i].vehicle.load(std::memory_order_relaxed);
    if (vehicle == 0) {
      idle_[i] = 0;
      continue;
    }
    const uint32_t phases = tap::g_slots[i].phases.exchange(0, std::memory_order_relaxed);
    if (phases == 0) {
      if (++idle_[i] >= kIdleWalks) {
        uint32_t expected = vehicle;
        tap::g_slots[i].vehicle.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
        idle_[i] = 0;
      }
      continue;
    }
    idle_[i] = 0;
    ++tap_vehicles_;
    add(vehicle, kFromTap, phases);
  }

  // The player's own position, and the opponent manager's racers (to tell which vehicle is a racer: CHECK 2 of the survey, whether racer+48's entity is the
  // vehicle's).
  car_mesh::PlayerVehicle player_state;
  car_mesh::ReadPlayerVehicle(&player_state, false);
  player_position_known_ = player_state.valid;
  for (int c = 0; c < 3; ++c) {
    player_position_[c] = player_state.position[c];
  }
  std::vector<car_rt_memory::VehicleRead> others;
  car_mesh::VehicleScan scan;
  if (player_state.valid) {
    car_mesh::ReadOtherVehicles(player_state, 1.0e6, &others, &scan, false);
  }

  // Walk them.
  if (found_.size() > kMaxCars) {
    note_ += std::format("{} vehicles found, the first {} walked; ", found_.size(), kMaxCars);
    found_.resize(kMaxCars);
  }
  cars_.resize(found_.size());
  racers_.assign(found_.size(), Racer{});
  for (uint32_t i = 0; i < found_.size(); ++i) {
    mem.Forget();
    WalkVehicle(mem, found_[i].vehicle, options_, &cars_[i]);
    const CarWalk& car = cars_[i];
    if (!car.ok || !car.position_read) {
      continue;
    }
    const double position[3] = {car.position[0], car.position[1], car.position[2]};
    for (const car_rt_memory::VehicleRead& other : others) {
      if (other.kind != car_rt_memory::VehicleKind::kOpponent) {
        continue;
      }
      double p[3];
      other.Position(p);
      const double d = Distance(position, p);
      if (!racers_[i].known || d < racers_[i].distance) {
        racers_[i].known = true;
        racers_[i].distance = d;
        racers_[i].racer = other.address;
      }
    }
  }
  walk_microseconds_ = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
}

// ---------------------------------------------------------------------------
// the comparison and the log
// ---------------------------------------------------------------------------

void Walker::FinishFrame() {
  if (!active_) {
    return;
  }
  active_ = false;
  ++walks_logged_;
  if (!walked_) {
    REXLOG_INFO("{} frame {}: no vehicle draw in this frame, nothing walked ({} draws noted)", kPrefix, frame_, draws_.size());
    return;
  }

  // Only the walks that reached a model set take part in the comparison.
  std::vector<const CarWalk*> usable;
  std::vector<uint32_t> index_of;  // usable index -> cars_ index
  for (uint32_t i = 0; i < cars_.size(); ++i) {
    if (cars_[i].ok) {
      usable.push_back(&cars_[i]);
      index_of.push_back(i);
    }
  }
  const FrameMatch frame = MatchFrame(usable, draws_);

  uint32_t cars_ok = 0, drawn = 0, culled = 0, failed = 0;
  for (uint32_t i = 0; i < cars_.size(); ++i) {
    cars_ok += cars_[i].ok;
    failed += !cars_[i].ok;
  }
  for (const CarMatch& m : frame.cars) {
    drawn += m.drawn;
    culled += !m.drawn;
  }
  uint64_t bytes = 0;
  for (const CarWalk& c : cars_) {
    bytes += c.validated_bytes;
  }
  REXLOG_INFO("{} frame {}: HEADER walked {} vehicles in {:.0f} us ({} are cars with a model set, {} are not; {} drawn, {} culled); sources: player global {:#010x}, "
              "renderer client {} (slot 7: {}, mask {:#010x}, {} types, {} instances), tap {} vehicles ({} calls so far, {} dropped); draws noted {} ({} wheel proxies, {} with no "
              "buffer key, {} dropped); matched {}, vertex-only {}, unowned {}; {:.1f} MB of vertex and index data read{}",
              kPrefix, frame_, cars_.size(), walk_microseconds_, cars_ok, failed, drawn, culled, player_vehicle_, list_.renderer ? (list_.client ? "found" : "slot empty") : "NOT READABLE",
              kVehicleClientSlot, list_.client_mask, list_.types, list_.instances, tap_vehicles_, tap_calls_total_, tap_dropped_, draws_.size(), draws_proxy_, draws_no_key_,
              draws_dropped_, frame.draws_matched, frame.draws_vertex_only, frame.draws_unowned, double(bytes) / (1024.0 * 1024.0),
              note_.empty() ? std::string() : " [" + note_ + "]");

  uint32_t failed_logged = 0;
  for (uint32_t i = 0; i < cars_.size(); ++i) {
    const CarWalk& car = cars_[i];
    const Found& found = found_[i];
    if (!car.ok) {
      if (++failed_logged <= 3) {
        REXLOG_INFO("{} frame {}: candidate {:#010x} ({}) is no vehicle with a model set: {}", kPrefix, frame_, car.vehicle, SourcesText(found.sources), car.why);
      }
      continue;
    }
  }
  uint32_t cars_logged = 0;
  for (uint32_t u = 0; u < usable.size(); ++u) {
    const CarWalk& car = *usable[u];
    const CarMatch& match = frame.cars[u];
    const uint32_t i = index_of[u];
    const Found& found = found_[i];
    const Racer& racer = racers_[i];
    const bool is_player = car.vehicle == player_vehicle_;
    const double position[3] = {car.position[0], car.position[1], car.position[2]};
    const double player_distance = player_position_known_ && car.position_read ? Distance(position, player_position_) : -1.0;
    const char* kind = is_player ? "player" : (racer.known && racer.distance < kRacerMetres) ? "racer" : "other";
    const uint32_t body_geometry = match.in_use - match.empty_in_use;
    const double percent = body_geometry != 0 ? 100.0 * double(match.matched_in_use) / double(body_geometry) : 0.0;
    uint32_t in_use_invalid = 0;
    for (const PartEntry& e : car.parts) {
      in_use_invalid += e.in_use && !e.valid;
    }
    uint32_t header_data = 0;
    for (const Geometry& g : car.geometries) {
      header_data += g.HeaderMatchesData();
    }

    REXLOG_INFO("{} frame {}: RESULT vehicle={:#010x} kind={} drawn={} geoms={} in_use={} in_use_tris={} matched={} matched_in_use={} pct={:.1f} matched_not_in_use={} "
                "walked_only={} drawn_only={} vertex_only={} draws={} resident={} validated={} entries_present={} loaded={} valid={} in_use_invalid={} exact={} idxobj={} idxaddr={} "
                "cnt={} stride={} posoff={} hdr_data={}",
                kPrefix, frame_, car.vehicle, kind, match.drawn ? 1 : 0, car.all.geometries, car.in_use.geometries, car.in_use.triangles, match.matched,
                match.matched_in_use, percent, match.matched_not_in_use, match.walked_only_in_use, match.drawn_only, match.vertex_only, match.draws, car.resident_in_use,
                car.validated_in_use, car.entries_present, car.entries_loaded, car.entries_valid, in_use_invalid, match.exact_words, match.index_object_equal,
                match.index_address_equal, match.index_count_equal, match.stride_equal, match.position_offset_equal, header_data);

    REXLOG_INFO("{} frame {}:   car {:#010x} ({}, {}) at ({:.1f}, {:.1f}, {:.1f}), {:.1f} m from the player{}: {}; seen by {} (tap phases {}); visible +116 = {}, enabled {}, "
                "lod body {} parts {} forced {} (lod distances {:.0f} / {:.0f} m), draw slot {}; entity state {}, body state {}, lod 0 {} (info {}); "
                "entity {:#010x}, model info {:#010x}, model set {:#010x}, customisation {:#010x}; render copy of the matrix at ({:.1f}, {:.1f}, {:.1f}), wheels {}",
                kPrefix, frame_, car.vehicle, kind, racer.known ? std::format("nearest racer {:.2f} m", racer.distance) : std::string("no racer near"), car.position[0], car.position[1],
                car.position[2], player_distance, is_player ? " (the player's)" : "", match.drawn ? "DRAWN this frame" : "CULLED (no draw of the frame is one of its geometries)",
                SourcesText(found.sources), PhasesText(found.phases), car.visible, car.enabled, car.body_lod, car.part_lod, car.forced_lod, car.lod_distance[0],
                car.lod_distance[1], car.draw_slot, car.entity_state, car.body_state, car.has_lod0 ? "present" : "absent", car.info_lod0 ? "present" : "absent", car.entity,
                car.info, car.model_set, car.custom, car.render_position[0], car.render_position[1], car.render_position[2],
                car.wheels_read ? std::format("({:.1f}, {:.1f}, {:.1f}) ({:.1f}, {:.1f}, {:.1f}) ({:.1f}, {:.1f}, {:.1f}) ({:.1f}, {:.1f}, {:.1f})", car.wheel_position[0][0],
                                              car.wheel_position[0][1], car.wheel_position[0][2], car.wheel_position[1][0], car.wheel_position[1][1], car.wheel_position[1][2],
                                              car.wheel_position[2][0], car.wheel_position[2][1], car.wheel_position[2][2], car.wheel_position[3][0], car.wheel_position[3][1],
                                              car.wheel_position[3][2])
                                : std::string("not readable"));
    REXLOG_INFO("{} frame {}:     model set: {} entries present, {} loaded (state 3), {} valid for the game's accessor, {} unreadable; {} geometries ({} in use: {} vertices, "
                "{} triangles; all by lod slot 0 / 1 / 2: {} / {} / {} geometries, {} / {} / {} triangles), {} skinned, {} shared drawables{}; {} of the in-use parts are NOT valid for the accessor (not loaded, no such variant or not placed); "
                "resident: {} of {} validated in-use geometries have readable data, indices in range and finite positions ({:.1f} MB read); header words name the data "
                "pointer's memory for {} of {} geometries",
                kPrefix, frame_, car.entries_present, car.entries_loaded, car.entries_valid, car.entries_unreadable, car.all.geometries, car.in_use.geometries,
                car.in_use.vertices, car.in_use.triangles, car.lod_tally[0].geometries, car.lod_tally[1].geometries, car.lod_tally[2].geometries,
                car.lod_tally[0].triangles, car.lod_tally[1].triangles, car.lod_tally[2].triangles, car.skinned, car.drawables_shared, car.truncated ? ", TRUNCATED" : "",
                in_use_invalid, car.resident_in_use, car.validated_in_use, double(car.validated_bytes) / (1024.0 * 1024.0), header_data, car.all.geometries);
    std::string ids_text;
    for (const uint32_t id : match.ids) {
      ids_text += std::format("{}{}", ids_text.empty() ? "" : ",", id);
    }
    if (match.drawn) {
      REXLOG_INFO("{} frame {}:     comparison: {} of {} in-use geometries are draws of the frame ({:.1f}%), {} geometries not in use are drawn too, {} in use have no draw (walked-only; "
                  "{} of the in-use ones have no indices), {} draws of its IDs are none of its geometries (drawn-only, {} of them by the vertex window only); {} draws are "
                  "its geometries, with the stencil IDs {}; the match is exact for {} of {} matched (fetch dwords), index object {}, index address {}, index count {}, stride {}, "
                  "position offset 0 {}; {} of its vertex windows are another car's too",
                  kPrefix, frame_, match.matched_in_use, body_geometry, percent, match.matched_not_in_use, match.walked_only_in_use, match.empty_in_use, match.drawn_only,
                  match.vertex_only, match.draws, ids_text, match.exact_words, match.matched, match.index_object_equal, match.index_address_equal, match.index_count_equal, match.stride_equal,
                  match.position_offset_equal, match.shared_buffers);
    } else {
      REXLOG_INFO("{} frame {}:     comparison: no draw of the frame is one of its geometries; the walk found {} geometries ({} in use, {} triangles) and {} of the {} validated "
                  "in-use geometries are resident (data readable, indices in range, finite positions)",
                  kPrefix, frame_, car.all.geometries, car.in_use.geometries, car.in_use.triangles, car.resident_in_use, car.validated_in_use);
    }

    // The paint, the matrix sources and the draws that did not match: the first walks, for the reading of the first run.
    if (walks_logged_ <= kSampleFrames) {
      for (uint32_t r = 0; r < kPaintRegions; ++r) {
        const Paint& p = car.paint[r];
        if (!p.read) {
          continue;
        }
        REXLOG_INFO("{} frame {}:     paint region {}: A ({:.3f}, {:.3f}, {:.3f}, {:.3f}) B ({:.3f}, {:.3f}, {:.3f}, {:.3f}) type {} gradient mode {} default {}", kPrefix, frame_, r,
                    p.a[0], p.a[1], p.a[2], p.a[3], p.b[0], p.b[1], p.b[2], p.b[3], p.type, p.mode, p.use_default);
      }
      uint32_t listed = 0;
      for (const PartEntry& e : car.parts) {
        if (!e.in_use || listed >= 70) {
          continue;
        }
        ++listed;
        uint32_t palette = 0, tris = 0;
        for (uint32_t g = 0; g < e.geometries && e.first_geometry + g < car.geometries.size(); ++g) {
          const Geometry& geometry = car.geometries[e.first_geometry + g];
          palette += geometry.palette_count != 0;
          tris += geometry.index_count / 3;
        }
        const double matrix_position[3] = {e.matrix_position[0], e.matrix_position[1], e.matrix_position[2]};
        REXLOG_INFO("{} frame {}:     in use: {} variant {} lod {}: entry {:#010x} state {} flags {:#010x} count {} {}; drawable {:#010x}, {} models, {} geometries ({} triangles, {} "
                    "skinned), item {:#010x}, matrices {:#010x}, matrix of the lowest bone {}: {}, paint region {}{}",
                    kPrefix, frame_, PartName(e.part), e.variant, e.lod, e.entry, e.state, e.flags, e.count, e.valid ? "VALID" : "NOT valid", e.drawable, e.models, e.geometries,
                    tris, palette, e.item, e.matrices, e.matrix_bone == kNoBone ? std::string("none") : std::to_string(e.matrix_bone),
                    e.matrix_read ? std::format("({:.2f}, {:.2f}, {:.2f}), {:.2f} m from the vehicle's position", e.matrix_position[0], e.matrix_position[1], e.matrix_position[2],
                                                car.position_read ? Distance(matrix_position, position) : -1.0)
                                  : std::string("not readable"),
                    e.region, e.shared_drawable ? ", shared drawable" : "");
      }
      LogSamples(car, match, u, frame);
    }
    if (++cars_logged >= 16) {
      break;
    }
  }
}

// Walked-only geometries and unmatched draws of a car with their raw words: what the first run's reading needs to see why a match failed.
void Walker::LogSamples(const CarWalk& car, const CarMatch& match, uint32_t car_index, const FrameMatch& frame) {
  if (!match.drawn) {
    return;
  }
  // the geometries of this car (by the index into the usable walks) that no draw hit: found again from the draws' own hits
  std::unordered_set<uint32_t> hit_geometry;
  for (const DrawMatch& d : frame.draws) {
    if (d.hit == DrawHit::kGeometry && d.car == car_index) {
      hit_geometry.insert(d.geometry);
    }
  }
  uint32_t shown = 0;
  for (uint32_t g = 0; g < car.geometries.size() && shown < kSamplesPerCar; ++g) {
    const Geometry& geometry = car.geometries[g];
    if (!geometry.in_use || geometry.index_count == 0 || hit_geometry.count(g)) {
      continue;
    }
    ++shown;
    REXLOG_INFO("{} frame {}:     walked-only sample: {} variant {} lod {} model {} slot {}: vertices {} indices {} stride {} decl {:#06x} (position at {}, type {}); vertex fetch {:#010x} "
                "{:#010x}, data {:#010x}; index object {:#010x} w0 {:#010x} w6 {:#010x} w7 {:#010x}, data {:#010x}",
                kPrefix, frame_, PartName(geometry.part), geometry.variant, geometry.lod, geometry.model, geometry.slot, geometry.vertex_count, geometry.index_count,
                geometry.vb_stride, geometry.decl.fvf, geometry.decl.offset[0], geometry.decl.position_type, geometry.vb_w6, geometry.vb_w7, geometry.vb_data, geometry.ib_header,
                geometry.ib_w0, geometry.ib_w6, geometry.ib_w7, geometry.ib_data);
  }
  shown = 0;
  for (uint32_t d = 0; d < draws_.size() && shown < kSamplesPerCar; ++d) {
    const DrawMatch& dm = frame.draws[d];
    if (dm.hit == DrawHit::kGeometry) {
      continue;
    }
    bool of_car = false;
    for (const uint32_t id : match.ids) {
      of_car = of_car || id == draws_[d].id;
    }
    if (!of_car) {
      continue;
    }
    ++shown;
    const DrawKey& k = draws_[d];
    REXLOG_INFO("{} frame {}:     drawn-only sample: id {} vertex fetch {:#010x} {:#010x} stride {} position offset {} {} index object {:#010x} dword0 {:#010x} address {:#010x} count {} "
                "start {} ({})",
                kPrefix, frame_, k.id, k.vertex_d0, k.vertex_d1, k.stride, k.position_offset, k.indexed ? "indexed" : "not indexed", k.index_object, k.index_dword0,
                k.index_address, k.count, k.start, dm.hit == DrawHit::kVertexOnly ? "the vertex window is a walked geometry's, its index buffer is not" : "no walked geometry has this vertex window");
  }
}

}  // namespace mcla::gpu::car_model_walk
