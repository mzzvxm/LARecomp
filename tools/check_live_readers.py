r"""check_live_readers.py: does every setting that promises to be live have a reader that follows it?  (dev only, reads files, builds nothing)

  python -I tools\check_live_readers.py [--src <src dir>] [--config <toml>] [--strict] [-v] [--only a,b]
  python -I tools\check_live_readers.py --selftest

The owner wants settings to apply at once (LIVE-SETTINGS-2.md). A setting that is not tagged restart-only says "a change is seen now": the F4 panel,
the console, the Director and the pause menu write it while the game runs. The tag is a label, not a mechanism: what happens is decided by the code that reads
the setting. This check looks for that code, so that a new setting that is read once at start (a constructor, a `static const x = ...Get()`, a
`call_once`) cannot be called live by mistake. It does not decide whether a reader is CORRECT, only that one exists that is not a start-time read.

Which settings: those that are not restart-only (no kRequiresRestart / kInitOnly / kLockedInGame, nor the old kRestart / kStartupOnly / kUntilLaunch) and are
either not debug-only (a player's setting: a row, a launcher row or the exception list of tools\check_settings_exposure.py) or named by a row of the
hidden debug menu (check_settings_exposure rule 3 already makes such a row live-only). A debug-only setting with no row is a launch argument and may be read
when its feature starts: not checked.

What counts as a live reader (any one is enough):
  * `LiveText name("setting")` (gpu/live_text.h: the change callback fills an atomic; the render thread reads that);
  * a change watch by name: `RegisterChangeCallback("setting", ...)`, or `settings::setting.OnChange(` / `.Bind(`;
  * a read, `settings::setting.Get()` (any `ns::setting.Get()` alias) or by name (`GetFlagByName("setting")`, `GetAs<T>("setting")`, `cvar::Get...("setting")`), whose
    enclosing function is not a start-time one: not a static variable's initializer, not inside `std::call_once`, and its name is not Init / Initialize / Create /
    Setup / Open / Register / Configure / Constructor / Startup / Load ... unless it says it is per frame or per call (Tick, Update, BeginFrame, Poll, Draw,
    Present, Hook_, Patch_, Apply, Process, Render, Resolve, Record, Execute ...).
A setting whose every read is start-time is "READ ONCE" (exit 1). A setting no source reads by either form is "NO READER" (a warning: it may be read through a helper
this scan cannot see). tools\live_readers.toml lists the settings that are read when a feature starts by nature (a calibration, a recording, a session) with the reason:
they are not failures.

Exit 0 when nothing is READ ONCE, 1 otherwise (and, with --strict, when something has NO READER); 2 for a usage or configuration error.
--only a,b   checks these names only.  -v   prints every setting with the evidence that made it live.
--selftest   runs the rules on a small made-up source tree.
"""

import argparse
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_settings_exposure as ex  # noqa: E402  (the definitions, the rows and the comment stripper are its)

try:
    import tomllib
except ImportError:   # Python before 3.11
    tomllib = None

# The enclosing function's name says it runs once, at start (unless a LIVE word is in it too).
START_NAME = re.compile(r"(Init|Initialize|Create|Setup|SetUp|Boot|Install|Register|Configure|Open|Ctor|Constructor|Startup|^Start|Load|Attach|ChooseStart)", re.I)
LIVE_NAME = re.compile(r"(Tick|Update|BeginFrame|EndFrame|Frame|Poll|Draw|Present|Hook_|Patch_|Apply|Process|Render|Resolve|Record|Execute|Step|Run|Callback|Changed|OnChange|Each|Per[A-Z])")
ONCE_BLOCK = re.compile(r"\bcall_once\b|\bonce_flag\b")
STATIC_VAR = re.compile(r"\bstatic\s+(?:const\s+|constexpr\s+|inline\s+)*[\w:<>,\s\*&]+?\s\w+\s*(?:=|\{)")
TABLE_READ = re.compile(r'\b(?:GetFlagByName|GetText)\s*\(\s*(?!")[A-Za-z_][\w.\[\]>-]*\s*[,)]')
CONTROL = re.compile(r"^\s*(?:if|for|while|switch|else|return|do|try|catch|namespace|struct|class|enum|union)\b")


def evidence_patterns(name):
    n = re.escape(name)
    return {
        "live_text": re.compile(r'\bLiveText\b[^;{}]*"' + n + '"'),
        "watch_by_name": re.compile(r'\bRegisterChangeCallback\s*\(\s*"' + n + '"'),
        "watch_member": re.compile(r"(?<![\w])" + n + r"\s*\.\s*(?:OnChange|Bind)\s*\("),
        "get": re.compile(r"(?<![\w])" + n + r"\s*\.\s*Get(?:Text)?\s*\("),
        "get_by_name": re.compile(r'\b(?:GetFlagByName|GetAs\s*<[^>]*>|Get\w*Flag\w*|GetText)\s*\(\s*"' + n + '"'),
    }


