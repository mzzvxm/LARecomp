// MCLA GPU renderer -- the car model walk: every car in the game from its own model data, checked against the frame's draws. See car_model_walk.h.

#include "car_model_walk.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>
#include <rex/kernel/title_runtime.h>
#include <rex/memory/mapped_ptr.h>

#include "car_dump_rules.h"
#include "car_mesh_read.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "guest_device.h"
#include "native_gfx/guest/guest_resources.h"
#include "native_gfx/vertex_declaration.h"
#include "records.h"

namespace mcla::gpu::car_model_walk {

namespace nfx = mcla::native_gfx;
namespace cw = mcla::gpu::car_model_walk;

car_model_rt::PaintTuning CurrentPaintTuning() {
  return car_model_rt::ClampedPaintTuning(double(larecomp::settings::mcla_gpu_car_rt_paint_gamma.Get()), double(larecomp::settings::mcla_gpu_car_rt_paint_lit.Get()));
}

namespace {

constexpr const char* kPrefix = "[car model walk]";
constexpr const char* kProductionPrefix = "[car model rt]";

// The sources a vehicle was found from.
constexpr uint32_t kFromPlayer = 1;
constexpr uint32_t kFromList = 2;
constexpr uint32_t kFromTap = 4;
constexpr uint32_t kFromItems = 8;   // a traffic or parked car's render item (RT-MODEL-PARTS)
// The debug walk takes this many of the nearest traffic items within this many metres of the player.
constexpr uint32_t kTrafficWalkCars = 8;
constexpr double kTrafficWalkMetres = 60.0;

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
// Production: the frames between two calibrations of the pose once it is decided, a window of pairs after which the calibration starts over, and the log's pace.
constexpr uint64_t kCalibrateEveryFrames = 16;
constexpr uint64_t kCalibrateUndecidedFrames = 4;
constexpr uint32_t kCalibrationWindowPairs = 3000;
constexpr uint64_t kProofLogFrames = 600;
// Production: a cached walk of a vehicle nobody asked for in this many frames is given up.
constexpr uint64_t kCacheKeepFrames = 600;

const char* const kPartNames[kPartSlots] = {
    "conv_top", "conv_topup", "hood", "bumper_r", "trunk_swap", "intercooler", "bumper_f", "fenders", "skirts", "spoiler", "spoiler_b", "headlight", "taillight",
    "taillight_b", "frontgrill", "wheeliebar", "blower", "bike_tail", "bike_tank", "bike_cowl", "door_l", "door_r", "conv_topdown", "exh_pipe", "exh_side", "armfl",
    "armfr", "armrl", "armrr", "shockfl", "shockfr", "shockrl", "shockrr", "guiderl", "guiderr", "axle", "subbumf", "subbumr", "widebody", "spoiler_wide", "hood_wide",
    "door_l_wide", "door_r_wide", "trunk_wide", "interior", "seats_f", "seats_p", "door_l_int", "door_r_int", "stereo", "stereo_l", "stereo_r", "steer_whl",
    "post_gauge", "occluder_"};

const char* PartName(uint32_t part) { return part < kPartSlots ? kPartNames[part] : part == kBodyPart ? "body" : "?"; }

// The words of a render item for the log (when an item could not be read: which link is wrong): its vtable, +8, +12, +16, +20 and, of the word at +16 taken for the car,
// its flags byte (+500) and the translation row of its matrix (+176).
template <class Mem>
std::string ItemWords(const Mem& mem, uint32_t item) {
  uint32_t w[6] = {};
  const uint32_t at[6] = {0, 8, 12, 16, 20, 24};
  bool ok[6] = {};
  for (int i = 0; i < 6; ++i) {
    ok[i] = mem.Word(item + at[i], &w[i]);
  }
  std::string text = std::format("item {:#010x}: +0 {:#010x} +8 {:#010x} +12 {:#010x} +16 {:#010x} +20 {:#010x} +24 {:#010x}{}", item, w[0], w[1], w[2], w[3], w[4], w[5],
                                 (ok[0] && ok[1] && ok[2] && ok[3] && ok[4] && ok[5]) ? "" : " (some of them not readable)");
  uint32_t flags = 0;
  if (cw::Plausible(w[3]) && mem.Byte(w[3] + cw::kAmbCarFlags, &flags)) {
    text += std::format("; the +16 object's byte +500 {:#04x}", flags);
    uint32_t t[3] = {};
    if (mem.Word(w[3] + cw::kAmbCarMatrix + cw::kMatrixTranslation, &t[0]) && mem.Word(w[3] + cw::kAmbCarMatrix + cw::kMatrixTranslation + 4, &t[1]) &&
        mem.Word(w[3] + cw::kAmbCarMatrix + cw::kMatrixTranslation + 8, &t[2])) {
      float f[3];
      for (int i = 0; i < 3; ++i) {
        std::memcpy(&f[i], &t[i], 4);
      }
      text += std::format(", its row +176 ({:.1f}, {:.1f}, {:.1f}) as floats", f[0], f[1], f[2]);
    }
  } else {
    text += "; the +16 word is not a readable object";
  }
  return text;
}

const uint8_t* GuestBase() {
  auto* runtime = REX_RUNTIME();
  return runtime ? runtime->GetMembase().GetBase() : nullptr;
}

}  // namespace

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

namespace {

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
  if (sources & kFromItems) {
    out += std::string(out.empty() ? "" : "+") + "ambient client item";
  }
  return out;
}

double Distance(const double a[3], const double b[3]) {
  const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

}  // namespace

uint32_t Interval() { return uint32_t(std::max<int32_t>(0, larecomp::settings::mcla_gpu_car_model_walk.Get())); }

// The mesh source the scene reads triangles with (inside the first vehicle draw's call of a producing frame): a fresh page cache each call, the walk's own reader.
class Walker::Source final : public MeshSource {
 public:
  car_model_rt::MeshProblem Read(const CarWalk& walk, uint32_t geometry, std::vector<float>* positions, std::vector<uint32_t>* indices,
                                 car_model_rt::MeshRead* info) const override {
    GuestReader mem;
    if (!mem.valid() || geometry >= walk.geometries.size()) {
      return car_model_rt::MeshProblem::kUnreadable;
    }
    mem.Forget();
    return car_model_rt::ReadGeometryMesh(mem, walk.geometries[geometry], positions, indices, info);
  }
};

Walker::Walker() : source_(std::make_unique<Source>()) {}
Walker::~Walker() = default;

const MeshSource& Walker::mesh_source() const {
  return *source_;
}

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
  // production: what the scene asked for in the frame before
  production_frame_ = collecting && production_next_.on && production_next_.limits.On() && production_next_.mode != car_model_rt::Mode::kOff;
  production_now_ = production_frame_ ? production_next_ : Production{};
  production_next_ = Production{};  // asked for again in every frame it is wanted
  production_walked_ = false;
  fresh_ = false;
  walked_cars_.clear();
  // The parts' pose is calibrated against the draws every kCalibrateEveryFrames and until it is decided; the wheels' and the traffic's hypotheses, which only some frames can
  // test (a drawn traffic car, a wheel draw), are tried every few frames until they are decided (the reading of the poses costs the render thread a little).
  const bool wheels_undecided = production_frame_ && production_now_.wheels && !(wheel_calibration_.decided(false) && wheel_calibration_.decided(true));
  const bool traffic_undecided = production_frame_ && production_now_.traffic_cars != 0 && !traffic_calibration_.decided();
  calibrating_ = production_frame_ && (!calibration_.decided() || guest_frame >= last_calibration_frame_ + kCalibrateEveryFrames ||
                                       ((wheels_undecided || traffic_undecided) && guest_frame >= last_calibration_frame_ + kCalibrateUndecidedFrames));
  draws_.clear();
  draws_proxy_ = draws_no_key_ = draws_dropped_ = 0;
  found_.clear();
  cars_.clear();
  racers_.clear();
  list_ = ListScan{};
  note_.clear();
  if ((active_ || production_frame_) && draws_.capacity() < 1024) {
    draws_.reserve(1024);
  }
  if (!production_frame_ && !cache_.empty() && guest_frame % 600 == 0) {
    cache_.clear();  // nobody asked for a while: the cached walks go
  }
}

