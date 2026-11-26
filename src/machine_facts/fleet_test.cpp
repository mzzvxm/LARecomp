// The test fleet for the launcher-follows-the-game cross-check: fake machine.json files, one per kind of PC the launcher has to render for.
//
//   clang++ -std=c++23 -Isrc src/machine_facts/fleet_test.cpp src/machine_facts/document.cpp src/os/file_replace.cpp  -o fleet_test
//   fleet_test <folder>        writes <folder>/<id>.machine.json for every machine below and checks what it can
//
// Every file is made by the game's own writer (machine::Serialize) from the game's own rules for the two things a table decides: the GPU class (gpu_class.h
// Classify: memory, the speed table, shared memory, the handheld table) and the ray tracing tier (rt_tier.h ClassifyRt, RtRung). What a card can run
// (FSR 4, DLSS, frame generation, the latency reducer, the denoiser) is stated per machine below, in the words of the contract (what the game's probes
// would say on that card). The files land in documentation/contract/examples/fleet/; the launcher's tests read them from there.
//
// The fleet (LAUNCHER-FOLLOWS-GAME section 6, XT): RTX 5080 (Direct3D 12 and Vulkan), RTX 4070 (no multi frame generation; and with DLSS modes refused), RTX 3060 (no DLSS frame
// generation), RX 9070 XT, RX 7800 XT (no FSR 4), Arc A770, the 9800X3D's 2-CU iGPU (class low), Steam Deck (handheld, Vulkan, Linux), GTX 1080 (no ray
// tracing units), and a PC whose probe failed (what `--probe_machine` leaves when the device never opened: the first-pass facts only).

#include "document.h"

#include "gpu/gpu_class.h"
#include "gpu/rt_tier.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace machine = larecomp::machine;
namespace gc = mcla::gpu::gpu_class;

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::cerr << "FAIL " << what << '\n';
  }
}

struct Spec {
  const char* id;
  const char* adapter;          // the name as the API prints it
  uint32_t vendor_id;
  uint32_t device_id;
  const char* driver;
  uint64_t vram_mb;
  bool uma = false;             // shares memory with the CPU
  bool integrated = false;
  const char* api = "d3d12";    // d3d12 | vulkan
  bool api_rt = true;           // DXR 1.1 / ray query exist on this device
  const char* os = "windows";   // windows | linux
  // What the card can run.
  bool rtx = false, rx9000 = false, fsr4 = false;
  const char* dlss = "no";      // no | ready | needs_setup | after_restart
  std::vector<std::string> refused_modes;   // DLSS modes the driver refuses on this card (their rows say N/A)
  bool fsr_fg = true, dlss_fg = false;
  unsigned dlss_multiplier = 0;
  const char* latency = "none";   // none | reflex | antilag2
  const char* denoiser = "none";  // nvidia | amd | none
  const char* dlss_rr = "no";
  const char* ray_regen = "no";
  // The screen.
  const char* display = "Primary display";
  uint32_t width = 1920, height = 1080;
  float hz = 60;
  bool hdr = false;
  // Expectations that follow from the game's rules (checked, not just written).
  const char* want_class = nullptr;
  const char* want_tier = nullptr;
  bool want_handheld = false;
  bool probe_failed = false;
};

