#pragma once
// The pre-runtime "install the game files" dialog.
//
// Vendored from the ReXGlue fork's rex::ui::InstallWizardDialog
// (rex/ui/overlay/install_wizard_overlay.h) so larecomp builds against the stock
// SDK, which has no such overlay. On rexglue 0.11 it is a dialog of the UI system
// (larecomp_dialog.h) added with ShowDialog.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "larecomp_dialog.h"
#include "os/file_pick.h"

namespace larecomp {

class InstallWizardDialog final : public ui::ImGuiDialog {
 public:
  // Asks the player for the ISO. A result with `unavailable` set (no file chooser could be opened: Linux without the xdg desktop portal or zenity)
  // makes the dialog log one line and show a box to type the path in instead (os/file_pick.h).
  using PickSourceCallback = std::function<os::FilePick()>;
  using InstallCallback = std::function<bool(const std::filesystem::path& destination,
                                            const std::filesystem::path& source,
                                            std::atomic<uint64_t>& copied_bytes,
                                            std::atomic<uint64_t>& total_bytes,
                                            std::function<void(std::string)> set_current_file,
                                            std::string& error)>;
  using CompleteCallback = std::function<void(std::filesystem::path)>;

  // Shown with ui::ShowDialog<InstallWizardDialog>(ui, ...) and destroyed by the UI
  // system when closed. `complete` runs on the UI thread, from the dialog's own
  // draw, once the player presses Start Game after a successful install.
  InstallWizardDialog(std::string title, std::string intro,
                      std::string install_directory, PickSourceCallback pick_source,
                      InstallCallback install, CompleteCallback complete);

  void OnClose() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class State {
    kWaitingForSource,
    kInstalling,
    kInstalled,
    kFailed,
  };

  // The install directory box as the path to install to; false (the dialog is then in its failed state) when it is empty.
  bool CheckInstallDirectory();
  void PickSourceAndInstall();
  // The typed path fallback: the path box's text checked as a file (os::ParseTypedPath), then the install.
  void InstallFromTypedPath();
  void StartInstall(std::filesystem::path source_path);
  void FinishInstallIfNeeded();

  std::string title_;
  std::string intro_;
  std::string install_directory_;
  std::vector<char> install_directory_input_;
  // Set once the file chooser could not be opened: what to tell the player, and the box they type the ISO's path in.
  std::string picker_unavailable_;
  std::vector<char> source_input_;
  std::string source_error_;
  PickSourceCallback pick_source_;
  InstallCallback install_;
  CompleteCallback complete_;
  std::thread install_thread_;
  std::atomic<bool> install_done_{false};
  std::atomic<bool> install_ok_{false};
  std::atomic<uint64_t> copied_bytes_{0};
  std::atomic<uint64_t> total_bytes_{0};
  State state_ = State::kWaitingForSource;
  std::filesystem::path source_path_;
  std::filesystem::path target_path_;
  std::string status_;
  std::string error_;
  std::mutex current_file_mutex_;
  std::string current_file_;
};

}  // namespace larecomp