STATIC_STMT = re.compile(r"^static\s+(?:const\s+|constexpr\s+|inline\s+|thread_local\s+)*[\w:<>,\s\*&]+?\s+\w+\s*(?:=|\{)")
STATIC_LAMBDA = re.compile(r"\bstatic\b[^;(]*=\s*\[")
LAMBDA_OPEN = re.compile(r"\[[^\]]*\]\s*(?:\([^)]*\)\s*)?(?:mutable\s*)?(?:->\s*[\w:<>\*&\s]+)?\s*\{\s*$")


def enclosing_blocks(lines, idx, limit=200):
    """The line numbers that open a block enclosing line idx, innermost first (a crude brace walk back over at most `limit` lines)."""
    out = []
    balance = 0   # closes minus opens of the lines walked over
    for j in range(idx - 1, max(-1, idx - limit), -1):
        line = lines[j]
        balance += line.count("}") - line.count("{")
        if balance < 0:
            out.append(j)
            balance = 0
    return out


def function_name_of(lines, j):
    """The name of the function whose body the block opened at line j starts, or None for a lambda, a control block, a namespace or a type body."""
    first = j
    while first > 0 and j - first < 3 and not re.search(r"[;{}]\s*$", lines[first - 1]):
        first -= 1
    sig = " ".join(l.strip() for l in lines[first:j + 1])
    if CONTROL.match(sig) or LAMBDA_OPEN.search(lines[j]):
        return None
    m = re.search(r"([A-Za-z_~][\w:~<>]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?(?:->\s*[\w:<>\*&\s]+)?(?:\s*:\s*[^{]*)?\{?\s*$", sig)
    if not m:
        return None
    parts = m.group(1).split("::")
    name = parts[-1]
    if len(parts) >= 2 and parts[-2] == name:
        return "Constructor"   # Renderer::Renderer(): runs once per object, at start
    return name


def classify_read(lines, idx):
    """'live' or 'once' for the read at lines[idx]: once when it initializes a static variable, is inside call_once, or sits in a start-time function."""
    start = idx
    while start > 0 and idx - start < 3 and not re.search(r"[;{}]\s*$", lines[start - 1]):
        start -= 1
    stmt = " ".join(l.strip() for l in lines[start:idx + 1])
    if STATIC_STMT.match(stmt) or STATIC_LAMBDA.search(stmt) or ONCE_BLOCK.search(stmt):
        return "once"
    blocks = enclosing_blocks(lines, idx)
    for j in blocks:
        text = lines[j - 1] + " " + lines[j] if j > 0 else lines[j]
        if ONCE_BLOCK.search(text) or STATIC_LAMBDA.search(text):
            return "once"
        if re.search(r"RegisterChangeCallback|\.OnChange\(|\.Bind\(", text):
            return "live"
    for j in blocks:
        name = function_name_of(lines, j)
        if name is None:
            continue
        if LIVE_NAME.search(name):
            return "live"
        if name == "Constructor" or START_NAME.search(name):
            return "once"
        return "live"
    return "live"   # no enclosing function: a namespace-scope read


def scan(src, names):
    """name -> {'live_text': [..], 'watch': [..], 'live': [(file, line)], 'once': [(file, line)]}"""
    res = {n: {"live_text": [], "watch": [], "live": [], "once": []} for n in names}
    pats = {n: evidence_patterns(n) for n in names}
    for path in ex.source_files(src):
        rel = ex.rel(src, path)
        if "/settings/generated/" in "/" + rel or rel.endswith("_test.cpp"):
            continue
        raw = ex.read(path)
        if not any(n in raw for n in names):
            continue
        text = ex.strip_comments(raw)
        lines = text.split("\n")
        low = text
        for n in names:
            if n not in low:
                continue
            p = pats[n]
            for m in p["live_text"].finditer(text):
                res[n]["live_text"].append((rel, ex.line_of(text, m.start())))
            for key in ("watch_by_name", "watch_member"):
                for m in p[key].finditer(text):
                    res[n]["watch"].append((rel, ex.line_of(text, m.start())))
            for key in ("get", "get_by_name"):
                for m in p[key].finditer(text):
                    ln = ex.line_of(text, m.start())
                    res[n][classify_read(lines, ln - 1)].append((rel, ln))
        # A table of names read by a variable (`GetFlagByName(row.cvar)`): every setting whose name is a string literal of this file is read there.
        table = [(ex.line_of(text, m.start()), classify_read(lines, ex.line_of(text, m.start()) - 1)) for m in TABLE_READ.finditer(text)]
        if table:
            for n in names:
                if f'"{n}"' in text:
                    ln, kind = sorted(table, key=lambda t: t[1] != "live")[0]
                    res[n][kind].append((rel + " (table read)", ln))
    return res


