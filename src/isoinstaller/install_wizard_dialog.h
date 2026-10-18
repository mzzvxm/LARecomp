#pragma once
// The pre-runtime "install the game files" dialog.
//
// Vendored from the ReXGlue fork's rex::ui::InstallWizardDialog
// (rex/ui/overlay/install_wizard_overlay.h) so larecomp builds against the stock
// SDK, which has no such overlay. It only needs rex::ui::ImGuiDialog and ImGui,
// both upstream. Renamed into larecomp's namespace so it can coexist with the
// fork's copy when built against the fork.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/ui/imgui_dialog.h>

namespace larecomp {

class InstallWizardDialog final : public rex::ui::ImGuiDialog {
 public:
  using PickSourceCallback = std::function<std::filesystem::path()>;
  using InstallCallback = std::function<bool(const std::filesystem::path& destination,
                                            const std::filesystem::path& source,
                                            std::atomic<uint64_t>& copied_bytes,
                                            std::atomic<uint64_t>& total_bytes,
                                            std::function<void(std::string)> set_current_file,
                                            std::string& error)>;
  using CompleteCallback = std::function<void(std::filesystem::path)>;

  // Shows itself on construction and deletes itself when closed, like every
  // rex::ui::ImGuiDialog. `complete` runs on the UI thread, from the dialog's
  // own draw, once the player presses Start Game after a successful install.
  InstallWizardDialog(rex::ui::ImGuiDrawer* drawer, std::string title, std::string intro,
                      std::string install_directory, PickSourceCallback pick_source,
                      InstallCallback install, CompleteCallback complete);

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class State {
    kWaitingForSource,
    kInstalling,
    kInstalled,
    kFailed,
  };

  void PickSourceAndInstall();
  void StartInstall(std::filesystem::path source_path);
  void FinishInstallIfNeeded();

  std::string title_;
  std::string intro_;
  std::string install_directory_;
  std::vector<char> install_directory_input_;
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
