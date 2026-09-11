#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - Save importer / porter
#define _CRT_SECURE_NO_WARNINGS
#include "larecomp_save_porter.h"

#include "../spdlog_console.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <queue>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/logging.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xtypes.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/windowed_app_context_sdl.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <commdlg.h>
#include <windows.h>

#elif defined(__APPLE__)
#else
#include <gtk/gtk.h>
#endif

REXCVAR_DEFINE_BOOL(skip_save_import, false, "MCLA",
    "Skip the save import wizard shown on first launch and start with no save. "
    "The wizard is skipped automatically anyway when no Xenia or RPCS3 save is "
    "found on the machine.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace larecomp {

namespace {

namespace fs = std::filesystem;

// X_STATUS_SUCCESS expands to a bare X_STATUS cast; bring the type into scope.
using rex::X_STATUS;

// Emulated console/profile identity. Matches rex::system user_profile xuid, so
// the content path resolves to the same folder the guest kernel reads from.
constexpr uint64_t kProfileXuid = 0xB13EBABEBABEBABEull;
constexpr uint32_t kTitleId = 0x545407F8u;

constexpr uint32_t kTagX360 = 14;  // Xenia / native 360 build tag
constexpr uint32_t kTagPs3 = 15;   // RPCS3 build tag
constexpr uint32_t kFormatVersion = 9;

const char kTitleFolder[] = "545407F8";
const char kSavedGameType[] = "00000001";
const char kPackageName[] = "mc4.sav";
const char kHeaderName[] = "mc4.sav.header";
const char kDisplayName[] = "Midnight Club: LA";

uint32_t ReadBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void WriteBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

std::string XuidHex() {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(kProfileXuid));
  return buf;
}

// Directory the content store keys the local profile under. Mirrors
// ContentManager::ProfileDirName: the XUID stays fixed, but the on-disk folder
// follows the sanitized profile name (falling back to the hex XUID only when no
// usable name is configured). The wizard runs before the runtime exists, so the
// name is derived from the user_name cvar directly, exactly like UserProfile
// does when the kernel later builds the same path.
std::string ProfileDirName() {
  std::string name = rex::system::xam::UserProfile::ConfiguredName();
  return name.empty() ? XuidHex() : name;
}

bool ReadFileBytes(const fs::path& path, std::vector<uint8_t>& out) {
#if defined(_WIN32)
  // Share-read so a file another program has open (hex editor, STFS tool, cloud
  // sync) can still be read when that program permits shared reads.
  HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
    CloseHandle(h);
    return false;
  }
  out.resize(static_cast<size_t>(size.QuadPart));
  size_t total = 0;
  while (total < out.size()) {
    const DWORD want =
        static_cast<DWORD>(std::min<size_t>(out.size() - total, 1u << 20));
    DWORD got = 0;
    if (!ReadFile(h, out.data() + total, want, &got, nullptr) || got == 0) {
      break;
    }
    total += got;
  }
  CloseHandle(h);
  return total == out.size();
#else
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return false;
  }
  const std::streamoff size = in.tellg();
  if (size <= 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  in.read(reinterpret_cast<char*>(out.data()), size);
  return static_cast<bool>(in);
#endif
}

bool HasStfsMagic(const std::vector<uint8_t>& b) {
  if (b.size() < 4) {
    return false;
  }
  const uint32_t m = ReadBe32(b.data());
  return m == 0x434F4E20u /* "CON " */ || m == 0x4C495645u /* "LIVE" */ ||
         m == 0x50495253u /* "PIRS" */;
}

bool WriteFileBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

struct SaveInfo {
  uint32_t tag = 0;
  int blocks = 0;
};

