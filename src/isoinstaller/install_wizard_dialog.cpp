#ifndef REXGLUE_HAS_XEO3_TARGET
// See install_wizard_dialog.h. Same behaviour as the fork's
// src/ui/overlay/install_wizard_overlay.cpp; only the namespace and the
// UTF-8-safe path display differ.

#include "install_wizard_dialog.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <imgui.h>

namespace larecomp {

namespace {
// path::string() is in the ANSI code page on Windows and can throw on a path it
// cannot represent; ImGui wants UTF-8.
std::string PathUtf8(const std::filesystem::path& p) {
  const std::u8string u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}
}  // namespace

InstallWizardDialog::InstallWizardDialog(rex::ui::ImGuiDrawer* drawer, std::string title,
                                         std::string intro, std::string install_directory,
                                         PickSourceCallback pick_source, InstallCallback install,
                                         CompleteCallback complete)
    : ImGuiDialog(drawer),
      title_(std::move(title)),
      intro_(std::move(intro)),
      install_directory_(std::move(install_directory)),
      install_directory_input_(32768, '\0'),
      pick_source_(std::move(pick_source)),
      install_(std::move(install)),
      complete_(std::move(complete)),
      status_(intro_) {
  std::memcpy(install_directory_input_.data(), install_directory_.data(),
              std::min(install_directory_.size(), install_directory_input_.size() - 1));
}

void InstallWizardDialog::OnClose() {
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
}

void InstallWizardDialog::PickSourceAndInstall() {
  install_directory_ = install_directory_input_.data();
  if (install_directory_.empty()) {
    state_ = State::kFailed;
    status_ = "Installation failed.";
    error_ = "Choose an install directory.";
    return;
  }
  if (!pick_source_) {
    return;
  }
  auto source_path = pick_source_();
  if (source_path.empty()) {
    return;
  }
  StartInstall(std::move(source_path));
}

void InstallWizardDialog::StartInstall(std::filesystem::path source_path) {
  if (install_thread_.joinable()) {
    install_thread_.join();
  }

  source_path_ = std::move(source_path);
  try {
    target_path_ = std::filesystem::path(
        std::u8string(install_directory_.begin(), install_directory_.end()));
  } catch (const std::filesystem::filesystem_error&) {
    state_ = State::kFailed;
    status_ = "Installation failed.";
    error_ = "The install directory is not a valid path.";
    return;
  }
  copied_bytes_ = 0;
  total_bytes_ = 0;
  install_done_ = false;
  install_ok_ = false;
  error_.clear();
  state_ = State::kInstalling;
  status_ = "Installing game files...";

  install_thread_ = std::thread([this]() {
    std::string error;
    auto update_status = [this](std::string status) {
      std::lock_guard<std::mutex> lock(current_file_mutex_);
      current_file_ = std::move(status);
    };
    const bool ok =
        install_ && install_(target_path_, source_path_, copied_bytes_, total_bytes_, update_status, error);
    error_ = std::move(error);
    install_ok_ = ok;
    install_done_ = true;
  });
}

void InstallWizardDialog::FinishInstallIfNeeded() {
  if (state_ != State::kInstalling || !install_done_.load(std::memory_order_acquire)) {
    return;
  }
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
  if (install_ok_.load(std::memory_order_acquire)) {
    state_ = State::kInstalled;
    status_ = "Installation complete.";
  } else {
    state_ = State::kFailed;
    status_ = "Installation failed.";
  }
}

void InstallWizardDialog::OnDraw(ImGuiIO& io) {
  FinishInstallIfNeeded();

  const float width = std::min(760.0f, std::max(460.0f, io.DisplaySize.x - 64.0f));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 9.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
  ImGui::PushFont(nullptr, 18.0f);

  if (ImGui::Begin(title_.c_str(), nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("%s", status_.c_str());
    ImGui::Spacing();
    if (state_ == State::kWaitingForSource || state_ == State::kFailed) {
      ImGui::InputText("Install directory", install_directory_input_.data(),
                       install_directory_input_.size());
    } else {
      ImGui::TextWrapped("Install directory: %s", install_directory_.c_str());
    }

    if (!source_path_.empty()) {
      ImGui::TextWrapped("Source: %s", PathUtf8(source_path_).c_str());
    }

    if (state_ == State::kInstalling) {
      const uint64_t total = total_bytes_.load(std::memory_order_relaxed);
      const uint64_t copied = copied_bytes_.load(std::memory_order_relaxed);
      const float progress =
          total == 0 ? 0.0f
                     : std::clamp(static_cast<float>(double(copied) / double(total)), 0.0f, 1.0f);
      ImGui::ProgressBar(progress, ImVec2(-1.0f, 28.0f));

      std::string current_file_copy;
      {
        std::lock_guard<std::mutex> lock(current_file_mutex_);
        current_file_copy = current_file_;
      }
      if (!current_file_copy.empty()) {
        ImGui::TextWrapped("Extracting: %s", current_file_copy.c_str());
      }
    } else if (state_ == State::kFailed) {
      ImGui::TextColored(ImVec4(0.95f, 0.28f, 0.24f, 1.0f), "%s", error_.c_str());
    }

    ImGui::Spacing();
    if (state_ == State::kWaitingForSource || state_ == State::kFailed) {
      if (ImGui::Button("Select ISO", ImVec2(160.0f, 42.0f))) {
        PickSourceAndInstall();
      }
    } else if (state_ == State::kInstalled) {
      if (ImGui::Button("Start Game", ImVec2(160.0f, 42.0f))) {
        auto complete = std::move(complete_);
        Close();
        if (complete) {
          complete(std::move(target_path_));
        }
      }
    }
  }
  // Begin/End are unconditionally paired: Begin returns false for a clipped or
  // collapsed window, and skipping End there unbalances the window stack.
  ImGui::End();

  ImGui::PopFont();
  ImGui::PopStyleVar(3);
}

}  // namespace larecomp

#endif  // REXGLUE_HAS_XEO3_TARGET