void Walker::Shutdown() {
  tap::g_enabled.store(false, std::memory_order_relaxed);
  active_ = false;
  walked_ = false;
  production_frame_ = false;
  production_walked_ = false;
  production_next_ = Production{};
  walked_cars_.clear();
  cache_.clear();
  draws_.clear();
  cars_.clear();
  found_.clear();
}

// ---------------------------------------------------------------------------
// the draws
// ---------------------------------------------------------------------------

void Walker::NoteDraw(const DrawView& view, const XenosState& state, uint32_t id, const temporal::Mat4* world) {
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
  if (world) {
    key.has_world = true;
    key.world = *world;
  }
  draws_.push_back(key);
}

void Walker::OnVehicleDraw(const DrawView& view, const XenosState& state, uint32_t id, const temporal::Mat4* world) {
  if (!active_ && !production_frame_) {
    return;
  }
  if (active_ && !walked_) {
    RunWalk();  // the first vehicle draw of the frame: the guest is held back inside this call
  }
  if (production_frame_ && !production_walked_) {
    RunProduction();
  }
  NoteDraw(view, state, id, world);
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
    found_.push_back({vehicle, source, phases, 0});
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
  // The traffic and the parked cars (RT-MODEL-PARTS): the nearest items of the renderer's ambient client, each walked as one drawable (car_model_traffic_rules.h).
  ambient_.clear();
  {
    cw::AmbientScan items_scan;
    const std::vector<uint32_t> items = cw::ListAmbientItems(mem, &items_scan);
    list_.ambient_renderer = items_scan.renderer;
    list_.ambient_client = items_scan.client;
    list_.ambient_items = items_scan.items;
    list_.ambient_models = items_scan.models;
    list_.ambient_flat = items_scan.flat_items;
    list_.ambient_foreign = items_scan.foreign_nodes;
    list_.ambient_mismatched = items_scan.mismatched;
    list_.ambient_truncated = items_scan.truncated;
    struct Near {
      cw::AmbientItem info;
      double distance;
    };
    std::vector<Near> near_items;
    if (player_state.valid) {
      for (const uint32_t item : items) {
        mem.Forget();
        cw::AmbientItem info = cw::ReadAmbientItem(mem, item);
        if (!info.ok) {
          ++list_.ambient_unreadable;
          ++list_.ambient_fail[size_t(info.fail)];
          if (list_.ambient_samples.size() < 2) {
            list_.ambient_samples.push_back(std::format("{} ({})", ItemWords(mem, item), info.why));
          }
          continue;
        }
        const double distance = Distance(info.position, player_position_);
        if (distance <= kTrafficWalkMetres) {
          near_items.push_back({std::move(info), distance});
        }
      }
      std::stable_sort(near_items.begin(), near_items.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
      if (near_items.size() > kTrafficWalkCars) {
        near_items.resize(kTrafficWalkCars);
      }
    }
    const size_t vehicles = found_.size();
    for (const Near& n : near_items) {
      found_.push_back({n.info.item, kFromItems, 0, 0});
    }
    ambient_.assign(found_.size(), cw::AmbientItem{});
    for (size_t k = 0; k < near_items.size(); ++k) {
      ambient_[vehicles + k] = near_items[k].info;
    }
  }
  cars_.resize(found_.size());
  racers_.assign(found_.size(), Racer{});
  wheel_mats_.assign(found_.size(), cw::WheelMatrices{});
  traffic_poses_.assign(found_.size(), cw::TrafficPoseData{});
  ambient_colour_.assign(found_.size(), cw::AmbientColour{});
  for (uint32_t i = 0; i < found_.size(); ++i) {
    mem.Forget();
    mem.Word(found_[i].vehicle, &found_[i].vtable);
    if (found_[i].sources & kFromItems) {
      cw::Options options = options_;
      options.min_lod = 0;  // the item's own lod where the main view shows the car, else the finest: the debug walk compares with the draws
      cw::WalkAmbient(mem, ambient_[i], options, &cars_[i]);
      if (cars_[i].ok) {
        traffic_poses_[i] = cw::ReadTrafficPose(mem, ambient_[i], cw::BonesNeeded(cars_[i]));
        ambient_colour_[i] = cw::ReadAmbientColour(mem, ambient_[i]);
      }
    } else {
      WalkVehicle(mem, found_[i].vehicle, options_, &cars_[i]);
      wheel_mats_[i] = cw::ReadWheelMatrices(mem, found_[i].vehicle);
    }
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
// production: the cars of the ray traced scene
// ---------------------------------------------------------------------------

namespace {

namespace crt = car_model_rt;
namespace tp = temporal;

uint64_t HashBytes(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h;
}

// The wheel resources a vehicle wears and the flags the wheel draw tests (a change makes the cached walk start again): the tyre's and the rim's ids and four flag bits.
uint64_t WheelSignature(const GuestReader& mem, uint32_t vehicle) {
  uint32_t tyre = 0, rim = 0, b = 0, hide = 0;
  mem.Word(vehicle + cw::kVehicleTyreId, &tyre);
  mem.Word(vehicle + cw::kVehicleRimId, &rim);
  uint64_t bits = 0;
  bits |= (mem.Byte(vehicle + cw::kVehicleTyreOn, &b) && b != 0) ? 1u : 0u;
  bits |= (mem.Byte(vehicle + cw::kVehicleRimOn, &b) && b != 0) ? 2u : 0u;
  bits |= (mem.Byte(vehicle + cw::kVehicleLoop44A, &b) && b != 0) ? 4u : 0u;
  if (mem.Word(vehicle + cw::kVehicleWheelHide, &hide) && cw::Plausible(hide)) {
    uint32_t x = 0;
    bits |= ((mem.Byte(hide + 266, &x) && x != 0) || (mem.Byte(hide + 267, &x) && x != 0) || (mem.Byte(hide + 268, &x) && x != 0)) ? 8u : 0u;
  }
  return (uint64_t(tyre) << 40) ^ (uint64_t(rim) << 8) ^ bits ^ 0x5748454Cull;  // "WHEL"
}

// Whether a matrix read from the guest is a car's pose (car_rt_memory_rules.h UsableRows).
bool UsableMatrix(const tp::Mat4& m) {
  double rows[4][3];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      rows[r][c] = m.m[r][c];
    }
  }
  return car_rt_memory::UsableRows(rows);
}

}  // namespace

void Walker::RunProduction() {
  production_walked_ = true;
  const auto started = std::chrono::steady_clock::now();
  GuestReader mem;
  if (!mem.valid()) {
    return;
  }
  mem.Forget();
  if (!options_read_) {
    ReadOptions(mem, &options_);
    options_read_ = true;
  }
  const Production& prod = production_now_;
  const double scan = car_rt_memory::ScanRadius(prod.limits.radius);

  // the candidates: the player's vehicle and the renderer's list of vehicles
  std::vector<uint32_t> vehicles;
  const auto add = [&](uint32_t vehicle) {
    if (std::find(vehicles.begin(), vehicles.end(), vehicle) == vehicles.end()) {
      vehicles.push_back(vehicle);
    }
  };
  uint32_t player = 0;
  mem.Word(car_mesh::kPlayerVehicleGlobal, &player);
  player_vehicle_ = PlausibleObject(player) ? player : 0;
  if (player_vehicle_ != 0) {
    add(player_vehicle_);
  }
  uint32_t renderer = 0, client = 0;
  if (mem.Word(kRendererGlobal, &renderer) && PlausibleObject(renderer) && mem.Word(renderer + kRendererClients + 4u * kVehicleClientSlot, &client) && PlausibleObject(client)) {
    uint32_t type = 0, types = 0, instances = 0;
    mem.Word(client + kClientTypes, &type);
    for (; PlausibleObject(type) && types < kMaxTypes; mem.Word(type + kTypeNext, &type)) {
      ++types;
      uint32_t instance = 0;
      mem.Word(type + kTypeInstances, &instance);
      for (; PlausibleObject(instance) && instances < kMaxInstances; mem.Word(instance + kInstanceNext, &instance)) {
        ++instances;
        add(instance);
      }
    }
  }
  car_mesh::PlayerVehicle player_state;
  car_mesh::ReadPlayerVehicle(&player_state, false);
  player_position_known_ = player_state.valid;
  for (int c = 0; c < 3; ++c) {
    player_position_[c] = player_state.position[c];
  }
  if (!player_state.valid) {
    return;  // no player: nothing is ranked (the scene has no top level either)
  }

  const crt::Pose pose = this->pose();
  const uint32_t per_car = std::max<uint32_t>(prod.limits.instances / std::max<uint32_t>(prod.limits.cars, 1u), 16u);
  const uint32_t min_indices = std::max<uint32_t>(prod.limits.min_indices, crt::kMinPartIndices);
  std::vector<crt::CarInfo> infos;
  std::unordered_set<uint32_t> seen;
  for (const uint32_t vehicle : vehicles) {
    mem.Forget();
    uint32_t entity = 0, info = 0, model_set = 0, custom = 0, w = 0;
    if (!mem.Word(vehicle + cw::kVehicleEntity, &entity) || !PlausibleObject(entity) || !mem.Word(entity + cw::kEntityModelInfo, &info) || !PlausibleObject(info) ||
        !mem.Word(info + cw::kInfoModelSet, &model_set) || !PlausibleObject(model_set)) {
      continue;
    }
    mem.Word(entity + cw::kEntityCustom, &custom);
    tp::Mat4 entity_matrix;
    const bool has_matrix = cw::ReadMatrix(mem, entity + cw::kEntityMatrix, &entity_matrix);
    if (!has_matrix || !UsableMatrix(entity_matrix)) {
      continue;
    }
    WalkedCar car;
    car.vehicle = vehicle;
    car.is_player = vehicle == player_vehicle_;
    for (int c = 0; c < 3; ++c) {
      car.position[c] = entity_matrix.m[3][c];
    }
    car.distance = Distance(car.position, player_position_);
    if (!car.is_player && car.distance > scan) {
      continue;
    }
    if (mem.Half(vehicle + cw::kVehicleVisible, &w)) {
      car.visible = w != 0;
    }
    // the lod floor: the cars the main view shows are walked at the game's own lod (their geometry is then the draws' and the placement can be checked against them),
    // the others at the reflection's
    car.min_lod = (car.is_player || car.visible) ? 0 : 1;

    // the signature of the structure
    crt::WalkSignature sig;
    sig.model_set = model_set;
    sig.min_lod = car.min_lod;
    uint32_t v = 0;
    sig.body_lod = mem.Word(vehicle + cw::kVehicleBodyLod, &v) ? int32_t(v) : -3;
    sig.part_lod = mem.Word(vehicle + cw::kVehiclePartLod, &v) ? int32_t(v) : -3;
    if (const uint8_t* c = cw::Plausible(custom) ? mem.Window(custom + cw::kCustomVariants, cw::kPartSlots + 4) : nullptr) {
      sig.variants = HashBytes(c, cw::kPartSlots + 4);
    }
    uint32_t b = 0;
    for (uint32_t k = 0; k < 7; ++k) {
      static const uint32_t offsets[7] = {cw::kVehicleGhost, cw::kVehicleLoop44A, cw::kVehicleLoop44B, cw::kVehicleBumperFront, cw::kVehicleBumperRear, 0, 0};
      if (offsets[k] != 0 && mem.Byte(vehicle + offsets[k], &b) && b != 0) {
        sig.flags |= 1u << k;
      }
    }
    sig.wheels = prod.wheels ? WheelSignature(mem, vehicle) : 0;
    sig.walked_frame = frame_;

    Cached& cached = cache_[vehicle];
    if (!cached.walk || !crt::WalkStillGood(cached.signature, sig, frame_)) {
      cw::Options options = options_;
      options.production = true;
      options.validate = false;
      options.min_lod = car.min_lod;
      options.wheels = prod.wheels;
      auto walk = std::make_shared<cw::CarWalk>();
      cw::WalkVehicle(mem, vehicle, options, walk.get());
      cached.walk = std::move(walk);
      cached.signature = sig;
      cached.picks = cached.walk->ok ? crt::PickGeometries(*cached.walk, min_indices, (car.is_player || car.visible) ? 0u : per_car) : std::vector<uint32_t>();
    }
    cached.last_frame = frame_;
    const cw::CarWalk& walk = *cached.walk;
    if (!walk.ok) {
      continue;
    }
    car.walk = cached.walk;
    car.paint = crt::PaintColourOf(walk.paint[0], CurrentPaintTuning());

    // this frame's placements (a car the camera sees only when the calibration wants them: they are not placed)
    const bool want_parts = !car.visible && !car.is_player;
    if (want_parts || calibrating_) {
      tp::Mat4 entity_for_pose = entity_matrix;
      std::unordered_map<uint64_t, uint32_t> pose_index;  // (entry, bone) -> the place in car.poses
      std::vector<crt::PoseSet> poses;
      if (calibrating_) {
        car.pose_of_geometry.assign(walk.geometries.size(), -1);
      }
      for (const uint32_t g : cached.picks) {
        const cw::Geometry& geometry = walk.geometries[g];
        if (cw::IsWheelPart(geometry.part)) {
          // A wheel geometry (the tyre or the rim): one mesh, one placement per wheel the vehicle draws, by the wheel matrices (car_model_wheel_rules.h). The calibration of the
          // wheels' pose (Resolve: the car's own wheel draws, the player's among them) decides; a wheel is not placed before a hypothesis fits the draws.
          if (!car.wheels_read) {
            car.wheel_matrices = cw::ReadWheelMatrices(mem, vehicle);
            car.wheels_read = true;
          }
          const bool rim = cw::IsRimPart(geometry.part);
          if (want_parts) {
            if (!wheel_calibration_.fits(rim)) {
              ++car.wheels_held;
              continue;
            }
            for (const tp::Mat4& placed : car_model_parts::WheelPlacements(car.wheel_matrices, walk.wheels.present, rim, geometry.bone, wheel_calibration_.chosen(rim))) {
              crt::Part part;
              part.geometry = g;
              part.key = crt::GeometryKey(geometry);
              part.indices = geometry.index_count;
              part.triangles = geometry.index_count / 3;
              part.world = placed;
              part.part = geometry.part;
              car.parts.push_back(part);
            }
          }
          continue;
        }
        if (geometry.entry_index >= walk.parts.size()) {
          continue;
        }
        const uint64_t key = (uint64_t(geometry.entry_index) << 32) | geometry.bone;
        auto found = pose_index.find(key);
        if (found == pose_index.end()) {
          const crt::BoneMatrices bone = crt::ReadBoneMatrices(mem, walk.parts[geometry.entry_index], geometry.bone);
          poses.push_back(crt::ComposePoses(bone, &entity_for_pose));
          found = pose_index.emplace(key, uint32_t(poses.size() - 1)).first;
        }
        const crt::PoseSet& set = poses[found->second];
        if (calibrating_) {
          car.pose_of_geometry[g] = int32_t(found->second);
        }
        if (want_parts && set.ok[size_t(pose)] && crt::UsablePlacement(set.m[size_t(pose)])) {
          crt::Part part;
          part.geometry = g;
          part.key = crt::GeometryKey(geometry);
          part.indices = geometry.index_count;
          part.triangles = geometry.index_count / 3;
          part.world = set.m[size_t(pose)];
          part.part = geometry.part;
          car.parts.push_back(part);
        }
      }
      if (calibrating_) {
        car.poses = std::move(poses);
      }
    }
    seen.insert(vehicle);
    walked_cars_.push_back(std::move(car));
  }
  // the traffic and the parked cars (RT-MODEL-PARTS)
  traffic_scan_ = TrafficScan{};
  if (prod.traffic_cars != 0) {
    AddTraffic(mem, prod, scan, min_indices, per_car);
  }
  // what each car's parts are by kind (the log's and the check's numbers)
  for (WalkedCar& car : walked_cars_) {
    car.composition = car_model_parts::Composition{};
    for (const crt::Part& part : car.parts) {
      car_model_parts::Count(&car.composition, part.part);
    }
    car.composition.skinned_left_out = car.walk ? car_model_parts::SkinnedInUse(*car.walk) : 0;
  }
  // the cars to stage: near, not seen by the main view, within the class's count and a spare (the racers' and the traffic's counts apart)
  for (const WalkedCar& car : walked_cars_) {
    crt::CarInfo info;
    info.vehicle = car.vehicle;
    info.is_player = car.is_player;
    info.visible = car.visible;
    info.traffic = car.traffic;
    info.distance = car.distance;
    info.parts = uint32_t(car.parts.size());
    for (int c = 0; c < 3; ++c) {
      info.position[c] = car.position[c];
    }
    infos.push_back(info);
  }
  for (const uint32_t i : car_model_parts::ChooseStagedAll(infos, prod.limits, prod.traffic_cars)) {
    walked_cars_[i].staging = true;
  }
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (frame_ > it->second.last_frame + kCacheKeepFrames) {
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
  fresh_ = true;
  walk_microseconds_ = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
}

// THE TRAFFIC AND THE PARKED CARS (RT-MODEL-PARTS; car_model_traffic_rules.h): the items of the renderer's ambient client, each a car object with one model, within the scan radius
// of the player. The model's one drawable is walked at the reflection's lod (the game's own lod where the main view shows the car: its draws are then the same geometry),
// cached per item; the pose data (the root and the bones of the item's block, the car's render-copy matrix) is read here, in the draw's call, in the frames that place the car or
// calibrate; a car the main view does not show is placed by the traffic calibration's hypothesis once one fits the draws of the traffic cars the camera sees.
void Walker::AddTraffic(const GuestReader& mem, const Production& prod, double scan, uint32_t min_indices, uint32_t per_car) {
  cw::AmbientScan list;
  const std::vector<uint32_t> items = cw::ListAmbientItems(mem, &list);
  traffic_scan_.renderer = list.renderer;
  traffic_scan_.client = list.client;
  traffic_scan_.models = list.models;
  traffic_scan_.items = uint32_t(items.size());
  (void)prod;
  for (const uint32_t item : items) {
    // the car's position first: the cars far from the player are left alone (an item list has up to a few hundred)
    double at[3];
    if (!cw::ReadAmbientPosition(mem, item, at)) {
      ++traffic_scan_.unreadable;
      continue;
    }
    if (!(Distance(at, player_position_) <= scan)) {
      continue;
    }
    ++traffic_scan_.in_range;
    const cw::AmbientItem info = cw::ReadAmbientItem(mem, item);
    if (!info.ok) {
      ++traffic_scan_.unreadable;
      continue;
    }
    if (!info.Eligible()) {
      ++traffic_scan_.rejected;
      continue;
    }
    WalkedCar car;
    car.traffic = true;
    car.vehicle = item;
    for (int c = 0; c < 3; ++c) {
      car.position[c] = info.position[c];
    }
    car.distance = Distance(car.position, player_position_);
    car.visible = info.Visible();
    traffic_scan_.visible += car.visible ? 1u : 0u;
    car.item_lod = info.lod;
    car.car_type = info.type;
    car.min_lod = car.visible ? 0 : 1;

    crt::WalkSignature sig;
    sig.model_set = info.model;
    sig.min_lod = car.min_lod;
    sig.body_lod = info.lod;
    sig.part_lod = -3;
    sig.wheels = info.type;
    sig.walked_frame = frame_;
    Cached& cached = cache_[item];
    if (!cached.walk || !crt::WalkStillGood(cached.signature, sig, frame_)) {
      cw::Options options = options_;
      options.production = true;
      options.validate = false;
      options.min_lod = car.min_lod;
      auto walk = std::make_shared<cw::CarWalk>();
      cw::WalkAmbient(mem, info, options, walk.get());
      cached.walk = std::move(walk);
      cached.signature = sig;
      cached.picks = cached.walk->ok ? crt::PickGeometries(*cached.walk, min_indices, car.visible ? 0u : per_car) : std::vector<uint32_t>();
      cached.colour = cw::ReadAmbientColour(mem, info);
    }
    cached.last_frame = frame_;
    const cw::CarWalk& walk = *cached.walk;
    if (!walk.ok) {
      continue;
    }
    car.walk = cached.walk;
    car.colour = cached.colour;

    // this frame's placements: a car the main view shows is not placed (its draws are the draw path's), but its pose is read in the frames that calibrate
    const bool want_parts = !car.visible;
    if (want_parts || calibrating_) {
      car.traffic_pose = cw::ReadTrafficPose(mem, info, cw::BonesNeeded(walk));
      car.pose_read = true;
      if (want_parts && !car.traffic_pose.Usable()) {
        ++traffic_scan_.block_missing;
      }
      // A car is placed by the calibration against the draws where one fits them; where none has decided (no traffic draw gave it a pair: the draws of the xAmbient* shaders decode
      // no model-to-world), by the block's check against the car's own matrix (car_model_parts_rt_rules.h TrafficSelfCheck). A calibration that decided and does NOT fit places nothing.
      bool by_self_check = false;
      if (want_parts && !traffic_calibration_.fits() && !traffic_calibration_.decided() && car.traffic_pose.Usable()) {
        const tp::Mat4& reference = car.traffic_pose.car_render_ok ? car.traffic_pose.car_render : info.matrix;
        const car_model_parts::TrafficSelfCheck check = car_model_parts::CheckTrafficBlock(car.traffic_pose, traffic_calibration_.chosen(), reference);
        by_self_check = check.ok;
        if (traffic_selfcheck_logs_ < 8 || frame_ >= traffic_selfcheck_log_frame_ + kProofLogFrames) {
          ++traffic_selfcheck_logs_;
          traffic_selfcheck_log_frame_ = frame_;
          REXLOG_INFO("{} frame {}: TRAFFIC SELF-CHECK (no draw has given the traffic calibration a pair: {} samples): item {:#010x}, car {:#010x}: the body bone of the block places the car "
                      "{:.3f} m from its own {} matrix, largest 3x3 difference {:.3f}: {} (the limits are {:.2f} m and {:.2f})",
                      kProductionPrefix, frame_, traffic_calibration_.samples(), item, info.car, check.metres, car.traffic_pose.car_render_ok ? "render-copy" : "update-copy", check.basis,
                      !check.placed ? "the block could not be turned into a placement" : check.ok ? "accepted: the car is placed by it" : "NOT accepted: the car is not placed",
                      car_model_parts::kSelfCheckMetres, car_model_parts::kSelfCheckBasis);
        }
      }
      if (want_parts && (traffic_calibration_.fits() || by_self_check)) {
        traffic_scan_.self_checked += by_self_check ? 1u : 0u;
        for (const uint32_t g : cached.picks) {
          const cw::Geometry& geometry = walk.geometries[g];
          tp::Mat4 placed;
          if (!cw::PlaceTraffic(car.traffic_pose, geometry.bone, traffic_calibration_.chosen(), &placed) || !crt::UsablePlacement(placed)) {
            continue;
          }
          crt::Part part;
          part.geometry = g;
          part.key = crt::GeometryKey(geometry);
          part.indices = geometry.index_count;
          part.triangles = geometry.index_count / 3;
          part.world = placed;
          part.part = geometry.part;
          car.parts.push_back(part);
        }
      }
    }
    walked_cars_.push_back(std::move(car));
  }
}

// The history trigger: which walked cars the frame draws, the calibration of the pose, and the placement of the chosen pose against the draws in the view space.
ProofReport Walker::Resolve(const temporal::Mat4& view, bool flip_z) {
  namespace crt = car_model_rt;
  ProofReport report;
  if (!production_frame_ || !production_walked_) {
    return report;
  }
  std::vector<const CarWalk*> usable;
  std::vector<uint32_t> index_of;  // usable index -> walked_cars_ index
  for (uint32_t i = 0; i < walked_cars_.size(); ++i) {
    if (walked_cars_[i].walk && walked_cars_[i].walk->ok) {
      usable.push_back(walked_cars_[i].walk.get());
      index_of.push_back(i);
    }
  }
  report.walked_cars = uint32_t(walked_cars_.size());
  if (usable.empty()) {
    return report;
  }
  for (const WalkedCar& car : walked_cars_) {
    report.traffic_walked += car.traffic ? 1u : 0u;
  }
  // Which car each draw is of (car_model_rt_rules.h AssignDraws: the cars of one model name the same buffers, so the buffers alone cannot say; the draw's stencil ID
  // and its origin do)
  std::vector<std::array<double, 3>> positions;
  for (const uint32_t i : index_of) {
    positions.push_back({walked_cars_[i].position[0], walked_cars_[i].position[1], walked_cars_[i].position[2]});
  }
  const crt::Assignment assigned = crt::AssignDraws(usable, positions, draws_);
  for (uint32_t u = 0; u < usable.size(); ++u) {
    WalkedCar& car = walked_cars_[index_of[u]];
    car.drawn = assigned.drawn[u] != 0;
    report.drawn_cars += car.drawn ? 1u : 0u;
    report.traffic_drawn += (car.drawn && car.traffic) ? 1u : 0u;
  }

  // the pairs: a matched (geometry, draw) whose placements were made this frame, and the draw's own matrix known
  struct Pair {
    uint32_t car;       // walked_cars_ index
    int32_t pose;       // the place in its poses
    uint32_t draw;
    uint32_t geometry;  // into the car's walk
  };
  std::vector<Pair> pairs;
  if (calibrating_) {
    for (uint32_t d = 0; d < draws_.size(); ++d) {
      const crt::DrawOwner& owner = assigned.draws[d];
      if (owner.car < 0 || !draws_[d].has_world) {
        continue;
      }
      const uint32_t car_index = index_of[size_t(owner.car)];
      const WalkedCar& car = walked_cars_[car_index];
      if (size_t(owner.geometry) >= car.pose_of_geometry.size() || car.pose_of_geometry[size_t(owner.geometry)] < 0) {
        continue;
      }
      pairs.push_back({car_index, car.pose_of_geometry[size_t(owner.geometry)], d, uint32_t(owner.geometry)});
    }
    // The first pairs of the first calibrating frames, raw (what the log needs to find the right matrices if no hypothesis fits): the draw's origin and first row
    // against each hypothesis'.
    if (pair_samples_logged_ < 12) {
      const auto fmt = [](const tp::Mat4& m) { return std::format("o({:.2f}, {:.2f}, {:.2f}) r0({:.3f}, {:.3f}, {:.3f})", m.m[3][0], m.m[3][1], m.m[3][2], m.m[0][0], m.m[0][1], m.m[0][2]); };
      uint32_t listed = 0;
      for (const Pair& pair : pairs) {
        if (listed >= 4 || pair_samples_logged_ >= 12) {
          break;
        }
        ++listed;
        ++pair_samples_logged_;
        const WalkedCar& car = walked_cars_[pair.car];
        const cw::Geometry& geometry = car.walk->geometries[pair.geometry];
        const crt::PoseSet& set = car.poses[size_t(pair.pose)];
        std::string hypotheses;
        for (uint32_t i = 0; i < crt::kPoseCount; ++i) {
          hypotheses += std::format("{}[{}] {}", hypotheses.empty() ? "" : "; ", i, set.ok[i] ? fmt(set.m[i]) : std::string("-"));
        }
        REXLOG_INFO("{} frame {}: pair sample: vehicle {:#010x}{} part {} bone {}: the draw {} | {} | the entity {}", kProductionPrefix, frame_, car.vehicle, car.is_player ? " (the player's)" : "",
                    geometry.part, geometry.bone, fmt(draws_[pair.draw].world), hypotheses, car.walk->has_entity_matrix ? fmt(car.walk->entity_matrix) : std::string("not read"));
      }
    }
    for (const Pair& pair : pairs) {
      calibration_.Add(walked_cars_[pair.car].poses[size_t(pair.pose)], draws_[pair.draw].world);
    }
    const bool was_decided = calibration_.decided();
    const crt::Pose was = calibration_.chosen();
    const bool decided = calibration_.Decide();
    last_calibration_frame_ = frame_;
    if (decided && (!was_decided || was != calibration_.chosen())) {
      REXLOG_INFO("{} frame {}: the cars' part matrices are placed by '{}' ({}; {} pairs of a walked geometry and the draw of it compared). All hypotheses: {}", kProductionPrefix,
                  frame_, crt::PoseName(calibration_.chosen()), calibration_.fits() ? "it fits the draws" : "NO HYPOTHESIS FITS THE DRAWS: the placements are not right", calibration_.samples(),
                  calibration_.Describe());
    }
    if (calibration_.samples() > kCalibrationWindowPairs) {
      REXLOG_INFO("{} frame {}: calibration window over: {}", kProductionPrefix, frame_, calibration_.Describe());
      calibration_.NewWindow();
    }
  }

  // The wheels' and the traffic's placements against the draws (RT-MODEL-PARTS): in the calibrating frames, for the cars the camera draws.
  if (calibrating_) {
    ResolveParts(usable, index_of, assigned, view, flip_z, &report);
  }

  // THE PROOF in the top level's own space: the chosen pose's instance of each matched geometry against the draw's instance, both through the frame's view. Not before the
  // calibration has chosen: until then the pose in force is the default, which is a hypothesis nobody has checked (the first check run's 1151 m line at frame 6753, logged
  // under the wrong hypothesis one frame before the calibration decided, failed the check for a placement that was exact from the next frame on).
  if (!calibration_.decided()) {
    return report;
  }
  const crt::Pose chosen = pose();
  double sum = 0.0;
  std::unordered_set<uint32_t> cars_with_pairs;
  for (const Pair& pair : pairs) {
    const crt::PoseSet& set = walked_cars_[pair.car].poses[size_t(pair.pose)];
    crt::PlacementError e;
    if (!set.ok[size_t(chosen)] || !crt::ViewError(set.m[size_t(chosen)], draws_[pair.draw].world, view, flip_z, &e)) {
      continue;
    }
    ++report.pairs;
    sum += e.translation;
    report.worst = std::max(report.worst, e.translation);
    report.basis_worst = std::max(report.basis_worst, e.basis);
    cars_with_pairs.insert(pair.car);
    report.player_included = report.player_included || walked_cars_[pair.car].is_player;
  }
  report.ran = report.pairs != 0;
  report.cars = uint32_t(cars_with_pairs.size());
  report.mean = report.pairs != 0 ? sum / double(report.pairs) : 0.0;
  proof_pairs_total_ += report.pairs;
  if (report.ran && (proof_logs_ < 3 || frame_ >= proof_log_frame_ + kProofLogFrames)) {
    ++proof_logs_;
    proof_log_frame_ = frame_;
    REXLOG_INFO("{} frame {}: PLACEMENT PROOF (a car the camera draws: the walk's placement of each geometry, through the frame's view, against the instance of the draw "
                "of it): {} geometries of {} cars{}, '{}': mean error {:.4f} m, worst {:.4f} m (basis {:.4f}) in the top level's view space",
                kProductionPrefix, frame_, report.pairs, report.cars, report.player_included ? " (the player's among them)" : "", crt::PoseName(chosen), report.mean, report.worst,
                report.basis_worst);
  }
  return report;
}

// The wheels' and the traffic's hypotheses against the draws of the frame (the frames that calibrate; RT-MODEL-PARTS). A pair is a walked geometry the draw's buffers name and
// the draw's own model-to-world: for a wheel geometry the best of the four wheels' placements under each hypothesis, for a traffic car's geometry its bone's placement.
// The calibration decides per kind (tyre, rim) and for the traffic, and the proof is the chosen hypothesis' placement through the frame's view against the draw's instance.
void Walker::ResolveParts(const std::vector<const CarWalk*>& usable, const std::vector<uint32_t>& index_of, const crt::Assignment& assigned, const temporal::Mat4& view, bool flip_z,
                          ProofReport* report) {
  namespace cmp = car_model_parts;
  struct Pair {
    uint32_t car;       // walked_cars_ index
    uint32_t draw;
    uint32_t geometry;  // into the car's walk
  };
  std::vector<Pair> wheel_pairs, traffic_pairs;
  for (uint32_t d = 0; d < draws_.size(); ++d) {
    const crt::DrawOwner& owner = assigned.draws[d];
    if (owner.car < 0 || owner.geometry < 0 || !draws_[d].has_world) {
      continue;
    }
    const uint32_t car_index = index_of[size_t(owner.car)];
    const WalkedCar& car = walked_cars_[car_index];
    if (!car.walk || size_t(owner.geometry) >= car.walk->geometries.size()) {
      continue;
    }
    const cw::Geometry& geometry = car.walk->geometries[size_t(owner.geometry)];
    if (car.traffic) {
      if (car.pose_read && car.traffic_pose.Usable()) {
        traffic_pairs.push_back({car_index, d, uint32_t(owner.geometry)});
      }
    } else if (cw::IsWheelPart(geometry.part) && car.wheels_read) {
      wheel_pairs.push_back({car_index, d, uint32_t(owner.geometry)});
    }
  }
  (void)usable;

  // ---- the wheels
  for (const Pair& pair : wheel_pairs) {
    const WalkedCar& car = walked_cars_[pair.car];
    const cw::Geometry& geometry = car.walk->geometries[pair.geometry];
    wheel_calibration_.Add(cw::IsRimPart(geometry.part), car.wheel_matrices, car.walk->wheels.present, geometry.bone, draws_[pair.draw].world);
  }
  if (!wheel_pairs.empty()) {
    for (const bool rim : {false, true}) {
      const bool was_decided = wheel_calibration_.decided(rim);
      const cw::WheelPose was = wheel_calibration_.chosen(rim);
      if (wheel_calibration_.Decide(rim) && (!was_decided || was != wheel_calibration_.chosen(rim))) {
        REXLOG_INFO("{} frame {}: the wheels' matrices place the {} by '{}' ({}; {} pairs of a walked wheel geometry and the draw of it compared). All hypotheses: {}", kProductionPrefix,
                    frame_, rim ? "rims" : "tyres", cw::WheelPoseName(wheel_calibration_.chosen(rim)),
                    wheel_calibration_.fits(rim) ? "it fits the draws" : "NO HYPOTHESIS FITS THE DRAWS: the wheels of the cars the camera does not draw are not placed",
                    wheel_calibration_.samples(rim), wheel_calibration_.Describe(rim));
      }
    }
    for (const bool rim : {false, true}) {
      if (wheel_calibration_.samples(rim) > kCalibrationWindowPairs) {
        REXLOG_INFO("{} frame {}: the {} calibration window is over: {}", kProductionPrefix, frame_, rim ? "rims'" : "tyres'", wheel_calibration_.Describe(rim));
        wheel_calibration_.NewWindow();
      }
    }
    // the proof, per kind, in the top level's view space
    double sum[2] = {}, worst[2] = {};
    uint32_t n[2] = {};
    for (const Pair& pair : wheel_pairs) {
      const WalkedCar& car = walked_cars_[pair.car];
      const cw::Geometry& geometry = car.walk->geometries[pair.geometry];
      const bool rim = cw::IsRimPart(geometry.part);
      if (!wheel_calibration_.decided(rim)) {
        continue;
      }
      tp::Mat4 placed;
      crt::PlacementError e;
      if (!cmp::BestWheelPlacement(car.wheel_matrices, car.walk->wheels.present, geometry.bone, wheel_calibration_.chosen(rim), draws_[pair.draw].world, &placed) ||
          !crt::ViewError(placed, draws_[pair.draw].world, view, flip_z, &e)) {
        continue;
      }
      const int k = rim ? 1 : 0;
      ++n[k];
      sum[k] += e.translation;
      worst[k] = std::max(worst[k], e.translation);
    }
    for (int k = 0; k < 2; ++k) {
      report->wheel_pairs[k] = n[k];
      report->wheel_mean[k] = n[k] != 0 ? sum[k] / double(n[k]) : 0.0;
      report->wheel_worst[k] = worst[k];
    }
    if ((n[0] + n[1]) != 0 && (wheel_proof_logs_ < 3 || frame_ >= wheel_proof_log_frame_ + kProofLogFrames)) {
      ++wheel_proof_logs_;
      wheel_proof_log_frame_ = frame_;
      REXLOG_INFO("{} frame {}: WHEEL PLACEMENT PROOF (a car the camera draws: the walk's wheel placement of each wheel geometry, the nearest of the four wheels, through the frame's view, "
                  "against the instance of the draw of it): tyres {} geometries '{}': mean error {:.4f} m, worst {:.4f} m; rims {} geometries '{}': mean error {:.4f} m, worst {:.4f} m in the top "
                  "level's view space",
                  kProductionPrefix, frame_, n[0], cw::WheelPoseName(wheel_calibration_.chosen(false)), report->wheel_mean[0], report->wheel_worst[0], n[1],
                  cw::WheelPoseName(wheel_calibration_.chosen(true)), report->wheel_mean[1], report->wheel_worst[1]);
    }
  }

  // ---- the traffic
  for (const Pair& pair : traffic_pairs) {
    const WalkedCar& car = walked_cars_[pair.car];
    traffic_calibration_.Add(car.traffic_pose, car.walk->geometries[pair.geometry].bone, draws_[pair.draw].world);
  }
  if (!traffic_pairs.empty()) {
    const bool was_decided = traffic_calibration_.decided();
    const cw::TrafficPose was = traffic_calibration_.chosen();
    if (traffic_calibration_.Decide() && (!was_decided || was != traffic_calibration_.chosen())) {
      REXLOG_INFO("{} frame {}: the traffic cars' bone matrices are placed by '{}' ({}; {} pairs of a walked geometry of a traffic car and the draw of it compared). All hypotheses: {}",
                  kProductionPrefix, frame_, cw::TrafficPoseName(traffic_calibration_.chosen()),
                  traffic_calibration_.fits() ? "it fits the draws" : "NO HYPOTHESIS FITS THE DRAWS: the traffic cars the camera does not draw are not placed", traffic_calibration_.samples(),
                  traffic_calibration_.Describe());
    }
    if (traffic_calibration_.samples() > kCalibrationWindowPairs) {
      REXLOG_INFO("{} frame {}: the traffic calibration window is over: {}", kProductionPrefix, frame_, traffic_calibration_.Describe());
      traffic_calibration_.NewWindow();
    }
    if (traffic_calibration_.decided()) {
      double sum = 0.0;
      uint32_t n = 0;
      std::unordered_set<uint32_t> cars;
      for (const Pair& pair : traffic_pairs) {
        const WalkedCar& car = walked_cars_[pair.car];
        tp::Mat4 placed;
        crt::PlacementError e;
        if (!cw::PlaceTraffic(car.traffic_pose, car.walk->geometries[pair.geometry].bone, traffic_calibration_.chosen(), &placed) ||
            !crt::ViewError(placed, draws_[pair.draw].world, view, flip_z, &e)) {
          continue;
        }
        ++n;
        sum += e.translation;
        report->traffic_worst = std::max(report->traffic_worst, e.translation);
        cars.insert(car.vehicle);
      }
      report->traffic_pairs = n;
      report->traffic_mean = n != 0 ? sum / double(n) : 0.0;
      if (n != 0 && (traffic_proof_logs_ < 3 || frame_ >= traffic_proof_log_frame_ + kProofLogFrames)) {
        ++traffic_proof_logs_;
        traffic_proof_log_frame_ = frame_;
        REXLOG_INFO("{} frame {}: TRAFFIC PLACEMENT PROOF (a traffic car the camera draws: the walk's placement of each geometry, through the frame's view, against the instance of the draw "
                    "of it): {} geometries of {} cars, '{}': mean error {:.4f} m, worst {:.4f} m in the top level's view space",
                    kProductionPrefix, frame_, n, cars.size(), cw::TrafficPoseName(traffic_calibration_.chosen()), report->traffic_mean, report->traffic_worst);
      }
    }
  }
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
  // Which car each draw is of: the cars of one model name the same buffers (two Murcielagos: the one beside the player and a racer 56 m away that the frame does not
  // draw), so the buffers alone would credit the near car's draws to both and the far one would look drawn with a different part list (car_model_rt_rules.h AssignDraws).
  std::vector<std::array<double, 3>> usable_positions;
  for (const CarWalk* walk : usable) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    usable_positions.push_back(walk->position_read ? std::array<double, 3>{walk->position[0], walk->position[1], walk->position[2]} : std::array<double, 3>{nan, nan, nan});
  }
  const car_model_rt::Assignment owners = car_model_rt::AssignDraws(usable, usable_positions, draws_);
  std::vector<int32_t> owner_of_draw(draws_.size(), -1);
  for (size_t d = 0; d < owners.draws.size() && d < owner_of_draw.size(); ++d) {
    owner_of_draw[d] = owners.draws[d].car;
  }
  const FrameMatch frame = MatchFrame(usable, draws_, &owner_of_draw);

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
  uint32_t traffic_walked = 0;
  for (const Found& f : found_) {
    traffic_walked += (f.sources & kFromItems) ? 1u : 0u;
  }
  REXLOG_INFO("{} frame {}: HEADER walked {} vehicles in {:.0f} us ({} are cars with a model set, {} are not; {} drawn, {} culled); sources: player global {:#010x}, "
              "renderer client {} (slot 7: {}, mask {:#010x}, {} types, {} instances), tap {} vehicles ({} calls so far, {} dropped); draws noted {} ({} wheel proxies, {} with no "
              "buffer key, {} dropped); matched {}, vertex-only {}, unowned {}; {:.1f} MB of vertex and index data read{}",
              kPrefix, frame_, cars_.size(), walk_microseconds_, cars_ok, failed, drawn, culled, player_vehicle_, list_.renderer ? (list_.client ? "found" : "slot empty") : "NOT READABLE",
              kVehicleClientSlot, list_.client_mask, list_.types, list_.instances, tap_vehicles_, tap_calls_total_, tap_dropped_, draws_.size(), draws_proxy_, draws_no_key_,
              draws_dropped_, frame.draws_matched, frame.draws_vertex_only, frame.draws_unowned, double(bytes) / (1024.0 * 1024.0),
              note_.empty() ? std::string() : " [" + note_ + "]");
  {
    std::string reasons;
    for (uint32_t f = 1; f < cw::kAmbientFailCount; ++f) {
      if (list_.ambient_fail[f] != 0) {
        reasons += std::format("{}{} {}", reasons.empty() ? "" : "; ", list_.ambient_fail[f], cw::AmbientFailName(cw::AmbientFail(f)));
      }
    }
    REXLOG_INFO("{} frame {}: TRAFFIC ambient client {} (renderer {}), {} model nodes ({} flat items, {} foreign nodes), {} render items under them{} ({} whose model word is another node), "
                "{} of them with no readable car{}, {} walked here (the nearest within {:.0f} m of the player; kind=traffic in the RESULT lines)",
                kPrefix, frame_, list_.ambient_client ? "found" : "NOT FOUND", list_.ambient_renderer ? "readable" : "NOT READABLE", list_.ambient_models, list_.ambient_flat,
                list_.ambient_foreign, list_.ambient_items, list_.ambient_truncated ? " (a cap was hit)" : "", list_.ambient_mismatched, list_.ambient_unreadable,
                reasons.empty() ? std::string() : " [" + reasons + "]", traffic_walked, kTrafficWalkMetres);
    for (const std::string& sample : list_.ambient_samples) {
      REXLOG_INFO("{} frame {}:     unreadable {}", kPrefix, frame_, sample);
    }
  }

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
    const bool is_item = (found.sources & kFromItems) != 0;
    const char* kind = is_player ? "player" : is_item ? "traffic" : (racer.known && racer.distance < kRacerMetres) ? "racer" : "other";
    // The wheels (RT-MODEL-PARTS) are told apart: the percentage and the counts of the earlier checks are of the model set's geometries (and a traffic car's), the wheels' own are the last
    // two fields of the line (`wheels_in_use`, `wheels_matched`), so that a wheel draw the walk does not know cannot make a drawn car look badly walked.
    std::unordered_set<uint32_t> hit_geometries;
    for (const DrawMatch& d : frame.draws) {
      if (d.hit == DrawHit::kGeometry && d.car == u) {
        hit_geometries.insert(d.geometry);
      }
    }
    uint32_t wheels_in_use = 0, wheels_matched = 0;
    for (uint32_t g = 0; g < car.geometries.size(); ++g) {
      const Geometry& geometry = car.geometries[g];
      if (IsWheelPart(geometry.part) && geometry.in_use && geometry.index_count != 0) {
        ++wheels_in_use;
        wheels_matched += hit_geometries.count(g) != 0 ? 1u : 0u;
      }
    }
    const uint32_t in_use_all = match.in_use - match.empty_in_use;
    const uint32_t body_geometry = in_use_all >= wheels_in_use ? in_use_all - wheels_in_use : 0u;
    const uint32_t matched_body = match.matched_in_use >= wheels_matched ? match.matched_in_use - wheels_matched : 0u;
    const double percent = body_geometry != 0 ? 100.0 * double(matched_body) / double(body_geometry) : 0.0;
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
                "cnt={} stride={} posoff={} hdr_data={} lodmatch={}/{}/{} lodwalk={}/{}/{} lods={}/{}/{} loop={} flags={}{}{}{}{}{}{} visible={} wheels_in_use={} wheels_matched={}",
                kPrefix, frame_, car.vehicle, kind, match.drawn ? 1 : 0, car.all.geometries, car.in_use.geometries, car.in_use.triangles, match.matched,
                matched_body, percent, match.matched_not_in_use, match.walked_only_in_use, match.drawn_only, match.vertex_only, match.draws, car.resident_in_use,
                car.validated_in_use, car.entries_present, car.entries_loaded, car.entries_valid, in_use_invalid, match.exact_words, match.index_object_equal,
                match.index_address_equal, match.index_count_equal, match.stride_equal, match.position_offset_equal, header_data, match.matched_by_lod[0], match.matched_by_lod[1],
                match.matched_by_lod[2], match.walked_by_lod[0], match.walked_by_lod[1], match.walked_by_lod[2], car.body_lod, car.part_lod, car.forced_lod, car.loop_parts,
                car.flags.ghost ? "G" : "-", car.flags.loop44_a ? "A" : "-", car.flags.loop44_b ? "B" : "-", car.flags.bumper_front_off ? "F" : "-", car.flags.bumper_rear_off ? "R" : "-",
                car.flags.sub_bumpers ? "S" : "-", car.flags.intercooler ? "I" : "-", car.visible, wheels_in_use, wheels_matched);

    REXLOG_INFO("{} frame {}:   car {:#010x} ({}, {}) at ({:.1f}, {:.1f}, {:.1f}), {:.1f} m from the player{}: {}; seen by {} (tap phases {}); visible +116 = {}, enabled {}, "
                "lod body {} parts {} forced {} (lod distances {:.0f} / {:.0f} m), draw slot {}; entity state {}, body state {}, lod 0 {} (info {}); "
                "entity {:#010x}, model info {:#010x}, model set {:#010x}, customisation {:#010x}; vtable {:#010x}{}; render copy of the matrix at ({:.1f}, {:.1f}, {:.1f}), wheels {}",
                kPrefix, frame_, car.vehicle, kind, racer.known ? std::format("nearest racer {:.2f} m", racer.distance) : std::string("no racer near"), car.position[0], car.position[1],
                car.position[2], player_distance, is_player ? " (the player's)" : "", match.drawn ? "DRAWN this frame" : "CULLED (no draw of the frame is one of its geometries)",
                SourcesText(found.sources), PhasesText(found.phases), car.visible, car.enabled, car.body_lod, car.part_lod, car.forced_lod, car.lod_distance[0],
                car.lod_distance[1], car.draw_slot, car.entity_state, car.body_state, car.has_lod0 ? "present" : "absent", car.info_lod0 ? "present" : "absent", car.entity,
                car.info, car.model_set, car.custom, found.vtable, found.vtable == kVehicleVtable ? " (mcCarModel)" : " (NOT mcCarModel's)", car.render_position[0], car.render_position[1],
                car.render_position[2],
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
    LogParts(car, found, u, i, frame);
    std::string ids_text;
    for (const uint32_t id : match.ids) {
      ids_text += std::format("{}{}", ids_text.empty() ? "" : ",", id);
    }
    if (match.drawn) {
      REXLOG_INFO("{} frame {}:     comparison: {} of {} in-use geometries are draws of the frame ({:.1f}%), {} geometries not in use are drawn too, {} in use have no draw (walked-only; "
                  "{} of the in-use ones have no indices), {} draws of its IDs are none of its geometries (drawn-only, {} of them by the vertex window only); {} draws are "
                  "its geometries, with the stencil IDs {}; the match is exact for {} of {} matched (fetch dwords), index object {}, index address {}, index count {}, stride {}, "
                  "position offset 0 {}; {} of its vertex windows are another car's too",
                  kPrefix, frame_, matched_body, body_geometry, percent, match.matched_not_in_use, match.walked_only_in_use, match.empty_in_use, match.drawn_only,
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

// The wheels of a racer and the item and the pose of a traffic car, for the reading of the debug walk (RT-MODEL-PARTS; the lines the check reads: "wheels:" and "traffic item").
void Walker::LogParts(const CarWalk& car, const Found& found, uint32_t car_index, uint32_t found_index, const FrameMatch& frame) {
  if (found_index >= found_.size()) {
    return;
  }
  if ((found.sources & kFromItems) != 0) {
    const cw::AmbientItem& item = ambient_[found_index];
    const cw::TrafficPoseData& pose = traffic_poses_[found_index];
    const cw::AmbientColour& colour = ambient_colour_[found_index];
    std::string pose_text;
    if (!pose.Usable()) {
      pose_text = std::format("the item's bone block was NOT usable (blocks table {:#010x}, buffer {}, block {:#010x})", item.blocks, pose.buffer, pose.block);
    } else {
      const cw::AmbientBone& b0 = pose.bones[0];
      const double at[3] = {pose.root[0] + double(b0.m[0][3]), pose.root[1] + double(b0.m[1][3]), pose.root[2] + double(b0.m[2][3])};
      const double update = Distance(at, item.position);
      std::string render = "no render copy";
      if (pose.car_render_ok) {
        const double rc[3] = {pose.car_render.m[3][0], pose.car_render.m[3][1], pose.car_render.m[3][2]};
        render = std::format("{:.2f} m from its render copy", Distance(at, rc));
      }
      double lengths[3];
      for (int axis = 0; axis < 3; ++axis) {
        lengths[axis] = std::sqrt(double(b0.m[0][axis]) * b0.m[0][axis] + double(b0.m[1][axis]) * b0.m[1][axis] + double(b0.m[2][axis]) * b0.m[2][axis]);
      }
      pose_text = std::format("block {:#010x} (buffer {}), root ({:.2f}, {:.2f}, {:.2f}), {} bones read of the {} the geometries use, bone 0 at root + ({:.2f}, {:.2f}, {:.2f}) (its axes' lengths "
                              "{:.3f} {:.3f} {:.3f}); root + bone 0 is {:.2f} m from the car's update-copy position and {}",
                              pose.block, pose.buffer, pose.root[0], pose.root[1], pose.root[2], pose.bones.size(), cw::BonesNeeded(car), b0.m[0][3], b0.m[1][3], b0.m[2][3], lengths[0], lengths[1],
                              lengths[2], update, render);
    }
    uint32_t models = 0;
    for (const PartEntry& e : car.parts) {
      models += e.models;
    }
    REXLOG_INFO("{} frame {}:     traffic item {:#010x}: car {:#010x} (type {:#010x}, radius {:.2f} m, flags {:#04x}), model {:#010x}, item lod {} ({}), enabled {}, view depth + radius {:.1f}; "
                "lod taken {} ({} models, {} geometries, {} triangles, {} bones used); colour index {} -> palette {} ; pose: {}",
                kPrefix, frame_, item.item, item.car, item.type, item.radius, item.car_flags, item.model, item.lod, item.Visible() ? "the main view shows it" : "outside the main view",
                item.enabled ? 1 : 0, item.depth, car.body_lod, models, car.in_use.geometries, car.in_use.triangles, cw::BonesNeeded(car), item.colour_index,
                colour.read ? std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f}) x {:.3f}", colour.rgba[0], colour.rgba[1], colour.rgba[2], colour.rgba[3], colour.scale) : std::string("not readable"),
                pose_text);
    return;
  }

  // a racer: the wheels
  const cw::WheelWalk& w = car.wheels;
  if (!w.read) {
    REXLOG_INFO("{} frame {}:     wheels: not read (the wheel manager [{:#010x}] or the vehicle's ids were not readable)", kPrefix, frame_, cw::kWheelManagerGlobal);
    return;
  }
  uint32_t tyre_geoms = 0, rim_geoms = 0, tyre_tris = 0, rim_tris = 0, in_use = 0, bones = 0;
  for (const Geometry& g : car.geometries) {
    if (g.part == kTyrePart) {
      ++tyre_geoms;
      tyre_tris += g.index_count / 3;
    } else if (g.part == kRimPart) {
      ++rim_geoms;
      rim_tris += g.index_count / 3;
    } else {
      continue;
    }
    in_use += g.in_use;
    if (g.bone != kNoBone) {
      bones = std::max(bones, g.bone + 1);
    }
  }
  std::unordered_set<uint32_t> matched;
  for (const DrawMatch& d : frame.draws) {
    if (d.hit == DrawHit::kGeometry && d.car == car_index && d.geometry < car.geometries.size() && IsWheelPart(car.geometries[d.geometry].part)) {
      matched.insert(d.geometry);
    }
  }
  const cw::WheelMatrices& m = wheel_mats_[found_index];
  std::string wheels_text;
  for (uint32_t i = 0; i < kWheelCount; ++i) {
    const auto at = [](bool ok, const temporal::Mat4& x) { return ok ? std::format("({:.2f}, {:.2f}, {:.2f})", x.m[3][0], x.m[3][1], x.m[3][2]) : std::string("-"); };
    double tyre_rim = -1.0;
    if (m.tyre_ok[i] && m.rim_ok[i]) {
      tyre_rim = Distance(m.tyre[i].m[3], m.rim[i].m[3]);
    }
    wheels_text += std::format("{}w{}: tyre {} rim {} brake1 {} brake2 {} (tyre to rim {:.2f} m)", wheels_text.empty() ? "" : "; ", i, at(m.tyre_ok[i], m.tyre[i]), at(m.rim_ok[i], m.rim[i]),
                               at(m.aux1_ok[i], m.aux1[i]), at(m.aux2_ok[i], m.aux2[i]), tyre_rim);
  }
  REXLOG_INFO("{} frame {}:     wheels: tyre id {} ({}; entry {:#010x}, drawable {:#010x}, {} geometries, {} triangles), rim id {} ({}; entry {:#010x}, drawable {:#010x}, {} geometries, {} triangles, "
              "{} bones used by the wheel resources); the wheel draw's flags: tyre {}, rim {}, hidden {}; wheels the vehicle draws {}{}{}{}; {} wheel geometries in use of {}, {} of them are "
              "draws of the frame; the vehicle's matrices (V+1344 tyre, V+1600 rim, V+1856 / V+1920 brake): {}",
              kPrefix, frame_, w.tyre_id, w.tyre_ok ? "the accessor hands it out" : "NOT handed out", w.tyre_entry, w.tyre_drawable, tyre_geoms, tyre_tris, w.rim_id,
              w.rim_ok ? "the accessor hands it out" : "NOT handed out", w.rim_entry, w.rim_drawable, rim_geoms, rim_tris, bones, w.tyre_on ? "on" : "off", w.rim_on ? "on" : "off",
              w.hidden ? "YES" : "no", w.present[0] ? 1 : 0, w.present[1] ? 1 : 0, w.present[2] ? 1 : 0, w.present[3] ? 1 : 0, in_use, tyre_geoms + rim_geoms, matched.size(), wheels_text);
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
