#include "document.h"

#include <fstream>
#include <iostream>
#include <iterator>

namespace {
namespace machine = larecomp::machine;

int checks = 0;
int failures = 0;

void Check(bool ok, const char* description) {
  ++checks;
  if (!ok) {
    ++failures;
    std::cerr << description << '\n';
  }
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}  // namespace

int main(int argc, char** argv) {
  Check(machine::Sha256("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA-256");
  Check(machine::Sha256("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc");
  Check(machine::Sha256(std::string(1000, 'a')) ==
        "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", "SHA-256 multiple blocks");

  machine::Facts facts;
  facts.os = "windows";
  facts.os_build = 26300;
  facts.cpu_threads = 16;
  facts.cpu_level = "x86-64-v3";
  facts.game_version = "PATCH 11 - PREVIEW 1";
  facts.commit = std::string(40, '1');
  facts.title_id = "545407F8";
  facts.profile_id = "B13EBABEBABEBABE";
  facts.game_folder = "C:/Example/LA Recomp";
  facts.config_file = facts.game_folder / "larecomp.toml";
  facts.mod_root = facts.game_folder / "modloader";
  facts.mod_roots_ignored = {facts.game_folder / "mods"};
  facts.music = {facts.game_folder / "music"};
  facts.displays = {{"Primary \"HDR\" monitor", 3840, 2160, 239.76f, true},
                    {"Secondary monitor", 1920, 1080, 60, false}};
  facts.windowed_sizes = {{1280, 720}, {1920, 1080}};
  facts.exclusive_sizes = {{3840, 2160, 240}};
  facts.hdr_capable = true;
  facts.portuguese_text = false;
  facts.city_pack = "current";
  facts.city_hit = "stale";
  facts.shader_pack = "missing";
  auto& gpu = facts.gpu;
  gpu.adapters = {
      {.name = "AMD Radeon Graphics", .vendor_id = 4098, .device_id = 0x164e,
       .driver_version = "32.0.0.0", .vram_mb = 512, .gpu_class = "low",
       .shared_memory = true, .class_why = "integrated"},
      {.name = "NVIDIA GeForce RTX 5080", .vendor_id = 4318, .device_id = 11037,
       .driver_version = "32.0.15.6094", .vram_mb = 16303, .gpu_class = "high",
       .shared_memory = false, .class_why = "by memory"},
  };
  gpu.selected = 1;
  const auto& adapter = gpu.adapters[1];
  const std::string id = machine::HardwareId(adapter, facts.os_build);
  Check(id == "7a3802f39715c93b3b93a2d5933dc90d29941c4ab92426d426671a5986b2a08b",
        "hardware ID matches the UTF-8/NUL SHA-256 vector");
  Check(id == machine::HardwareId(adapter, facts.os_build), "same inputs, same hardware ID");
  auto changed = adapter;
  changed.driver_version = "32.0.15.6095";
  Check(id != machine::HardwareId(changed, facts.os_build), "a driver update changes hardware ID");
  Check(id != machine::HardwareId(adapter, facts.os_build + 1), "an OS update changes hardware ID");

  gpu.rtx = true;
  gpu.rx9000 = false;
  gpu.handheld = false;
  gpu.rt_benchmarked = false;
  gpu.rt_tier = gpu.rt_rung = "strong";
  gpu.api = "d3d12";
  gpu.api_version = "12_2";
  gpu.dxr_1_1 = gpu.ray_query = gpu.car_rt = gpu.city_rt_shadows = true;
  gpu.fsr1 = gpu.fsr31 = true;
  gpu.fsr4 = false;
  gpu.fidelityfx_files = gpu.streamline_files = true;
  gpu.pending = true;

  constexpr std::string_view kUtc = "2026-11-24T18:02:00Z";
  const std::string probe = machine::Serialize(facts, true, kUtc);
  const auto contains = [](std::string_view body, std::string_view field) {
    return body.find(field) != std::string_view::npos;
  };
  const auto one_selected = [&](std::string_view body) {
    const auto first = body.find("\"selected\": true");
    return first != body.npos && body.find("\"selected\": true", first + 1) == body.npos;
  };
  Check(contains(probe, "\"source\": \"probe\"") && contains(probe, "\"final\": false"), "probe envelope");
  Check(contains(probe, "\"hardware_id\": \"" + id + "\""), "selected adapter supplies hardware ID");
  Check(one_selected(probe) && contains(probe, "\"selected\": false"), "exactly one selected adapter");
  Check(contains(probe, "\"dlss\": null") && contains(probe, "\"dlss_modes_refused\": null"),
        "probe leaves unobserved DLSS facts unknown");
  Check(contains(probe, "Primary \\\"HDR\\\" monitor"), "strings are JSON escaped");
  Check(contains(probe, "\"size\": \"3840X2160\""), "display size is a contract label");
  Check(!machine::IsFinal(false, gpu), "game waits for started probes");

  machine::Publication publisher;
  const auto pending = machine::Serialize(facts, false);
  Check(publisher.Changed(pending) && publisher.Changed(pending), "first publication retries until successful");
  publisher.NoteSuccess(pending);
  Check(!publisher.Changed(pending), "identical payload skips disk write");
  Check(!contains(pending, "written_utc"), "timestamp excluded from comparison");

  gpu.pending = false;
  Check(publisher.Changed(machine::Serialize(facts, false)), "final transition republishes unchanged facts");
  Check(!machine::IsFinal(true, gpu), "probe remains provisional even with no pending work");
  gpu.rt_benchmarked = true;
  gpu.rt_tier = gpu.rt_rung = "weak";
  gpu.dlss = "ready";
  gpu.refused_modes = std::vector<std::string>{"ultra_performance"};
  gpu.dlss_fg = true;
  gpu.dlss_multiplier = 4;
  gpu.dlss_after_restart = false;
  gpu.latency = "reflex";
  gpu.latency_auto = true;
  gpu.denoiser = "nvidia";
  gpu.dlss_rr = "ready";
  gpu.ray_regen = "no";
  gpu.paper_white = 240;
  gpu.peak_nits = 1000;
  gpu.gi_update = gpu.pcss = true;
  const auto final = machine::Serialize(facts, false);
  Check(contains(final, "\"source\": \"game\"") && contains(final, "\"final\": true"), "final game envelope");
  Check(contains(final, "\"hardware_id\": \"" + id + "\""), "runtime results do not change hardware ID");
  Check(publisher.Changed(final), "completed probes update the document");
  publisher.NoteSuccess(final);
  gpu.class_forced = "low";
  const auto forced = machine::Serialize(facts, false);
  Check(publisher.Changed(forced), "forced GPU class updates the document");
  Check(forced.find("\"class\": \"low\"") < forced.find("\"adapters\""), "effective class is top level");
  Check(contains(forced, "\"class\": \"high\""), "selected adapter retains its detected class");

  gpu.selected = 99;
  const auto missing = machine::Serialize(facts, true, kUtc);
  Check(!contains(missing, "\"hardware_id\"") && !contains(missing, "\"selected\": true"),
        "unresolved selection cannot select the wrong adapter");
  Check(contains(missing, "\"adapters\": []"), "unresolved selection has no generated adapter choices");
  gpu.selected = 1;

  if (argc == 2) {
    const std::filesystem::path output = argv[1];
    std::string error;
    const auto path = output / "machine.probe.json";
    Check(machine::WriteDocument(path, pending, error), "first file write");
    Check(machine::WriteDocument(path, probe, error) && Read(path) == probe, "replacement keeps complete bytes");
    Check(machine::WriteDocument(output / "machine.game.json", machine::Serialize(facts, false, kUtc), error),
          "write final game fixture");
    Check(machine::WriteDocument(output / "machine.unresolved.json", missing, error),
          "write unresolved selection fixture");
    Check(!machine::WriteDocument(output / "absent" / "machine.json", probe, error) && !error.empty(),
          "failed replacement reports error");
    const auto directory = output / "directory.json";
    std::filesystem::create_directory(directory);
    Check(!machine::WriteDocument(directory, probe, error) && std::filesystem::is_directory(directory),
          "failed atomic move leaves the existing destination intact");
  }
  std::cout << checks << " checks, " << failures << " failed\n";
  return failures != 0;
}
