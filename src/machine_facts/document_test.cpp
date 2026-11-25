#include "document.h"
#include "../../documentation/contract/cpp/mini_json.h"

#include <fstream>
#include <iostream>
#include <iterator>

namespace {
namespace machine = larecomp::machine;
namespace json = larecomp::minijson;

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

json::Json Parse(std::string_view text) {
  json::Json doc;
  Check(json::Parse(text, &doc), "valid machine JSON");
  return doc;
}

void CheckApi(const json::Json& doc, std::string_view api, std::optional<bool> car_rt,
              const char* rt_tier, const char* ray_regen) {
  const std::string path = "api.by_api." + std::string(api);
  const auto* car = doc.Path(path + ".car_rt");
  const auto* tier = doc.Path(path + ".rt_tier");
  const auto* regen = doc.Path(path + ".ray_regen");
  Check(car && (car_rt ? car->IsBool() && car->b == *car_rt : car->IsNull()), "per-API car RT");
  Check(tier && (rt_tier ? tier->IsString() && tier->s == rt_tier : tier->IsNull()), "per-API RT tier");
  Check(regen && (ray_regen ? regen->IsString() && regen->s == ray_regen : regen->IsNull()), "per-API Ray Regeneration");
}

void CheckDlssCard(const json::Json& doc, std::optional<bool> expected) {
  const auto* card = doc.Path("features.frame_generation.dlss_card");
  Check(card && (expected ? card->IsBool() && card->b == *expected : card->IsNull()), "DLSS FG card eligibility");
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
  gpu.d3d12_compiled = gpu.vulkan_compiled = gpu.ray_regen_built = true;
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
  const auto probe_doc = Parse(probe);
  CheckDlssCard(probe_doc, true);
  CheckApi(probe_doc, "d3d12", true, "strong", nullptr);
  CheckApi(probe_doc, "vulkan", std::nullopt, nullptr, "unsupported");
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
  const auto game_doc = Parse(final);
  CheckDlssCard(game_doc, true);
  CheckApi(game_doc, "d3d12", true, "weak", "no");
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
  CheckDlssCard(Parse(missing), std::nullopt);
  gpu.selected = 1;

  machine::Facts device_free;
  auto& offline = device_free.gpu;
  offline.adapters = gpu.adapters;
  offline.adapters.push_back({.name = adapter.name, .software = true});
  Check(!machine::SelectProbeAdapter(offline.adapters, ""), "multiple hardware adapters without a preference are ambiguous");
  Check(machine::SelectProbeAdapter(offline.adapters, offline.adapters[0].name) == 0,
        "the matching hardware adapter wins even when another follows it");
  offline.selected = machine::SelectProbeAdapter(offline.adapters, adapter.name);
  Check(offline.selected == 1, "a unique preferred hardware adapter wins over a matching software adapter");
  offline.adapters.pop_back();
  offline.adapters.push_back(adapter);
  Check(!machine::SelectProbeAdapter(offline.adapters, adapter.name), "duplicate hardware names stay ambiguous");
  offline.adapters.pop_back();
  offline.d3d12_compiled = offline.vulkan_compiled = offline.ray_regen_built = true;
  offline.streamline_files = offline.dlss_fg = false;
  const auto offline_doc = Parse(machine::Serialize(device_free, true));
  CheckDlssCard(offline_doc, true);
  CheckApi(offline_doc, "d3d12", std::nullopt, nullptr, nullptr);
  CheckApi(offline_doc, "vulkan", std::nullopt, nullptr, "unsupported");
  offline.adapters = {adapter};
  offline.selected = machine::SelectProbeAdapter(offline.adapters, "");
  Check(offline.selected == 0, "the only enumerated hardware adapter is selected");
  offline.adapters[0].name = "nvidia geforce rtx 4070";
  CheckDlssCard(Parse(machine::Serialize(device_free, true)), true);
  offline.adapters[0].name = "NVIDIA RTX PRO 6000 Blackwell";
  CheckDlssCard(Parse(machine::Serialize(device_free, true)), true);
  offline.adapters[0].name = "NVIDIA RTX 2000 Ada";
  CheckDlssCard(Parse(machine::Serialize(device_free, true)), true);
  offline.adapters[0].name = "NVIDIA GeForce RTX 3060";
  CheckDlssCard(Parse(machine::Serialize(device_free, true)), false);
  offline.adapters[0].name = "NVIDIA GeForce GTX 1080";
  CheckDlssCard(Parse(machine::Serialize(device_free, true)), false);
  offline.adapters[0].name = "NVIDIA GeForce RTX 4070";
  offline.adapters[0].vendor_id = gpu.adapters[0].vendor_id;
  CheckDlssCard(Parse(machine::Serialize(device_free, true)), false);
  offline.adapters[0].vendor_id = 0;
  CheckDlssCard(Parse(machine::Serialize(device_free, true)), std::nullopt);
  offline.adapters[0].vendor_id = adapter.vendor_id;
  offline.adapters[0].name.clear();
  CheckDlssCard(Parse(machine::Serialize(device_free, true)), std::nullopt);

  offline.api = "vulkan";
  offline.car_rt = true;
  offline.rt_tier = "weak";
  offline.ray_regen = "unsupported";
  offline.ray_regen_built = false;
  const auto vulkan_doc = Parse(machine::Serialize(device_free, false));
  CheckApi(vulkan_doc, "d3d12", std::nullopt, nullptr, "unsupported");
  CheckApi(vulkan_doc, "vulkan", true, "weak", "unsupported");
  offline.d3d12_compiled = false;
  CheckApi(Parse(machine::Serialize(device_free, false)), "d3d12", false, "none", "unsupported");
  offline.api = "d3d12";
  offline.d3d12_compiled = true;
  offline.vulkan_compiled = false;
  offline.car_rt = false;
  offline.rt_tier = "none";
  offline.ray_regen = "needs_setup";
  const auto d3d_doc = Parse(machine::Serialize(device_free, false));
  CheckApi(d3d_doc, "d3d12", false, "none", "needs_setup");
  CheckApi(d3d_doc, "vulkan", false, "none", "unsupported");

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