// Walks the RAGE block container. Structure per block:
//   [namelen:u32be][name][version:u8][size:u32be][payload]
// preceded by [build_tag:u32be][block_count:u32be]. Validation is structural
// only (no checksum exists in the format); a clean walk to EOF means the file
// is a genuine save.
bool ValidateSave(const std::vector<uint8_t>& b, SaveInfo& info, std::string& err) {
  if (b.size() < 12) {
    err = "File is too small to be a Midnight Club save.";
    return false;
  }
  info.tag = ReadBe32(&b[0]);

  size_t off = 8;
  int nblocks = 0;
  while (off < b.size()) {
    if (off + 4 > b.size()) {
      break;
    }
    const uint32_t nlen = ReadBe32(&b[off]);
    if (nlen == 0 || nlen > 64 || off + 4 + nlen + 5 > b.size()) {
      break;
    }
    bool ascii = true;
    for (uint32_t i = 0; i < nlen; i++) {
      const uint8_t c = b[off + 4 + i];
      if (c < 32 || c > 126) {
        ascii = false;
        break;
      }
    }
    if (!ascii) {
      break;
    }
    const size_t p = off + 4 + nlen;
    const uint32_t size = ReadBe32(&b[p + 1]);
    off = p + 5 + static_cast<size_t>(size);
    nblocks++;
  }
  info.blocks = nblocks;

  if (nblocks == 0 || off != b.size()) {
    err = "This does not look like a RAGE save (block structure did not reach end of file).";
    return false;
  }
  if (info.tag != kTagX360 && info.tag != kTagPs3) {
    err = "Unrecognized build tag " + std::to_string(info.tag) +
          " (expected 14 for Xbox 360 or 15 for PS3).";
    return false;
  }
  return true;
}

// The .header is generic for this title's save: it carries only the display
// name, package file name and title id -- no per-save size or hash. One
// template therefore works for any mc4.sav. Reproduces the byte layout the
// guest kernel wrote for a native save.
std::vector<uint8_t> BuildHeaderTemplate() {
  std::vector<uint8_t> h(328, 0);
  WriteBe32(&h[0x00], 1);         // device id
  WriteBe32(&h[0x04], 1);         // content type (saved game)
  // display name, UTF-16 big-endian, starting at 0x08
  size_t o = 0x08;
  for (const char* s = kDisplayName; *s && o < 0x108; ++s) {
    h[o++] = 0;
    h[o++] = static_cast<uint8_t>(*s);
  }
  // package file name, ASCII, at 0x108
  std::memcpy(&h[0x108], kPackageName, std::strlen(kPackageName));
  WriteBe32(&h[0x140], kTitleId);  // title id
  return h;
}

fs::path SaveTargetPath(const fs::path& user_data_root) {
  return user_data_root / ProfileDirName() / kTitleFolder / kSavedGameType / kPackageName /
         kPackageName;
}

fs::path HeaderTargetPath(const fs::path& user_data_root) {
  return user_data_root / ProfileDirName() / kTitleFolder / "Headers" / kSavedGameType /
         kHeaderName;
}

// Content imported by an older build landed under the hex XUID, which the
// content manager no longer reads. Move it across once so those saves are not
// silently lost on the next launch.
void MigrateLegacyXuidDir(const fs::path& user_data_root) {
  const std::string current = ProfileDirName();
  const std::string legacy = XuidHex();
  if (current == legacy) {
    return;
  }
  std::error_code ec;
  const fs::path from = user_data_root / legacy;
  const fs::path to = user_data_root / current;
  if (!fs::is_directory(from, ec) || fs::exists(to, ec)) {
    return;
  }
  fs::rename(from, to, ec);
  if (ec) {
    REXLOG_ERROR("Failed to migrate legacy save directory '{}' -> '{}': {}", from.string(),
                 to.string(), ec.message());
  } else {
    REXLOG_INFO("Migrated legacy save directory '{}' -> '{}'", from.string(), to.string());
  }
}

// Reads all bytes of a mounted STFS entry into a vector.
bool ReadEntryBytes(rex::filesystem::Entry* entry, std::vector<uint8_t>& out) {
  rex::filesystem::File* file = nullptr;
  if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS ||
      !file) {
    return false;
  }
  out.resize(entry->size());
  size_t off = 0;
  while (off < out.size()) {
    size_t got = 0;
    file->ReadSync(std::span<uint8_t>(out.data() + off, out.size() - off), off, &got);
    if (got == 0) {
      break;
    }
    off += got;
  }
  file->Destroy();
  return off == out.size();
}

