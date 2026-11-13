r"""check_settings_exposure.py: is every cvar of the game either a row, on the exception list, or `.debug_only()`?  (dev only, reads files, builds nothing)

  python tools\check_settings_exposure.py [--src <src dir>] [--config <toml>] [--dirs <dir> ...] [--ignore-pending] [--strict] [--rows] [-v]

Reads, under <src dir> (default: the src folder next to tools\):
  * every REXCVAR_DEFINE_* (BOOL, INT32, ..., COMMAND) with its chained calls, and whether the chain has `.debug_only()`;
  * the rows of the pause menu (pause_menu.cpp and the menu files named in settings_exposure.toml): the cvar each Bool / Str / Dbl / Color row
    writes, and the cvars a row writes that its constructor does not name (the upscaling mode row, RENDERER, VSYNC's native cvar);
  * tools\settings_exposure.toml: the exception list (SETTINGS-REVIEW.md B.1), the cvars that only the launcher has a row for, the virtual rows,
    and the "pending" list (a tagged cvar whose row is still in a normal pause-menu table until SX-G4 and SX-G5 move it).

Fails (exit 1) on:
  1. an untagged cvar with no row and not on the exception list (a new cvar must be classified when it is written);
  2. a tagged cvar that a normal (non-debug) row uses, unless it is on the pending list; also a tagged cvar on the exception list;
  3. a cvar that a debug table names and that is not tagged, or whose chain says kRequiresRestart / kInitOnly (a debug row cannot be restart-only);
  4. a cvar defined twice (the Linux stubs, the legacy runtime) with the tag on one definition and not on the other;
  5. the table kLockedPanelCvars in src\mc_engine\ui\settings.cpp (what the F4 panel lists while the debug menu is locked) and the exception list
     of settings_exposure.toml name different cvars (whole-tree runs only).
Warnings (exit 0, 1 with --strict): list entries that name a cvar the sources do not define, pending entries that are not needed any more.

--dirs <path> ...  checks only the definitions in these folders / files of <src dir> (relative to it, for example gpu native_gfx), so that each part of a
                   split job can be checked alone. Rows, the exception list and the pending list are still read for the whole tree.
--ignore-pending   treat the pending list as empty (the check to run after SX-G5 has removed the debug rows from the normal tables).
--rows             print every row cvar found (file:line, kind) and exit.
-v                 print the names of each group, not only the counts.
Exit code 0 when everything agrees, 1 when something differs, 2 for a usage or configuration error.
"""
import argparse
import os
import re
import sys

try:
    import tomllib
except ImportError:   # Python before 3.11
    tomllib = None

HERE = os.path.dirname(os.path.abspath(__file__))
DEFINE = re.compile(r"\bREXCVAR_DEFINE_(\w+)\s*\(")
ROW = re.compile(r'\b(Bool|Str|Dbl|Color)\s*\(\s*"PM_\w+"\s*,\s*(?:"(\w+)"|(\w+))')
TABLE = re.compile(r"\bItemDef\s+(\w+)\s*\[\s*\]\s*=\s*\{")
CONST = re.compile(r'\bconstexpr\s+const\s+char\s*\*\s*(\w+)\s*=\s*"(\w+)"\s*;')
CVAR_HELPER = re.compile(r"^[ \t]*(?:constexpr\s+)?const\s+char\s*\*\s*\w*Cvar\w*\s*(?:\(\s*\)\s*\{|=)(.*)$", re.M)
STRING = re.compile(r'"(\w+)"')
RESTART = re.compile(r"Lifecycle::(kRequiresRestart|kInitOnly)\b")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().replace("\r\n", "\n")


