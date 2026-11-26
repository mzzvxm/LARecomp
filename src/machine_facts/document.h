#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larecomp::machine {

struct Adapter {
  std::string name;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  std::string driver_version;
  uint64_t vram_mb = 0;
  bool software = false;
  std::optional<std::string> gpu_class;
  std::optional<bool> shared_memory;
  std::string class_why;
};

struct GpuFacts {
  std::vector<Adapter> adapters;
  std::optional<size_t> selected;
  std::optional<bool> rtx, rx9000, rt_benchmarked, handheld;
  std::optional<std::string> class_forced, rt_tier, rt_rung;
  std::string vram_warning;
  std::string rt_tier_why;
  std::string api, api_version, shader_model;
  std::optional<bool> dxr_1_1, ray_query, car_rt, city_rt_shadows;
  std::optional<bool> fsr1, fsr31, fsr4, fsr31_fg, dlss_fg, dlss_after_restart;
  std::optional<std::string> dlss;
  std::optional<std::vector<std::string>> refused_modes;
  std::optional<uint32_t> dlss_multiplier;
  std::optional<std::string> latency, denoiser, dlss_rr, ray_regen;
  std::optional<bool> latency_auto, pcss;
  std::optional<uint32_t> paper_white, peak_nits;
  std::optional<bool> gi_update, fidelityfx_files, streamline_files;
  bool d3d12_compiled = false, vulkan_compiled = false, ray_regen_built = false;
  bool pending = false;
};

// After a failed headless device open, a matching hardware adapter or the sole
// hardware adapter can still identify the card. An ambiguous list stays unknown.
std::optional<size_t> SelectProbeAdapter(const std::vector<Adapter>& adapters,
                                         std::string_view preferred_name);

struct Display {
  std::string name;
  uint32_t width = 0, height = 0;
  float hz = 0;
  std::optional<bool> hdr;
};

struct Facts {
  GpuFacts gpu;
  std::vector<Display> displays;  // primary first
  std::vector<std::array<uint32_t, 2>> windowed_sizes;
  std::vector<std::array<uint32_t, 3>> exclusive_sizes;
  std::optional<bool> hdr_capable;
  std::string os, cpu_level, game_version, commit;
  uint32_t os_build = 0, cpu_threads = 1;
  bool exclusive_fullscreen = false, live_window_resize = false, window_size_fixed = false;
  bool hdr_output = false, real_weather = true, mouse_look = true;
  bool wheel_ffb = false, frame_pacing_wake = true;
  std::optional<bool> portuguese_text;
  std::optional<std::string> city_pack, city_hit, shader_pack;
  std::filesystem::path game_folder, config_file, user_root, saves_root, cache_root, logs;
  std::filesystem::path game_data_root, mod_root, strings_replace;
  std::vector<std::filesystem::path> mod_roots_ignored, music;
  std::string title_id, profile_id;
};

// Names and driver versions use their UTF-8 bytes as emitted. Separate the five fields with a
// single NUL, without a trailing NUL or BOM. Integers are unsigned decimal without padding.
std::string HardwareId(const Adapter& adapter, uint32_t os_build);
std::string Sha256(std::string_view bytes);
std::string PathText(const std::filesystem::path& path);

bool IsFinal(bool probe, const GpuFacts& gpu);
std::string Serialize(const Facts& facts, bool probe, std::string_view written_utc = {});

// The comparison excludes the timestamp. A failed write leaves the previous snapshot in place
// and is eligible for retry on the next refresh.
class Publication {
 public:
  bool Changed(std::string_view snapshot) const { return snapshot != published_; }
  void NoteSuccess(std::string snapshot) { published_ = std::move(snapshot); }

 private:
  std::string published_;
};

// A unique temporary file on the same volume, followed by one atomic replacement.
bool WriteDocument(const std::filesystem::path& path, std::string_view bytes, std::string& error);

}  // namespace larecomp::machine