// A real-console Xbox 360 save is an STFS package (magic CON/LIVE/PIRS) whose
// outer file carries profile/console identity, wrapping the actual RAGE save as
// an inner file. Xenia represents this as a folder named after the package with
// the decrypted inner file inside. Mount the package and pull out the inner
// save (the first entry that validates as a RAGE container).
bool ExtractSaveFromStfs(const fs::path& package, std::vector<uint8_t>& out, SaveInfo& info,
                         std::string& err) {
  if (!rex::filesystem::StfsContainerDevice::ReadPackageHeader(package)) {
    err = "not an STFS package";
    return false;
  }
  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package);
  if (!device->Initialize()) {
    err = "the STFS package could not be opened (damaged or unsupported)";
    return false;
  }
  auto* root = device->ResolvePath("");
  if (!root) {
    err = "the STFS package has no readable contents";
    return false;
  }

  std::queue<rex::filesystem::Entry*> queue;
  queue.push(root);
  while (!queue.empty()) {
    auto* entry = queue.front();
    queue.pop();
    for (const auto& child : entry->children()) {
      queue.push(child.get());
    }
    if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
      continue;
    }
    if (entry->size() < 12 || entry->size() > 8ull * 1024 * 1024) {
      continue;
    }
    std::vector<uint8_t> bytes;
    if (!ReadEntryBytes(entry, bytes)) {
      continue;
    }
    std::string ignored;
    if (ValidateSave(bytes, info, ignored)) {
      out = std::move(bytes);
      REXLOG_INFO("Extracted inner save '{}' from STFS package", entry->name());
      return true;
    }
  }
  err = "no Midnight Club save was found inside the package";
  return false;
}

// Resolves a picked source into validated save bytes. Accepts a raw decrypted
// dump (Xenia inner mc4.sav, or RPCS3 RAGE.SAV) or a real-console STFS package.
bool ReadSourceSaveBytes(const fs::path& source, std::vector<uint8_t>& out, SaveInfo& info,
                         std::string& err) {
  std::vector<uint8_t> raw;
  if (!ReadFileBytes(source, raw)) {
    err = "Could not open the file. Close any program that has it open (a hex editor, an STFS "
          "or save tool, or cloud sync) and try again.";
    return false;
  }

  // Already-decrypted dump: Xenia inner mc4.sav, RPCS3 RAGE.SAV, or a save with
  // the STFS wrapper already stripped.
  std::string verr;
  if (ValidateSave(raw, info, verr)) {
    out = std::move(raw);
    return true;
  }

  // Encrypted real-console package: unwrap the STFS container.
  if (HasStfsMagic(raw)) {
    std::string serr;
    if (ExtractSaveFromStfs(source, out, info, serr)) {
      return true;
    }
    err = "This is an Xbox 360 STFS save, but its contents could not be read (" + serr +
          "). Make sure no other program has the file open.";
    return false;
  }

  err = "Unrecognized file: this is neither a Midnight Club save dump nor an Xbox 360 STFS "
        "package.";
  return false;
}

// Reads a source save, validates it, converts the build tag to the 360 value
// and writes it (plus a header if missing) into the larecomp content store.
bool ImportSaveFile(const fs::path& source, const fs::path& user_data_root, std::string& err) {
  std::vector<uint8_t> bytes;
  SaveInfo info;
  if (!ReadSourceSaveBytes(source, bytes, info, err)) {
    return false;
  }

  // Normalize to the Xbox 360 build tag regardless of source platform.
  WriteBe32(&bytes[0], kTagX360);

  const fs::path pkg = SaveTargetPath(user_data_root);
  const fs::path hdr = HeaderTargetPath(user_data_root);

  std::error_code ec;
  fs::create_directories(pkg.parent_path(), ec);
  if (ec) {
    err = "Unable to create the save directory.";
    return false;
  }
  fs::create_directories(hdr.parent_path(), ec);

  if (!WriteFileBytes(pkg, bytes)) {
    err = "Failed to write the imported save.";
    return false;
  }
  if (!fs::exists(hdr)) {
    WriteFileBytes(hdr, BuildHeaderTemplate());
  }

  REXLOG_INFO("Imported save from {} (source tag {}) -> {}", source.string(), info.tag,
              pkg.string());
  return true;
}