def strip_comments(text):
    """The text with every comment replaced by spaces (newlines kept, so offsets and line numbers stay), strings and character literals kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            raw = re.match(r'R"([^()\\\s]{0,16})\(', text[i - 1:i + 20]) if i > 0 and text[i - 1] == "R" else None
            if raw:
                end = text.find(")" + raw.group(1) + '"', i)
                j = n if end < 0 else end + len(raw.group(1)) + 2
            else:
                j = i + 1
                while j < n and text[j] != '"' and text[j] != "\n":
                    j += 2 if text[j] == "\\" else 1
                j += 1
            out.append(text[i:j])
            i = j
        elif c == "'":
            k = i
            while k > 0 and (text[k - 1].isalnum() or text[k - 1] in "_'"):
                k -= 1
            if k < i and text[k].isdigit():   # a digit separator (500'000)
                out.append(c)
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j += 1
            out.append(text[i:j])
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def close_paren(text, i):
    """The index just after the ')' that matches the '(' before <i> (comments are already blanked; strings are skipped)."""
    depth, n = 1, len(text)
    while i < n and depth:
        c = text[i]
        if c == '"':
            raw = re.match(r'R"([^()\\\s]{0,16})\(', text[i - 1:i + 20]) if i > 0 and text[i - 1] == "R" else None
            if raw:
                end = text.find(")" + raw.group(1) + '"', i)
                i = n if end < 0 else end + len(raw.group(1)) + 2
            else:
                i += 1
                while i < n and text[i] != '"' and text[i] != "\n":
                    i += 2 if text[i] == "\\" else 1
                i += 1
            continue
        if c == "'":
            k = i
            while k > 0 and (text[k - 1].isalnum() or text[k - 1] in "_'"):
                k -= 1
            if not (k < i and text[k].isdigit()):
                i += 1
                while i < n and text[i] != "'" and text[i] != "\n":
                    i += 2 if text[i] == "\\" else 1
                i += 1
                continue
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return i


def statement_end(text, i):
    """The index after the macro call's ')' and the chained .calls that follow it (stops before the ';')."""
    end = close_paren(text, i)
    while True:
        m = re.match(r"\s*\.\s*\w+\s*\(", text[end:])
        if not m:
            return end
        end = close_paren(text, end + m.end())


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def source_files(src):
    for dirpath, dirs, files in os.walk(src):
        dirs.sort()
        for name in sorted(files):
            if name.endswith((".cpp", ".h", ".inc", ".cc", ".c")):
                yield os.path.join(dirpath, name)


def rel(src, path):
    return os.path.relpath(path, src).replace("\\", "/")


def definitions(src):
    """name -> list of {file, line, type, tagged, restart} over every definition under <src>."""
    found = {}
    for path in source_files(src):
        text = strip_comments(read(path))
        if "REXCVAR_DEFINE_" not in text:
            continue
        for m in DEFINE.finditer(text):
            name = re.match(r"\s*(\w+)", text[m.end():])
            if not name:
                continue
            end = statement_end(text, m.end())
            chain = text[m.end():end]
            found.setdefault(name.group(1), []).append({
                "file": rel(src, path),
                "line": line_of(text, m.start()),
                "type": m.group(1),
                "tagged": re.search(r"\.\s*debug_only\s*\(", chain) is not None,
                "restart": RESTART.search(chain) is not None,
            })
    return found


def rows_of(src, cfg, defined):
    """name -> list of (file, line, kind, debug) for every row. kind: 'row' (a constructor names it), 'dynamic' (a row writes it), 'virtual'."""
    rows = {}
    menus = cfg.get("menus", {})
    normal = menus.get("normal_files", [])
    dynamic = set(menus.get("dynamic_files", []))
    debug_files = set(menus.get("debug_files", []))
    table_rx = re.compile(menus["debug_table_pattern"]) if menus.get("debug_table_pattern") else None
    virtual = {}
    for v in cfg.get("virtual_row", []):
        for key in v.get("rows", []):
            virtual[key] = list(v["names"])

    def add(name, f, line, kind, debug):
        rows.setdefault(name, []).append((f, line, kind, debug))

    for f in list(dict.fromkeys(normal + sorted(debug_files))):
        path = os.path.join(src, "mc_engine", f) if not os.path.exists(os.path.join(src, f)) else os.path.join(src, f)
        if not os.path.exists(path):
            continue
        text = strip_comments(read(path))
        name_in_tree = rel(src, path)
        consts = dict(CONST.findall(text))
        tables = [(m.start(), m.group(1)) for m in TABLE.finditer(text)]

        def table_at(pos):
            current = None
            for start, tname in tables:
                if start <= pos:
                    current = tname
            return current

        for m in ROW.finditer(text):
            key = m.group(2) or consts.get(m.group(3)) or None
            if key is None:
                continue   # nullptr, or a cvar picked at run time (the dynamic ones are below)
            tname = table_at(m.start())
            debug = f in debug_files or bool(table_rx and tname and table_rx.search(tname))
            names = virtual.get(key, [key])
            for nm in names:
                add(nm, name_in_tree, line_of(text, m.start()), "virtual" if key in virtual else "row", debug)
        if f in dynamic:
            for m in STRING.finditer(text):
                if m.group(1) in defined:
                    add(m.group(1), name_in_tree, line_of(text, m.start()), "dynamic", f in debug_files)
        for m in CVAR_HELPER.finditer(text):
            for s in STRING.finditer(m.group(1)):
                for nm in virtual.get(s.group(1), [s.group(1)]):
                    if nm in defined or s.group(1) in virtual:
                        add(nm, name_in_tree, line_of(text, m.start()), "dynamic", f in debug_files)
    return rows