def candidates(src, cfg):
    defs = ex.definitions(src)
    rows = ex.rows_of(src, cfg, set(defs))
    out = {}
    for name, ds in defs.items():
        if any(d["restart"] for d in ds):
            continue
        tagged = any(d["tagged"] for d in ds)
        has_row = bool(rows.get(name))
        if tagged and not any(r[3] for r in rows.get(name, [])):
            continue   # a debug-only setting with no debug row: a launch argument
        if not tagged or has_row:
            out[name] = ds
    return out


def load(path):
    if tomllib is None:
        sys.exit("Python 3.11 or newer is needed (tomllib)")
    try:
        with open(path, "rb") as f:
            return tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError) as e:
        sys.exit(f"cannot read {path}: {e}")


def verdicts(src, cfg, live_cfg, only=None):
    cand = candidates(src, cfg)
    if only:
        cand = {n: d for n, d in cand.items() if n in only}
    res = scan(src, sorted(cand))
    next_use = ex.names_of(live_cfg, "next_use")
    out = []
    for name in sorted(cand):
        r = res[name]
        evidence = []
        if r["live_text"]:
            evidence.append(f"LiveText {r['live_text'][0][0]}:{r['live_text'][0][1]}")
        if r["watch"]:
            evidence.append(f"change watch {r['watch'][0][0]}:{r['watch'][0][1]}")
        if r["live"]:
            evidence.append(f"read per use {r['live'][0][0]}:{r['live'][0][1]}")
        if evidence:
            out.append((name, "live", "; ".join(evidence)))
        elif name in next_use:
            out.append((name, "next_use", next_use[name]))
        elif r["once"]:
            out.append((name, "once", ", ".join(f"{f}:{l}" for f, l in r["once"][:3])))
        else:
            out.append((name, "none", ""))
    return out


def run(args):
    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        sys.exit(f"no such source folder: {src}")
    cfg = ex.load_config(args.config)
    live_cfg = load(args.live_config) if os.path.exists(args.live_config) else {}
    only = {n for n in args.only.split(",") if n} if args.only else None
    res = verdicts(src, cfg, live_cfg, only)
    live = [r for r in res if r[1] == "live"]
    nxt = [r for r in res if r[1] == "next_use"]
    once = [r for r in res if r[1] == "once"]
    none = [r for r in res if r[1] == "none"]
    print(f"check_live_readers: {src}")
    print(f"  {len(res)} settings promise to be live: {len(live)} have a live reader, {len(nxt)} are read when a feature starts by nature (live_readers.toml), "
          f"{len(once)} are READ ONCE, {len(none)} have no reader this scan can see")
    if args.verbose:
        for n, _, why in live:
            print(f"      live     {n}: {why}")
        for n, _, why in nxt:
            print(f"      next use {n}: {why}")
    for n, _, where in once:
        print(f"  READ ONCE  {n}: read only at start ({where}); make it follow the setting (a LiveText, a change callback, a per-frame read) or tag it kRequiresRestart")
    for n, _, _ in none:
        print(f"  NO READER  {n}: no `settings::{n}.Get()`, no by-name read and no watch found (read through a helper? then say so in live_readers.toml)")
    for name in ex.names_of(live_cfg, "next_use"):
        if only is None and name not in {r[0] for r in res}:
            print(f"  warning: live_readers.toml names {name}, which is not a checked setting any more")
    if once or (args.strict and none):
        return 1
    print("the settings that promise to be live all have a reader that follows them")
    return 0


# ---------------------------------------------------------------------------------------------------------------------------------------------
# --selftest: a made-up tree
# ---------------------------------------------------------------------------------------------------------------------------------------------

SELF_SETTINGS = '''
class S final : public rex::SettingOwner {
 public:
  REX_SETTING(bool, with_live_text, false, "a", rex::kDebugOnly);
  REX_SETTING(bool, with_callback, false, "a");
  REX_SETTING(bool, per_frame, false, "a");
  REX_SETTING(bool, read_in_ctor, false, "a");
  REX_SETTING(bool, static_init, false, "a");
  REX_SETTING(bool, call_once_read, false, "a");
  REX_SETTING(bool, init_named_but_per_frame, false, "a");
  REX_SETTING(bool, next_use_by_nature, false, "a");
  REX_SETTING(bool, never_read, false, "a");
  REX_SETTING(bool, restart_one, false, "a", rex::kRequiresRestart);
  REX_SETTING(bool, debug_launch_arg, false, "a", rex::kDebugOnly);
  REX_SETTING(bool, by_name, false, "a");
};
'''

