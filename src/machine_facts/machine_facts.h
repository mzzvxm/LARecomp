#pragma once

#include "app_paths.h"
#include "document.h"

#include <chrono>

namespace larecomp::machine {

// Batch action and UI-thread publisher. The game keeps one publisher per start.
int Probe(const Paths& paths, const std::filesystem::path& output);

class GamePublisher {
 public:
  void Tick(const Paths* paths = nullptr);

 private:
  Facts facts_;
  Publication publication_;
  std::chrono::steady_clock::time_point next_{};
  bool warned_ = false;
};

}  // namespace larecomp::machine