def load_config(path):
    if tomllib is None:
        sys.exit("Python 3.11 or newer is needed (tomllib)")
    try:
        with open(path, "rb") as f:
            return tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError) as e:
        sys.exit(f"cannot read {path}: {e}")


# The F4 panel lists these while the debug menu is locked (SX-G3); it is the exception list, written out in C++.
LOCKED_PANEL_FILE = "mc_engine/ui/settings.cpp"
LOCKED_PANEL_TABLE = "kLockedPanelCvars"


def locked_panel_names(src):
    """The set of names in kLockedPanelCvars, or None when the file or the table is not there."""
    path = os.path.join(src, LOCKED_PANEL_FILE)
    if not os.path.exists(path):
        return None
    text = strip_comments(read(path))
    m = re.search(r"\b" + LOCKED_PANEL_TABLE + r"\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not m:
        return None
    return set(STRING.findall(m.group(1)))


def names_of(cfg, key):
    out = {}
    for entry in cfg.get(key, []):
        for name in entry.get("names", []):
            out[name] = entry.get("why", "")
    return out


def in_dirs(path, dirs):
    if not dirs:
        return True
    return any(path == d or path.startswith(d.rstrip("/") + "/") for d in dirs)


def show(label, names, verbose, width=6):
    if verbose and names:
        for i in range(0, len(names), width):
            print("      " + ", ".join(names[i:i + width]))


def main():
    ap = argparse.ArgumentParser(description="every cvar is a row, an exception or .debug_only()")
    ap.add_argument("--src", default=os.path.join(HERE, "..", "src"))
    ap.add_argument("--config", default=os.path.join(HERE, "settings_exposure.toml"))
    ap.add_argument("--dirs", nargs="+", default=[], metavar="DIR")
    ap.add_argument("--ignore-pending", action="store_true")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--rows", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        sys.exit(f"no such source folder: {src}")
    dirs = [d.replace("\\", "/").strip("/") for part in args.dirs for d in part.split(",") if d.strip()]
    for d in dirs:
        if not os.path.exists(os.path.join(src, d)):
            sys.exit(f"--dirs: {d} is not under {src}")
    cfg = load_config(args.config)

    defs = definitions(src)
    rows = rows_of(src, cfg, set(defs))
    if args.rows:
        for name in sorted(rows):
            for f, line, kind, debug in rows[name]:
                print(f"{name}\t{f}:{line}\t{kind}{' (debug table)' if debug else ''}")
        print(f"{len(rows)} cvars are named by a row")
        return 0

    exceptions = names_of(cfg, "exception")
    launcher = names_of(cfg, "launcher_row")
    pending = {} if args.ignore_pending else names_of(cfg, "pending")

    checked = sorted(n for n, ds in defs.items() if any(in_dirs(d["file"], dirs) for d in ds))
    problems = []
    warnings = []
    tagged, row_pause, row_launcher, exception, still_pending = [], [], [], [], []
    for name in checked:
        all_ds = defs[name]
        ds = [d for d in all_ds if in_dirs(d["file"], dirs)]   # the definitions this run is about
        where = ", ".join(f"{d['file']}:{d['line']}" for d in ds)
        tags = {d["tagged"] for d in all_ds}
        is_tagged = True in {d["tagged"] for d in ds}
        if len(tags) > 1:
            also = ", ".join(f"{d['file']}:{d['line']}" for d in all_ds)
            text = f"{name}: defined more than once ({also}) with `.debug_only()` on only some of the definitions"
            if len(ds) == len(all_ds):
                problems.append(text)
            else:
                warnings.append(text + " (another definition is outside --dirs; the whole-tree run fails on it)")
        normal_rows = [r for r in rows.get(name, []) if not r[3]]
        debug_rows = [r for r in rows.get(name, []) if r[3]]
        for f, line, kind, debug in debug_rows:
            if not is_tagged:
                problems.append(f"{name}: a debug table row ({f}:{line}) names it, and it is not `.debug_only()`")
            elif any(d["restart"] for d in ds):
                problems.append(f"{name}: a debug table row ({f}:{line}) names it, and its lifecycle is restart-only ({where})")
        if is_tagged:
            tagged.append(name)
            uses = [f"{f}:{line}" for f, line, _, _ in normal_rows]
            if name in launcher:
                uses.append("the launcher's row list (settings_exposure.toml)")
            if uses:
                if name in pending:
                    still_pending.append(name)
                else:
                    problems.append(f"{name}: `.debug_only()` ({where}) but a normal row uses it: {', '.join(uses)}")
            if name in exceptions:
                problems.append(f"{name}: `.debug_only()` ({where}) but it is on the exception list, which is for cvars players see with the debug menu locked")
        else:
            if normal_rows:
                row_pause.append(name)
            elif name in launcher:
                row_launcher.append(name)
            elif name in exceptions:
                exception.append(name)
            else:
                problems.append(f"{name}: no `.debug_only()`, no row and not on the exception list ({where})")

    if not dirs:
        for key, label in (("exception", "exception list"), ("launcher_row", "launcher row list"), ("pending", "pending list")):
            if key == "pending" and args.ignore_pending:
                continue
            for name in names_of(cfg, key):
                if name not in defs:
                    warnings.append(f"the {label} names {name}, which no source defines (an SDK cvar? then it does not belong here)")
        for name in pending:
            if name in defs and not (any(d["tagged"] for d in defs[name]) and any(not r[3] for r in rows.get(name, []))):
                if not any(d["tagged"] for d in defs[name]):
                    warnings.append(f"the pending list names {name}, which is not tagged: add `.debug_only()` to it, or remove the entry from settings_exposure.toml")
                else:
                    warnings.append(f"the pending list names {name}, but no normal row uses it any more: remove the entry from settings_exposure.toml")

    if not dirs:
        panel = locked_panel_names(src)
        if panel is None:
            warnings.append(f"cannot read {LOCKED_PANEL_TABLE} in {LOCKED_PANEL_FILE}: the F4 panel's locked list is not compared with the exception list")
        else:
            for name in sorted(set(exceptions) - panel):
                problems.append(f"{name}: on the exception list but not in {LOCKED_PANEL_TABLE} ({LOCKED_PANEL_FILE}): a locked F4 panel would not list it")
            for name in sorted(panel - set(exceptions)):
                problems.append(f"{name}: in {LOCKED_PANEL_TABLE} ({LOCKED_PANEL_FILE}) but not on the exception list of settings_exposure.toml")

    n_defs = sum(1 for n in checked for d in defs[n] if in_dirs(d["file"], dirs))
    print(f"check_settings_exposure: {src}" + (f"  (--dirs {' '.join(dirs)})" if dirs else ""))
    print(f"  {n_defs} definitions, {len(checked)} cvars checked")
    print(f"  {len(tagged)} are `.debug_only()`" + (f", {len(still_pending)} of them still have a normal pause-menu row (pending SX-G4/G5)" if still_pending else ""))
    show("pending", still_pending, args.verbose)
    kept = len(row_pause) + len(row_launcher) + len(exception)
    print(f"  {kept} are not tagged: {len(row_pause)} with a pause-menu row (KEEP), {len(row_launcher)} with a launcher row only (KEEP), {len(exception)} on the exception list")
    show("row", row_pause + row_launcher + exception, args.verbose)
    for w in warnings:
        print("  warning: " + w)
    for p in problems:
        print("  " + p)
    if problems or (warnings and args.strict):
        print(f"{len(problems)} problem(s)" + (f", {len(warnings)} warning(s)" if warnings else ""))
        return 1
    print("every cvar is a row, on the exception list or `.debug_only()`; no tagged cvar has a normal row" + (" (apart from the pending ones)" if still_pending else ""))
    return 0


sys.exit(main())