const std::vector<Spec>& Fleet() {
  static const std::vector<Spec> fleet = [] {
    std::vector<Spec> f;
    {
      Spec s{"rtx5080-d3d12", "NVIDIA GeForce RTX 5080", 0x10DE, 0x2C02, "32.0.15.6094", 16303};
      s.rtx = true; s.dlss = "ready"; s.dlss_fg = true; s.dlss_multiplier = 4; s.latency = "reflex"; s.denoiser = "nvidia"; s.dlss_rr = "ready";
      s.display = "Primary display"; s.width = 3840; s.height = 2160; s.hz = 240; s.hdr = true; s.want_class = "high"; s.want_tier = "strong";
      f.push_back(s);
    }
    {
      Spec s{"rtx5080-vulkan", "NVIDIA GeForce RTX 5080", 0x10DE, 0x2C02, "32.0.15.6094", 16303, false, false, "vulkan"};
      s.rtx = true; s.dlss = "ready"; s.dlss_fg = true; s.dlss_multiplier = 4; s.latency = "reflex"; s.denoiser = "nvidia"; s.dlss_rr = "ready";
      s.width = 2560; s.height = 1440; s.hz = 165; s.want_class = "high"; s.want_tier = "strong";
      f.push_back(s);
    }
    {
      Spec s{"rtx4070", "NVIDIA GeForce RTX 4070", 0x10DE, 0x2786, "32.0.15.5612", 12282};
      s.rtx = true; s.dlss = "ready"; s.dlss_fg = true; s.dlss_multiplier = 2; s.latency = "reflex"; s.denoiser = "nvidia"; s.dlss_rr = "ready";
      s.width = 2560; s.height = 1440; s.hz = 144; s.want_class = "high"; s.want_tier = "strong";
      f.push_back(s);
    }
    {
      // The same card with the driver refusing two DLSS modes (the rows say N/A) and DLSS not yet restarted into: what a first start on a new driver looks like.
      Spec s{"rtx4070-dlss-refused", "NVIDIA GeForce RTX 4070", 0x10DE, 0x2786, "32.0.15.5612", 12282};
      s.rtx = true; s.dlss = "after_restart"; s.dlss_fg = false; s.latency = "reflex"; s.denoiser = "nvidia"; s.dlss_rr = "after_restart";
      s.refused_modes = {"performance", "ultra_performance"}; s.want_class = "high"; s.want_tier = "strong";
      f.push_back(s);
    }
    {
      Spec s{"rtx3060", "NVIDIA GeForce RTX 3060", 0x10DE, 0x2503, "32.0.15.5612", 12288};
      s.rtx = true; s.dlss = "ready"; s.dlss_fg = false; s.latency = "reflex"; s.denoiser = "nvidia"; s.dlss_rr = "ready";
      s.want_tier = "weak";
      f.push_back(s);
    }
    {
      Spec s{"rx9070xt", "AMD Radeon RX 9070 XT", 0x1002, 0x7550, "32.0.21013.1000", 16304};
      s.rx9000 = true; s.fsr4 = true; s.latency = "antilag2"; s.denoiser = "amd"; s.ray_regen = "ready";
      s.width = 3840; s.height = 2160; s.hz = 144; s.hdr = true; s.want_class = "high"; s.want_tier = "strong";
      f.push_back(s);
    }
    {
      Spec s{"rx7800xt", "AMD Radeon RX 7800 XT", 0x1002, 0x747E, "32.0.21013.1000", 16304};
      s.latency = "antilag2"; s.denoiser = "none"; s.ray_regen = "unsupported";
      s.width = 2560; s.height = 1440; s.hz = 144; s.want_tier = "weak";   // the name says strong (RX 7800 XT and up), the chip says weak (Navi 32): the lower stands
      f.push_back(s);
    }
    {
      Spec s{"arc-a770", "Intel(R) Arc(TM) A770 Graphics", 0x8086, 0x56A0, "32.0.101.6130", 16256};
      s.want_tier = "weak";
      f.push_back(s);
    }
    {
      // The 9800X3D's own graphics: 2 compute units of RDNA 2 on the processor, sharing system memory.
      Spec s{"igpu-2cu", "AMD Radeon(TM) Graphics", 0x1002, 0x164E, "32.0.21013.1000", 512, true};
      s.latency = "none"; s.want_class = "low"; s.want_tier = "weak";
      f.push_back(s);
    }
    {
      Spec s{"steam-deck", "AMD Custom GPU 0405 (RADV VANGOGH)", 0x1002, 0x163F, "25.2.0", 1024, true, true, "vulkan", true, "linux"};
      s.display = "Embedded display"; s.width = 1280; s.height = 800; s.hz = 60;
      s.want_class = "low"; s.want_tier = "weak"; s.want_handheld = true;
      f.push_back(s);
    }
    {
      Spec s{"gtx1080", "NVIDIA GeForce GTX 1080", 0x10DE, 0x1B80, "31.0.15.4601", 8192, false, false, "vulkan", false};
      s.dlss = "no"; s.latency = "reflex"; s.want_tier = "none";
      f.push_back(s);
    }
    {
      // The device never opened: `--probe_machine` wrote only what the first pass sees (adapter, display, OS); every slow fact is null.
      Spec s{"failed-probe", "NVIDIA GeForce RTX 4070", 0x10DE, 0x2786, "32.0.15.5612", 12282};
      s.probe_failed = true;
      f.push_back(s);
    }
    return f;
  }();
  return fleet;
}