// ---- Auto-detection of source saves from other emulators --------------------

std::vector<fs::path> CandidateRoots() {
  std::vector<fs::path> roots;
  auto add = [&](const fs::path& p) {
    if (!p.empty()) {
      roots.push_back(p);
    }
  };

#if defined(_WIN32)
  if (const char* up = std::getenv("USERPROFILE")) {
    const fs::path home = up;
    for (const char* sub : {"Desktop", "Documents", "Downloads", ""}) {
      add(home / sub);
    }
  }
#else
  if (const char* home = std::getenv("HOME")) {
    const fs::path h = home;
    for (const char* sub : {"Desktop", "Documents", "Downloads", ""}) {
      add(h / sub);
    }
  }
#endif
  return roots;
}

bool DirExists(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

// RPCS3: <root>/dev_hdd0/home/<user>/savedata/<*MC4_SAV*>/RAGE.SAV
fs::path FindRpcs3Save() {
  std::error_code ec;
  for (const fs::path& base_root : CandidateRoots()) {
    // Accept either the root itself or a child named after the emulator.
    std::array<fs::path, 2> bases = {base_root, base_root / "RPCS3"};
    for (const fs::path& base : bases) {
      const fs::path home = base / "dev_hdd0" / "home";
      if (!DirExists(home)) {
        continue;
      }
      for (fs::directory_iterator ui(home, ec), uend; ui != uend && !ec; ui.increment(ec)) {
        const fs::path savedata = ui->path() / "savedata";
        if (!DirExists(savedata)) {
          continue;
        }
        for (fs::directory_iterator gi(savedata, ec), gend; gi != gend && !ec; gi.increment(ec)) {
          const std::string name = gi->path().filename().string();
          if (name.find("MC4_SAV") == std::string::npos) {
            continue;
          }
          const fs::path rage = gi->path() / "RAGE.SAV";
          if (fs::is_regular_file(rage, ec)) {
            return rage;
          }
        }
      }
    }
  }
  return {};
}

// Xenia: <root>/content/<xuid>/545407F8/00000001/mc4.sav/mc4.sav
fs::path FindXeniaSave() {
  std::error_code ec;
  for (const fs::path& base_root : CandidateRoots()) {
    std::array<fs::path, 3> bases = {base_root, base_root / "Xenia", base_root / "xenia"};
    for (const fs::path& base : bases) {
      const fs::path content = base / "content";
      if (!DirExists(content)) {
        continue;
      }
      for (fs::directory_iterator xi(content, ec), xend; xi != xend && !ec; xi.increment(ec)) {
        const fs::path save = xi->path() / kTitleFolder / kSavedGameType / kPackageName / kPackageName;
        if (fs::is_regular_file(save, ec)) {
          return save;
        }
      }
    }
  }
  return {};
}

// ---- Native file picker -----------------------------------------------------

#if defined(_WIN32)
fs::path PickSaveFile() {
  wchar_t filename[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFile = filename;
  ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
  ofn.lpstrFilter = L"Midnight Club save (mc4.sav, RAGE.SAV)\0mc4.sav;RAGE.SAV;*.sav;*.SAV\0"
                    L"All files (*.*)\0*.*\0";
  ofn.lpstrTitle = L"Select mc4.sav (Xenia) or RAGE.SAV (RPCS3)";
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&ofn)) {
    return {};
  }
  return filename;
}
#elif defined(__APPLE__)
fs::path PickSaveFile() { return {}; }
#else
fs::path PickSaveFile() {
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "Select mc4.sav (Xenia) or RAGE.SAV (RPCS3)", nullptr, GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
  if (!dialog) {
    return {};
  }
  GtkFileFilter* sav = gtk_file_filter_new();
  gtk_file_filter_set_name(sav, "Midnight Club save (mc4.sav, RAGE.SAV)");
  gtk_file_filter_add_pattern(sav, "mc4.sav");
  gtk_file_filter_add_pattern(sav, "RAGE.SAV");
  gtk_file_filter_add_pattern(sav, "*.sav");
  gtk_file_filter_add_pattern(sav, "*.SAV");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), sav);

  fs::path result;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (name) {
      result = name;
      g_free(name);
    }
  }
  gtk_widget_destroy(dialog);
  while (gtk_events_pending()) {
    gtk_main_iteration_do(FALSE);
  }
  return result;
}
#endif

