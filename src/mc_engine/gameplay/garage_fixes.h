#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
// Fixes for garage tune files the game ships broken. Only what is missing is
// written here: each fix amends the game's own text as the game opens it, in
// memory (garage_fixes_open.cpp), so nothing is written to disk. A file a mod
// ships is left alone. See garage_fixes.cpp for what each one fixes.
//
// Text in, text out, no game or SDK dependency, so it can be checked offline.
// ─────────────────────────────────────────────────────────────────────────

namespace mc::garage_fixes {

// Archive paths of every tune file a fix applies to.
std::vector<std::string> Paths();

// True when the file does not exist in the game and is made from nothing.
bool IsNewFile(const std::string& path);

// Applies the fix for `path` to `text` (empty for a new file). False when there
// was nothing to change: already fixed, or the text does not have the expected
// shape. `text` is untouched in that case.
bool Apply(const std::string& path, std::string& text);

}  // namespace mc::garage_fixes