machine::Facts MakeFacts(const Spec& s, std::string* class_name, std::string* tier_name) {
  machine::Facts facts;
  const bool linux_os = std::string(s.os) == "linux";
  const std::string home = linux_os ? "/home/player" : "C:/Users/player";
  const std::string game = linux_os ? "/home/player/LARecomp" : std::string("C:/Games/Fleet/") + s.id;
  facts.os = s.os;
  facts.os_build = linux_os ? 0 : 26300;
  facts.cpu_threads = linux_os ? 8 : 16;
  facts.cpu_level = linux_os ? "x86-64-v3" : "x86-64-v3";
  facts.game_version = "PATCH 11 - PREVIEW 1";
  facts.commit = std::string(40, 'f');
  facts.min_launcher = "1.0.0";  // the committed examples are snapshots: they carry the recipe example's value
  facts.title_id = "545407F8";
  facts.profile_id = "B13EBABEBABEBABE";
  facts.game_folder = game;
  facts.config_file = game + "/larecomp.toml";
  facts.user_root = home + "/Documents/LARecomp";
  facts.saves_root = facts.user_root / "content";
  facts.cache_root = facts.user_root / "cache";
  facts.logs = game + "/logs";
  facts.game_data_root = linux_os ? "/home/player/MCLA_Game_Files" : "C:/Games/Fleet/MCLA_Game_Files";
  facts.mod_root = game + "/modloader";
  facts.strings_replace = game + "/strings/replace";
  facts.music = {std::filesystem::path(game + "/music")};
  facts.displays = {{s.display, s.width, s.height, s.hz, s.hdr}};
  facts.windowed_sizes = {{1280, 720}, {1920, 1080}};
  if (s.width >= 2560) facts.windowed_sizes.push_back({2560, 1440});
  facts.exclusive_sizes = {{s.width, s.height, uint32_t(s.hz)}};
  facts.exclusive_fullscreen = !linux_os;
  facts.live_window_resize = true;
  facts.hdr_output = !linux_os;
  facts.real_weather = true;
  facts.mouse_look = true;
  facts.wheel_ffb = !linux_os;
  facts.frame_pacing_wake = !linux_os;

  machine::GpuFacts& gpu = facts.gpu;
  gpu.d3d12_compiled = !linux_os;
  gpu.vulkan_compiled = true;
  gpu.ray_regen_built = !linux_os;
  machine::Adapter adapter;
  adapter.name = s.adapter;
  adapter.vendor_id = s.vendor_id;
  adapter.device_id = s.device_id;
  adapter.driver_version = s.driver;
  adapter.vram_mb = s.vram_mb;
  gc::AdapterFacts af;
  af.uma = s.uma;
  af.integrated = s.integrated;
  af.dedicated_bytes = s.uma ? 0 : s.vram_mb << 20;
  af.raytracing = s.api_rt;
  af.name = s.adapter;
  af.vendor_id = s.vendor_id;
  const gc::Verdict verdict = gc::Classify(af);
  gc::RtFacts rf;
  rf.api_rt = s.api_rt;
  rf.api_reason = s.api_rt ? "" : "no ray query on this device";
  rf.uma = s.uma;
  rf.integrated = s.integrated;
  rf.vendor_id = s.vendor_id;
  rf.device_id = s.device_id;
  rf.name = s.adapter;
  const gc::RtVerdict rt = gc::ClassifyRt(rf);
  const gc::RtTier rung = gc::RtRung(verdict.cls, rt.tier);
  *class_name = std::string(gc::ClassName(verdict.cls));
  *tier_name = std::string(gc::RtTierName(rt.tier));

  adapter.gpu_class = *class_name;
  adapter.shared_memory = s.uma;
  adapter.class_why = verdict.why;
  gpu.adapters = {adapter};
  // A second adapter on the two PCs that have one: the owner's (an RTX card with the processor's graphics beside it).
  if (std::string(s.id).rfind("rtx5080", 0) == 0) {
    machine::Adapter igpu;
    igpu.name = "AMD Radeon(TM) Graphics";
    igpu.vendor_id = 0x1002;
    igpu.device_id = 0x164E;
    igpu.driver_version = "32.0.21013.1000";
    igpu.vram_mb = 2048;
    igpu.gpu_class = "low";
    igpu.shared_memory = true;
    igpu.class_why = "by adapter type: UMA";
    gpu.adapters.insert(gpu.adapters.begin(), igpu);
    gpu.selected = 1;
  } else {
    gpu.selected = 0;
  }

  if (s.probe_failed) {
    // Only the adapter (name, ids, driver, memory) and the screen: everything the device would say is null, the document is not final.
    gpu.adapters[0].gpu_class.reset();
    gpu.adapters[0].shared_memory.reset();
    gpu.adapters[0].class_why.clear();
    gpu.pending = true;
    facts.exclusive_sizes.clear();
    return facts;
  }

  gpu.rtx = s.rtx;
  gpu.rx9000 = s.rx9000;
  gpu.handheld = verdict.handheld;
  gpu.rt_benchmarked = false;
  gpu.rt_tier = *tier_name;
  gpu.rt_rung = std::string(gc::RtTierName(rung));
  gpu.rt_tier_why = rt.why;
  gpu.api = s.api;
  gpu.api_version = std::string(s.api) == "d3d12" ? "12_2" : "1.3";
  gpu.shader_model = std::string(s.api) == "d3d12" ? "6.7" : "";
  gpu.dxr_1_1 = std::string(s.api) == "d3d12" ? s.api_rt : false;
  gpu.ray_query = s.api_rt;
  gpu.car_rt = s.api_rt;
  gpu.city_rt_shadows = s.api_rt && rt.tier != gc::RtTier::kNone;
  gpu.fsr1 = true;
  gpu.fsr31 = true;
  gpu.fsr4 = s.fsr4;
  gpu.dlss = s.dlss;
  gpu.refused_modes = s.refused_modes;
  gpu.fsr31_fg = s.fsr_fg;
  gpu.dlss_fg = s.dlss_fg;
  gpu.dlss_multiplier = s.dlss_multiplier;
  gpu.dlss_after_restart = false;
  gpu.latency = std::string(s.latency);
  gpu.latency_auto = std::string(s.latency) != "none";
  gpu.denoiser = std::string(s.denoiser);
  gpu.dlss_rr = std::string(s.dlss_rr);
  gpu.ray_regen = std::string(s.ray_regen);
  gpu.paper_white = s.hdr ? 240 : 203;
  gpu.peak_nits = s.hdr ? 1000 : 400;
  gpu.pcss = true;
  gpu.gi_update = true;
  gpu.fidelityfx_files = true;
  gpu.streamline_files = s.vendor_id == 0x10DE;
  facts.hdr_capable = s.hdr;
  facts.portuguese_text = false;
  facts.city_pack = "current";
  facts.city_hit = "current";
  facts.shader_pack = "current";
  return facts;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: fleet_test <folder>\n";
    return 2;
  }
  const std::filesystem::path folder = argv[1];
  std::filesystem::create_directories(folder);
  for (const Spec& s : Fleet()) {
    std::string cls, tier;
    machine::Facts facts = MakeFacts(s, &cls, &tier);
    const bool probe = s.probe_failed;
    const std::string text = machine::Serialize(facts, probe, "2026-11-24T12:00:00Z");
    if (s.want_class) Check(cls == s.want_class, std::string(s.id) + ": class is " + s.want_class + ", the game's rules say " + cls);
    if (s.want_tier) Check(tier == s.want_tier, std::string(s.id) + ": ray tracing tier is " + s.want_tier + ", the game's rules say " + tier);
    if (s.want_handheld) Check(facts.gpu.handheld.value_or(false), std::string(s.id) + ": the handheld table knows it");
    Check(text.find("\"hardware_id\"") != std::string::npos, std::string(s.id) + ": a hardware id");
    Check(text.find("\"final\": " + std::string(probe ? "false" : "true")) != std::string::npos, std::string(s.id) + ": the final flag");
    std::string error;
    Check(machine::WriteDocument(folder / (std::string(s.id) + ".machine.json"), text, error), std::string(s.id) + ": written " + error);
    std::printf("%-16s class %-4s tier %-6s %s\n", s.id, cls.c_str(), tier.c_str(), probe ? "(probe failed)" : "");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed != 0;
}