// ---- Dialog -----------------------------------------------------------------

class SaveImportDialog final : public rex::ui::ImGuiDialog {
 public:
  SaveImportDialog(rex::ui::ImGuiDrawer* drawer, fs::path user_data_root, fs::path xenia_found,
                   fs::path rpcs3_found, std::function<void()> on_finished)
      : ImGuiDialog(drawer),
        user_data_root_(std::move(user_data_root)),
        xenia_found_(std::move(xenia_found)),
        rpcs3_found_(std::move(rpcs3_found)),
        on_finished_(std::move(on_finished)) {}

 protected:
  void OnClose() override {
    REXLOG_INFO("Save wizard: dialog closed");
    if (on_finished_) {
      on_finished_();
      on_finished_ = nullptr;
    }
  }

  void OnDraw(ImGuiIO& io) override {
    // Diagnostics: confirms the dialog is being drawn at a sane size and that
    // pointer input actually reaches ImGui (a click that never arrives looks
    // exactly like a dead button from the outside).
    if (!logged_first_draw_) {
      logged_first_draw_ = true;
      REXLOG_INFO("Save wizard: first draw, display {}x{}", io.DisplaySize.x, io.DisplaySize.y);
    }
    if (io.MouseDown[0] && !mouse_was_down_) {
      REXLOG_INFO("Save wizard: mouse down at ({}, {}), want_capture_mouse={}", io.MousePos.x,
                  io.MousePos.y, io.WantCaptureMouse);
    }
    mouse_was_down_ = io.MouseDown[0];

    const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Always);

    if (ImGui::Begin("Import existing save##larecomp_save_import", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::TextWrapped(
          "No Midnight Club: LA save was found. Import one from an emulator or a real "
          "console (Xbox 360 STFS saves are unpacked automatically), or skip and let the "
          "game create a new save.");
      ImGui::Dummy(ImVec2(0.0f, 6.0f));
      ImGui::Separator();
      ImGui::Dummy(ImVec2(0.0f, 6.0f));

      if (state_ == State::kChoosing) {
        DrawChoice("Import from Xenia (Xbox 360)", "mc4.sav", xenia_found_,
                   "larecomp_import_xenia");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        DrawChoice("Import from RPCS3 (PlayStation 3)", "RAGE.SAV", rpcs3_found_,
                   "larecomp_import_rpcs3");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        // Real console save copied off a 360 (USB / transfer). Accepts either an
        // encrypted STFS package or an already-extracted raw mc4.sav -- the import
        // path auto-detects and handles both. No standard host path, so browse-only.
        DrawChoice("Import from a real Xbox 360 (STFS or extracted)", "mc4.sav", fs::path(),
                   "larecomp_import_real360");
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        if (ImGui::Button("Skip - start without a save", ImVec2(-1.0f, 0.0f))) {
          REXLOG_INFO("Save wizard: skip pressed, starting without a save");
          Close();
        }
      } else if (state_ == State::kDone) {
        ImGui::TextColored(ImVec4(0.34f, 0.76f, 0.35f, 1.0f), "%s", status_.c_str());
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        if (ImGui::Button("Continue", ImVec2(-1.0f, 0.0f))) {
          Close();
        }
      } else {  // kError
        ImGui::TextColored(ImVec4(0.97f, 0.72f, 0.33f, 1.0f), "%s", status_.c_str());
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        if (ImGui::Button("Back", ImVec2(150.0f, 0.0f))) {
          state_ = State::kChoosing;
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip", ImVec2(-1.0f, 0.0f))) {
          Close();
        }
      }
    }
    ImGui::End();
  }

 private:
  enum class State { kChoosing, kDone, kError };