SELF_CODE = '''
namespace ns {
static LiveText g_text("with_live_text");

void Setup() {
  larecomp::cvar::RegisterChangeCallback("with_callback", [](std::string_view, std::string_view v) { Apply(v); });
}

void Renderer::BeginFrame() {
  const bool a = larecomp::settings::per_frame.Get();
  if (larecomp::settings::init_named_but_per_frame.Get()) Go();
}

Renderer::Renderer() {
  ready_ = larecomp::settings::read_in_ctor.Get();
}

bool Enabled() {
  static const bool on = larecomp::settings::static_init.Get();
  return on;
}

void Thing::Start() {
  static std::once_flag once;
  std::call_once(once, [] {
    use(larecomp::settings::call_once_read.Get());
  });
}

void Recorder::Initialize() {
  larecomp::settings::next_use_by_nature.Get();
  larecomp::settings::debug_launch_arg.Get();
}

bool Tick() { return larecomp::cvar::GetFlagByName("by_name") == "on"; }
}
'''

SELF_ROWS = '''
const ItemDef kDebugTable[] = {
    Bool("PM_RxA", "with_live_text", "A: "),
};
const ItemDef kNormalTable[] = {
    Bool("PM_RxB", "with_callback", "B: "),
    Bool("PM_RxC", "per_frame", "C: "),
    Bool("PM_RxD", "read_in_ctor", "D: "),
    Bool("PM_RxE", "static_init", "E: "),
    Bool("PM_RxF", "call_once_read", "F: "),
    Bool("PM_RxG", "init_named_but_per_frame", "G: "),
    Bool("PM_RxH", "next_use_by_nature", "H: "),
    Bool("PM_RxI", "never_read", "I: "),
    Bool("PM_RxJ", "by_name", "J: "),
};
'''

SELF_CFG = '''
[menus]
normal_files = ["menus/pause_menu.cpp"]
dynamic_files = []
debug_files = ["menus/debug_menu_pages.cpp"]
debug_table_pattern = "^kDebug"
'''


def selftest():
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "src")
        os.makedirs(os.path.join(src, "settings", "generated"))
        os.makedirs(os.path.join(src, "menus"))
        with open(os.path.join(src, "settings", "generated", "settings_x.h"), "w") as f:
            f.write(SELF_SETTINGS)
        with open(os.path.join(src, "code.cpp"), "w") as f:
            f.write(SELF_CODE)
        row_lines = [l for l in SELF_ROWS.splitlines() if l.startswith("    Bool(") and "with_live_text" not in l]
        with open(os.path.join(src, "menus", "pause_menu.cpp"), "w") as f:
            f.write("\n".join(["const ItemDef kNormalTable[] = {"] + row_lines + ["};", ""]))
        with open(os.path.join(src, "menus", "debug_menu_pages.cpp"), "w") as f:
            f.write("\n".join(["const ItemDef kDebugTable[] = {", '    Bool("PM_RxA", "with_live_text", "A: "),', "};", ""]))
        cfg_path = os.path.join(tmp, "exposure.toml")
        with open(cfg_path, "w") as f:
            f.write(SELF_CFG)
        live_path = os.path.join(tmp, "live_readers.toml")
        with open(live_path, "w") as f:
            f.write('[[next_use]]\nwhy = "read when the recorder starts"\nnames = ["next_use_by_nature"]\n')
        cfg = ex.load_config(cfg_path)
        got = {n: v for n, v, _ in verdicts(src, cfg, load(live_path))}
        want = {
            "with_live_text": "live",
            "with_callback": "live",
            "per_frame": "live",
            "read_in_ctor": "once",
            "static_init": "once",
            "call_once_read": "once",
            "init_named_but_per_frame": "live",
            "next_use_by_nature": "next_use",
            "never_read": "none",
            "by_name": "live",
        }
        failed = 0
        for name, verdict in want.items():
            if got.get(name) != verdict:
                failed += 1
                print(f"FAILED: {name}: got {got.get(name)}, want {verdict}")
        for name in ("restart_one", "debug_launch_arg"):
            if name in got:
                failed += 1
                print(f"FAILED: {name} is not a checked setting (restart-only / a debug launch argument) but was checked: {got[name]}")
        if failed:
            print(f"check_live_readers --selftest: {failed} FAILED")
            return 1
        print(f"check_live_readers --selftest: {len(want) + 2} checks passed")
        return 0


def main():
    ap = argparse.ArgumentParser(description="does every setting that promises to be live have a reader that follows it?")
    ap.add_argument("--src", default=os.path.join(HERE, "..", "src"))
    ap.add_argument("--config", default=os.path.join(HERE, "settings_exposure.toml"))
    ap.add_argument("--live-config", dest="live_config", default=os.path.join(HERE, "live_readers.toml"))
    ap.add_argument("--only", default="")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
