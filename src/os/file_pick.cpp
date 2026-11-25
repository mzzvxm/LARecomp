// Asking the player for a file without GTK, and the typed path it falls back to. See file_pick.h.

#include "os/file_pick.h"

#include <cctype>
#include <system_error>

namespace larecomp::os {

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "%20" -> " ". A % that is not followed by two hex digits stays as it is (a file name may hold one).
std::string PercentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 && HexValue(text[i + 2]) >= 0) {
      out += static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2]));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

// A path from UTF-8 text on any system (path::string() is the ANSI code page on Windows).
std::filesystem::path PathOf(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string Shown(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

}  // namespace

FilePick FromDialogAnswer(bool opened, const std::vector<std::filesystem::path>& chosen, std::string_view error) {
  FilePick pick;
  if (!opened) {
    pick.unavailable = error.empty() ? std::string("the dialog did not open") : std::string(error);
    return pick;
  }
  if (chosen.empty() || chosen.front().empty()) {
    pick.cancelled = true;
    return pick;
  }
  pick.path = chosen.front();
  return pick;
}

std::string UnavailableText(std::string_view reason, std::string_view what) {
  std::string text = "No file chooser could be opened here";
  const std::string_view why = Trim(reason);
  if (!why.empty()) {
    text += " (";
    text += why;
    text += ")";
  }
  text += ". Type the path of ";
  text += what;
  text += " below instead.";
  return text;
}

TypedPath ParseTypedPath(std::string_view typed, std::string_view home) {
  TypedPath result;
  std::string text(Trim(typed));
  if (text.empty()) {
    result.error = "Type the path of the file.";
    return result;
  }

  // One pair of matching quotes around the whole path.
  if (text.size() >= 2 && (text.front() == '\'' || text.front() == '"') && text.back() == text.front()) {
    text = std::string(Trim(std::string_view(text).substr(1, text.size() - 2)));
  }

  // A file:// address, as a file manager gives it for a dropped or copied file.
  if (StartsWith(text, "file://")) {
    std::string_view rest = std::string_view(text).substr(7);
    if (StartsWith(rest, "localhost/")) {
      rest.remove_prefix(9);
    }
    text = PercentDecode(rest);
  }

#ifndef _WIN32
  // A shell's escapes: "My\ Games" is "My Games".
  if (text.find('\\') != std::string::npos) {
    std::string plain;
    plain.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\\' && i + 1 < text.size()) {
        ++i;
      }
      plain += text[i];
    }
    text = std::move(plain);
  }
#endif

  // ~ and ~/ are the home folder.
  if (!home.empty() && (text == "~" || StartsWith(text, "~/"))) {
    text = std::string(home) + text.substr(1);
  }

  if (text.empty()) {
    result.error = "Type the path of the file.";
    return result;
  }
  const std::filesystem::path path = PathOf(text);
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    result.error = Shown(path) + " is a folder, not a file.";
    return result;
  }
  ec.clear();
  if (!std::filesystem::is_regular_file(path, ec)) {
    result.error = "There is no file at " + Shown(path) + ".";
    return result;
  }
  result.path = path;
  return result;
}

}  // namespace larecomp::os
