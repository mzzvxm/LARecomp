#pragma once
//
// Edits of the scene file as line-addressed text patches (DIRECTOR-DESIGN.md sections 2.4 and 4.6; documentation/DIRECTOR-PROTOCOL.md "scene.edit").
// FROZEN with director.h. Pure: text in, text out; the file, the revision counter and the reload are the handlers'.
//
// The text scene file stays the single source of truth, so an edit changes lines and nothing else: comments, blank lines, the order of the file,
// its line ending (LF or CRLF) and its final newline are kept. All line numbers of one request are those of the text the caller read (the text
// of `base_rev`), whatever the earlier ops of the same request did to the numbering; the ops apply together or not at all.
//
//   insert  {text}                          a new statement (or several, one per line of `text`), placed in time order: after the last line of the
//                                           same track whose time is at or before the new one's; else after the header (`scene`, `world`, `statelog`,
//                                           `start`); else at the first statement; else at the end. A line with no time (the header's) goes after
//                                           the last header line.
//   replace {line, expect, text}            line N becomes `text` (the whole line, comment included if it is to stay)
//   delete  {line, expect}                  line N goes
//   move    {line, expect, t}               rewrites only the time word of the statement (`at 3 hour 12` -> `at 4 hour 12`, `cam 3.2 ...`, `cut 3`, `end 20`)
//   param   {line, expect, key, value}      rewrites the words after `key` in the statement: as many words as `value` has (one by default), or appends
//                                           ` key value` before the trailing comment when the line has no `key`
//
// `expect` is the line's text as the caller saw it (without its line ending; trailing blanks and a trailing CR do not matter). A line that is not that
// any more (someone edited the file) makes the whole request `stale`, with the line's actual text in the details so the caller can re-read and try again.
//

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "error_code.h"
#include "json.h"

namespace mc::cinematic::director {

enum class EditKind : uint8_t { kInsert, kReplace, kDelete, kMove, kParam };
const char* EditKindName(EditKind kind);

struct EditOp {
    EditKind kind = EditKind::kInsert;
    int line = 0;         // replace, delete, move, param: 1-based line of the text the caller read
    std::string expect;   // replace, delete, move, param: that line's text
    std::string text;     // insert, replace
    bool has_t = false;
    double t = 0.0;       // move
    std::string key;      // param
    std::string value;    // param: the new words ("40", "1 2 3", "car:p2 0 2 -6")
};

// {"op":"insert","text":"..."} and the others, as the wire has them. False with `error` when the op is malformed ("op 2: 'line' must be a number").
bool EditOpFromJson(const Json& json, EditOp* out, std::string* error);
Json EditOpToJson(const EditOp& op);

struct EditResult {
    bool ok = false;
    ErrorCode code = ErrorCode::kBadArgs;  // !ok: kBadArgs (malformed, a line that is not there) or kStale (an `expect` that does not match)
    std::string message;
    int failed_op = -1;                    // !ok: the index of the op in the request
    Json details;                          // !ok, kStale: {"line":N,"actual":"..."}
    std::string text;                      // ok: the new file
    std::vector<int> lines;                // ok: the numbers, in the NEW text, of the lines the ops wrote (inserted, replaced, moved, param'd)
    std::vector<int> changed;              // ok: the numbers in the NEW text of every line that differs from the old (ChangedLines)
    int64_t rev = 0;                       // ok: base_rev + 1
};

class SceneEdit {
  public:
    // Applies the ops to `text` (the file as of revision `base_rev`). Pure; never throws. `ops` empty is a bad_args.
    static EditResult Apply(std::string_view text, const std::vector<EditOp>& ops, int64_t base_rev);
};

// The checksum the protocol reports as "hash": FNV-1a 64 over the bytes, 16 lower-case hex digits.
std::string TextHash(std::string_view text);

// The 1-based numbers of the lines of `after` that are not in `before` (a longest-common-subsequence of the two files' lines: an inserted line counts
// one, a changed line counts one, a moved line counts both ends). The `changed` of the scene.reloaded event. Capped: past 4000 lines it compares by
// position.
std::vector<int> ChangedLines(std::string_view before, std::string_view after);

// How a scene statement reads for placement and for the timeline rows: its time and its track. A line that is blank, a comment or has no time (the
// header) has has_time false. Words are split like scene.h's parser (blanks, "double quotes", '#' comments).
struct StatementInfo {
    bool is_statement = false;  // not blank, not a comment
    bool has_time = false;
    double t = 0.0;
    std::string verb;           // "cam", "cut", "end", "scene", "world", "hour", "car", "set", ...
    std::string track;          // the track a key belongs to: "camera" (cam and cut), "car:<id>", "set:<cvar>" for a setting, else the verb
    size_t time_begin = 0;      // where the time word starts and ends in the line (has_time)
    size_t time_end = 0;
};
StatementInfo ReadStatement(std::string_view line);
// "3", "3.5", "1:30.5" -> seconds; false when it is not a time.
bool ParseSceneTime(std::string_view word, double* seconds);
// A time as the edit writes it: "3.2" ("3" for a whole number), at most 3 decimals, trailing zeros dropped; over a minute stays in seconds ("90.5").
std::string FormatSceneTime(double seconds);

}  // namespace mc::cinematic::director
