// The AppImage variables the launcher leaves in the game's environment. See appimage_env.h.

#include "os/appimage_env.h"

#include <array>

#if defined(__linux__)
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>
#endif

namespace larecomp::os {

namespace {

// What the AppImage runtime sets (type2-runtime): the launcher's Core\Setup\Integration\AppImageHost.RuntimeVariables is the same list.
constexpr std::array<std::string_view, 4> kRuntimeVariables = {"APPIMAGE", "APPDIR", "ARGV0", "OWD"};

#if defined(__linux__)

// The variable's value, "" when it is not set.
std::string Variable(std::string_view name) {
  const std::string key(name);
  const char* value = std::getenv(key.c_str());
  return value ? std::string(value) : std::string();
}

// This process's own file, as the kernel resolves it. "" when /proc cannot say.
std::string OwnProgramPath() {
  std::string path(4096, '\0');
  const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length <= 0) {
    return {};
  }
  path.resize(size_t(length));
  return path;
}

#endif

}  // namespace

bool ProgramIsInsideMount(std::string_view mount, std::string_view program) {
  while (mount.size() > 1 && mount.back() == '/') {
    mount.remove_suffix(1);
  }
  if (mount.size() < 2 || mount.front() != '/') {
    return false;  // empty, relative, or "/"
  }
  return program.size() > mount.size() + 1 && program.starts_with(mount) && program[mount.size()] == '/';
}

bool RunsFromAppImage() {
#if defined(__linux__)
  const std::string image = Variable("APPIMAGE");
  const std::string mount = Variable("APPDIR");
  if (image.empty() || mount.empty()) {
    return false;
  }
  const std::string program = OwnProgramPath();
  if (ProgramIsInsideMount(mount, program)) {
    return true;
  }
  // A mount folder reached through a link (a TMPDIR that is a link): the kernel resolved the program's path, $APPDIR may not be.
  std::error_code error;
  const std::filesystem::path resolved = std::filesystem::canonical(mount, error);
  return !error && ProgramIsInsideMount(resolved.string(), program);
#else
  return false;
#endif
}

std::string OwnAppImageFile() {
#if defined(__linux__)
  return RunsFromAppImage() ? Variable("APPIMAGE") : std::string();
#else
  return {};
#endif
}

std::string DropInheritedAppImageVariables() {
  std::string removed;
#if defined(__linux__)
  if (RunsFromAppImage()) {
    return removed;
  }
  for (const std::string_view name : kRuntimeVariables) {
    const std::string key(name);
    if (std::getenv(key.c_str()) == nullptr) {
      continue;
    }
    unsetenv(key.c_str());
    removed += (removed.empty() ? "" : ", ") + key;
  }
#else
  (void)kRuntimeVariables;
#endif
  return removed;
}

}  // namespace larecomp::os