  void DrawChoice(const char* title, const char* expected, const fs::path& found,
                  const char* id) {
    ImGui::PushID(id);
    const bool has = !found.empty();
    const std::string label = has ? (std::string(title) + "\n  found: " + found.string())
                                  : (std::string(title) + "\n  browse for " + expected + "...");
    if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
      fs::path source = has ? found : PickSaveFile();
      if (!source.empty()) {
        DoImport(source);
      }
    }
    ImGui::PopID();
  }

  void DoImport(const fs::path& source) {
    std::string err;
    if (ImportSaveFile(source, user_data_root_, err)) {
      status_ = "Save imported. It will be available in-game.";
      state_ = State::kDone;
    } else {
      status_ = "Import failed: " + err;
      state_ = State::kError;
      REXLOG_ERROR("Save import failed: {}", err);
    }
  }

  fs::path user_data_root_;
  fs::path xenia_found_;
  fs::path rpcs3_found_;
  std::function<void()> on_finished_;
  State state_ = State::kChoosing;
  std::string status_;
  bool logged_first_draw_ = false;
  bool mouse_was_down_ = false;
};

}  // namespace

bool SaveAlreadyPresent(const fs::path& user_data_root) {
  MigrateLegacyXuidDir(user_data_root);
  std::error_code ec;
  const fs::path target = SaveTargetPath(user_data_root);
  const bool present = fs::is_regular_file(target, ec);
  REXLOG_INFO("Save check: '{}' {}", target.string(), present ? "found" : "missing");
  return present;
}

void RunSaveImportWizardBlocking(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                                 rex::ui::ImGuiDrawer* drawer, const rex::PathConfig& paths) {
  const fs::path user_data_root = paths.user_data_root;

  if (REXCVAR_GET(skip_save_import)) {
    REXLOG_INFO("Save import: skipped (skip_save_import)");
    return;
  }

  const fs::path xenia = FindXeniaSave();
  const fs::path rpcs3 = FindRpcs3Save();
  REXLOG_INFO("Save import: xenia='{}' rpcs3='{}'", xenia.string(), rpcs3.string());

  // With no emulator save on the machine the wizard has nothing to offer but a
  // file browser, and the game creates its own save anyway - so it is a wall
  // between a first-time player and the title. Skip it.
  if (xenia.empty() && rpcs3.empty()) {
    REXLOG_INFO("Save import: nothing found to import, starting without a save");
    return;
  }

  auto done = std::make_shared<std::atomic<bool>>(false);
  // Self-deletes on Close(); the completion callback releases the pump below.
  new SaveImportDialog(drawer, user_data_root, xenia, rpcs3,
                       [done]() { done->store(true, std::memory_order_release); });

  REXLOG_INFO("Entering save import pump");
  // Step tracing for the first ticks only: the pump is the first thing that
  // runs after the profile name is accepted, and a death in here leaves a log
  // that just stops. INFO is flushed per line, so the last one printed names
  // the statement that died.
  int tick = 0;
  while (!done->load(std::memory_order_acquire) && !app_context.HasQuitFromUIThread()) {
    const bool trace = tick < 5;
    if (trace) REXLOG_INFO("Save pump tick {}: pending functions", tick);
    app_context.ExecutePendingFunctionsFromUIThread();

    if (trace) REXLOG_INFO("Save pump tick {}: pump events", tick);
    app_context.PumpEvents();

    if (app_context.HasQuitFromUIThread()) {
      break;
    }
    if (window) {
      if (trace) REXLOG_INFO("Save pump tick {}: request paint", tick);
      window->RequestPaint();
    }
    if (trace) REXLOG_INFO("Save pump tick {}: done", tick);
    ++tick;
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }

  // Closing the window during the wizard tears the surface down. Booting the
  // title anyway leaves it rendering into a dead window (a black screen that
  // never starts), so quit here the same way the ISO installer does.
  if (app_context.HasQuitFromUIThread()) {
    REXLOG_INFO("Leaving save import pump: window closed, aborting startup");
    ShutdownLarecompLogging();
    std::_Exit(0);
  }
  REXLOG_INFO("Leaving save import pump");
}

}  // namespace larecomp

#endif // REXGLUE_HAS_XEO3_TARGET
