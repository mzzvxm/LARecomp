r"""check_menu_rows.py: does every pause-menu page fit the row cap?  (dev only, reads files, builds nothing)

  python tools\check_menu_rows.py [--src <src dir>] [--max <rows>] [--strict] [--selftest] [-v]

pause_menu.cpp keeps one table of guest row objects per page (NativeRows: four arrays of kMaxRowsPerMenu), and BuildNativeRows REFUSES A PAGE WHOLE when it
has more rows than that: the page does not open, with one warning in the log. Nothing else counts a page's rows, so a branch that adds a debug switch to a
page that is already full breaks the page and no build or test says so (INTEGRATION-TRIAL.md section 8.6: the debug FIXES page had 32 rows against a cap of
24 on the integrated tip). This tool counts them from the sources.

What it counts, the way MenuItemCount does (a page's rows are its `items` table, then what its `extra` function returns, less the rows of the renderer this
launch does not run):
  * kMenus in pause_menu.cpp: every MenuDef, with `#if` blocks resolved for each of the three builds that exist (no GPU code: the XENIA renderer only; GPU
    code with the NATIVE renderer; GPU code with the XENIA renderer). NativeOnly(...) rows count on a native run, XeniaOnly(...) rows on a XENIA run;
  * the tables (`ItemDef kName[] = {...}`, in any file under <src dir>), counted by their top-level entries, `#if` blocks resolved the same way;
  * `extra` functions: DebugXRows (`return Rows(kDebugXItems, count);` in debug_menu_pages.cpp), and any other function whose body names the tables it
    returns (CameraLookMenuItems, LowLatencyMenuRows: the largest of them). A function that builds its rows at run time (UpscalingPageRows) is counted
    with the largest number its comment promises: RUNTIME_EXTRA below.
The cap is read from `kMaxRowsPerMenu` in pause_menu.cpp, so raising it there moves this check with it (--max overrides, for a trial).

Fails (exit 1) when a page has more rows than the cap in any of the three builds, naming the page, its count and the rows past the cap. A page the
tool cannot read (a table that is not found, an extra function that builds its rows some new way) is counted with what it can read and listed as a
warning; with --strict a warning also fails, so that a refactor cannot make the check pass by counting nothing. tools\check_row_tags.py runs this
check too (its rule T5), without --strict, so every chain that runs the row tags also holds the pages to the cap. Exit 2 for a usage error.
--selftest runs the checks of this file's own rules.
"""
import argparse
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Pages whose `extra` function builds its rows at run time, so no table can be counted: the most rows the page can show (the number in the source's
# own comment, which a change to the page has to update with it). UPSCALING: the upscaler's rows and frame generation's, "6 at most" (pause_menu.cpp,
# the page-width note; upscaling_menu.cpp, frame_generation_menu.h).
RUNTIME_EXTRA = {"UpscalingPageRows": 6}

# The builds a page is listed in: (name, LARECOMP_HAS_GPU defined, the renderer mask of the run). A build without GPU code runs the XENIA renderer only.
BUILDS = (("no GPU code", False, "xenia"), ("native renderer", True, "native"), ("XENIA renderer", True, "xenia"))


def load_exposure_tool():
    spec = importlib.util.spec_from_file_location("check_settings_exposure", os.path.join(HERE, "check_settings_exposure.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CSE = None


def cse():
    global CSE
    if CSE is None:
        CSE = load_exposure_tool()
    return CSE


# ── The preprocessor, as far as the tables need it ─────────────────────

DEFINED = re.compile(r"\bdefined\s*(?:\(\s*(\w+)\s*\)|\s+(\w+))")


def eval_condition(expr, defines):
    """The truth of an #if expression made of defined(X), !, &&, ||, parentheses and 0 / 1. A macro the build set does not know (defined(REXGLUE_...))
    counts as defined: the larger page is the one to be sure of."""
    def known(m):
        name = m.group(1) or m.group(2)
        return " True " if name in defines or name not in KNOWN_MACROS else " False "
    text = DEFINED.sub(known, expr)
    text = text.replace("&&", " and ").replace("||", " or ")
    text = re.sub(r"!(?!=)", " not ", text)
    text = re.sub(r"\b(\d+)\b", lambda m: " True " if int(m.group(1)) else " False ", text)
    try:
        return bool(eval(text, {"__builtins__": {}}, {}))   # only True / False / and / or / not / parentheses are left
    except Exception:
        return True


# The macros a build of this check decides; any other macro is read as defined.
KNOWN_MACROS = {"LARECOMP_HAS_GPU"}


def resolve(text, gpu):
    """<text> with the #if / #elif / #else / #endif blocks resolved for a build that does (gpu) or does not define LARECOMP_HAS_GPU; the directive
    lines are blanked (the line count stays)."""
    defines = {"LARECOMP_HAS_GPU"} if gpu else set()
    out, stack = [], []   # stack of [parent_active, branch_taken, active]
    active = True
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#"):
            d = re.match(r"#\s*(\w+)\s*(.*)", s)
            word, rest = (d.group(1), d.group(2)) if d else ("", "")
            if word in ("if", "ifdef", "ifndef"):
                if word == "if":
                    cond = eval_condition(rest, defines)
                else:
                    name = rest.split()[0] if rest.split() else ""
                    cond = (name in defines or name not in KNOWN_MACROS) if word == "ifdef" else (name not in defines and name in KNOWN_MACROS)
                stack.append([active, cond, active and cond])
                active = stack[-1][2]
            elif word == "elif" and stack:
                parent, taken, _ = stack[-1]
                cond = (not taken) and eval_condition(rest, defines)
                stack[-1] = [parent, taken or cond, parent and cond]
                active = stack[-1][2]
            elif word == "else" and stack:
                parent, taken, _ = stack[-1]
                stack[-1] = [parent, True, parent and not taken]
                active = stack[-1][2]
            elif word == "endif" and stack:
                active = stack.pop()[0]
            out.append("")
            continue
        out.append(line if active else "")
    return "\n".join(out)


# ── Tables, menus, extras ──────────────────────────────────────────────

WRAPPER = re.compile(r"^\s*(?:\w+::)*(\w+)\s*\(")


def entry_renderers(entry):
    """'native', 'xenia' or 'any': the renderer tag of one row's expression (NativeOnly(WithDynamics(Str(...), &k)), WithDynamics(NativeOnly(...), &k))."""
    text = entry
    while True:
        m = WRAPPER.match(text)
        if not m:
            return "any"
        if m.group(1) == "NativeOnly":
            return "native"
        if m.group(1) == "XeniaOnly":
            return "xenia"
        if m.group(1) != "WithDynamics":
            return "any"
        text = text[m.end():]


def split_entries(body):
    """The non-empty top-level pieces of a brace list's inside (a trailing comma leaves an empty one)."""
    return [p.strip() for p in cse().split_args(body) if p.strip()]


def table_rows(text, name, gpu):
    """The renderer tags of the rows of table <name> in <text> for a build, or None when the table is not defined in it."""
    for m in cse().TABLE.finditer(text):
        if m.group(1) != name:
            continue
        end = cse().close_group(text, m.end(), "{", "}")
        body = resolve(text[m.end():end - 1], gpu)
        return [entry_renderers(e) for e in split_entries(body)]
    return None


def count_for(tags, mask):
    return sum(1 for t in tags if t == "any" or t == mask)


def read_tree(src):
    """{relative path: comment-free text} of every source file under <src> that mentions a table or an extra function we may need."""
    files = {}
    for path in cse().source_files(src):
        if path.endswith(("_test.cpp", "_test.h")):
            continue   # a test reads the tables as text (ssr_test.cpp holds the line "const ItemDef kReflectionItems[] = {" in a string)
        text = cse().read(path)
        if "ItemDef" in text or "MenuDef" in text:
            files[cse().rel(src, path)] = cse().strip_comments(text)
    return files


def find_table(files, name, gpu):
    for rel, text in files.items():
        rows = table_rows(text, name, gpu)
        if rows is not None:
            return rows
    return None


DEBUG_ROWS_FN = re.compile(r"\b(\w+)\s*\(\s*int\s*&\s*\w+\s*\)\s*\{\s*return\s+Rows\s*\(\s*(\w+)\s*,")


def extra_rows(files, fn, gpu):
    """(tags, note) for the rows an `extra` function returns: a list of renderer tags, or (None, why) when they cannot be counted."""
    for text in files.values():
        for m in DEBUG_ROWS_FN.finditer(text):
            if m.group(1) == fn:
                rows = find_table(files, m.group(2), gpu)
                return (rows, "") if rows is not None else (None, f"the table {m.group(2)} of {fn} is not found")
    if fn in RUNTIME_EXTRA:
        return ["any"] * RUNTIME_EXTRA[fn], f"{fn} builds its rows at run time: counted at its promised {RUNTIME_EXTRA[fn]}"
    for text in files.values():
        for m in re.finditer(r"\b" + re.escape(fn) + r"\s*\(\s*int\s*&\s*\w+\s*\)\s*\{", text):
            body = text[m.end():cse().close_group(text, m.end(), "{", "}")]
            names = list(dict.fromkeys(re.findall(r"\breturn\s+(k\w+)\s*;", body)))
            tables = [find_table(files, n, gpu) for n in names]
            tables = [t for t in tables if t is not None]
            if tables:
                return max(tables, key=len), ""
    return None, f"the rows of the extra function {fn} cannot be counted (add it to RUNTIME_EXTRA with its largest count, or return a named table)"


MENU_STRUCT = re.compile(r"\bMenuDef\s+kMenus\s*\[\s*\]\s*=\s*\{")


def menu_defs(files, gpu):
    """[(menu key, label, items table or None, extra function or None)] of kMenus for a build (the label alone is not unique: CAMERA is two pages)."""
    for rel, text in files.items():
        m = MENU_STRUCT.search(text)
        if not m:
            continue
        end = cse().close_group(text, m.end(), "{", "}")
        body = resolve(text[m.end():end - 1], gpu)
        menus = []
        for entry in split_entries(body):
            if not entry.startswith("{"):
                continue
            args = [a.strip() for a in cse().split_args(entry[1:entry.rfind("}")])]
            if len(args) < 6:
                continue
            label = cse().literal_text(args[1]) or args[0]
            key = cse().literal_text(args[3]) or args[3]
            items = args[4] if re.fullmatch(r"\w+", args[4]) and args[4] != "nullptr" else None
            extra = None
            if len(args) > 6:
                last = re.sub(r"^(?:\w+::)*", "", args[6])
                extra = last if re.fullmatch(r"\w+", last) and last != "nullptr" else None
            menus.append((key, label, items, extra))
        return rel, menus
    return None, []


def read_cap(files, override=None):
    if override is not None:
        return override
    for text in files.values():
        m = re.search(r"\bconstexpr\s+int\s+kMaxRowsPerMenu\s*=\s*(\d+)\s*;", text)
        if m:
            return int(m.group(1))
    return None


def count_pages(files):
    """{menu key: {"label": the page's name, "builds": {build name: (count, note)}}} and a list of what could not be read (the page is counted with what
    could)."""
    pages, problems = {}, []
    for bname, gpu, mask in BUILDS:
        rel, menus = menu_defs(files, gpu)
        if rel is None:
            problems.append("kMenus is not found")
            return pages, problems
        for key, label, items, extra in menus:
            tags, notes = [], []
            if items:
                rows = find_table(files, items, gpu)
                if rows is None:
                    problems.append(f"{label}: the table {items} is not found")
                    continue
                tags += rows
            if extra:
                rows, note = extra_rows(files, extra, gpu)
                if rows is None:
                    if f"{label}: {note}" not in problems:
                        problems.append(f"{label}: {note}")
                else:
                    tags += rows
                    if note:
                        notes.append(note)
            pages.setdefault(key, {"label": label, "builds": {}})["builds"][bname] = (count_for(tags, mask), "; ".join(notes))
    return pages, problems


def judge(pages, cap):
    """The problems of <pages> against the cap, and (label, worst count, its build, menu key) of every page, the fullest first."""
    over, worst = [], []
    for key, page in pages.items():
        label = page["label"] + (" (" + key + ")" if sum(1 for p in pages.values() if p["label"] == page["label"]) > 1 else "")
        name, (n, _) = max(page["builds"].items(), key=lambda kv: kv[1][0])
        worst.append((label, n, name, key))
        if n > cap:
            over.append(f"page '{label}' has {n} rows on the {name} build, {n - cap} over the cap of {cap}: BuildNativeRows refuses it whole and it will not "
                        f"open. Move at least {n - cap} of its rows to a page with room (-v lists every page's count; a debug fix goes to FIXES or PICTURE FIXES, "
                        f"see debug_menu_pages.cpp) rather than raising kMaxRowsPerMenu")
    return over, sorted(worst, key=lambda w: -w[1])


def selftest():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    check(resolve("a\n#if defined(LARECOMP_HAS_GPU)\nb\n#else\nc\n#endif\nd", True).split() == ["a", "b", "d"], "the GPU branch of an #if / #else")
    check(resolve("a\n#if defined(LARECOMP_HAS_GPU)\nb\n#else\nc\n#endif\nd", False).split() == ["a", "c", "d"], "the other branch of an #if / #else")
    check(resolve("#ifndef LARECOMP_HAS_GPU\nx\n#endif", True).split() == [], "#ifndef")
    check(resolve("#if defined(_WIN32)\nw\n#endif", False).split() == ["w"], "an unknown macro counts as defined")
    check(resolve("#if defined(LARECOMP_HAS_GPU)\n#if defined(_WIN32)\nn\n#endif\n#endif", False).split() == [], "a nested #if under a false #if")
    check(entry_renderers('NativeOnly(Bool("PM_a", "c", "A: "))') == "native", "NativeOnly")
    check(entry_renderers('NativeOnly(WithDynamics(Str("PM_a", "c", "A: ", v, n, 2), &kD))') == "native", "NativeOnly(WithDynamics(...))")
    check(entry_renderers('WithDynamics(XeniaOnly(Dbl("PM_a", "c", "A: ", v, 2, "%g")), &kD)') == "xenia", "WithDynamics(XeniaOnly(...))")
    check(entry_renderers('Bool("PM_a", "c", "A: ")') == "any", "an untagged row")
    check(entry_renderers('mcla_menu::Bool("PM_a", "c", "A: ")') == "any", "a qualified row")

    def sample(rows):
        tbl = "const ItemDef kDebugFixesItems[] = {\n" + "\n".join(f'    Bool("PM_R{i}", "c{i}", "R{i}: "),' for i in range(rows)) + "\n"
        tbl += '#if defined(LARECOMP_HAS_GPU)\n    NativeOnly(Bool("PM_N", "n", "N: ")),\n#endif\n    XeniaOnly(Bool("PM_X", "x", "X: ")),\n};\n'
        fn = "const ItemDef* DebugFixesRows(int& count) { return Rows(kDebugFixesItems, count); }\n"
        menus = ('constexpr int kMaxRowsPerMenu = 24;\nconst MenuDef kMenus[] = {\n'
                 '    {"PM_T", "FIXES", "d", "RxF", nullptr, 0, mcla_menu::DebugFixesRows, false, nullptr, true},\n'
                 '    {"PM_U", "OTHER", "d", "RxO", kOtherItems, int(sizeof(kOtherItems) / sizeof(kOtherItems[0]))},\n};\n')
        other = 'const ItemDef kOtherItems[] = {\n    Bool("PM_O", "o", "O: "),\n};\n'
        return {"pause_menu.cpp": cse().strip_comments(menus + other), "debug_menu_pages.cpp": cse().strip_comments(tbl + fn)}

    files = sample(22)
    pages, problems = count_pages(files)
    check(not problems, f"a sample reads cleanly: {problems}")
    check(pages["RxF"]["builds"]["native renderer"][0] == 23 and pages["RxF"]["builds"]["XENIA renderer"][0] == 23 and pages["RxF"]["builds"]["no GPU code"][0] == 23,
          f"22 plain rows + 1 tagged row for the build: {pages.get('RxF')}")
    check(pages["RxO"]["builds"]["native renderer"][0] == 1, "a table page of one row")
    check(read_cap(files) == 24 and read_cap(files, 10) == 10, "the cap is read from pause_menu.cpp and --max overrides it")
    check(judge(pages, 24)[0] == [], "23 rows fit a cap of 24")
    over, _ = judge(count_pages(sample(24))[0], 24)
    check(len(over) == 1 and "FIXES" in over[0] and "25 rows" in over[0], f"24 + 1 rows is one over: {over}")
    check(len(judge(count_pages(sample(23))[0], 24)[0]) == 0, "exactly the cap passes (24 on the native build)")
    # an extra function that returns a named table, and one that builds its rows at run time
    extra_text = chr(10).join([
        'const MenuDef kMenus[] = {',
        '    {"PM_L", "LOOK", "d", "RxL", nullptr, 0, LookRows},',
        '    {"PM_U", "UP", "d", "RxU", nullptr, 0, UpscalingPageRows},',
        '    {"PM_V", "VID", "d", "RxV", kVid, 1, MysteryRows},',
        '};',
        'constexpr int kMaxRowsPerMenu = 24;',
        'const ItemDef kVid[] = {',
        '    Bool("PM_V1", "v", "V: "),',
        '};',
        'const ItemDef kLook[] = {',
        '    Bool("PM_a", "a", "A: "), Bool("PM_b", "b", "B: "),',
        '};',
        'const ItemDef* LookRows(int& count) {',
        '    count = 2;',
        '    return kLook;',
        '}',
    ])
    extra_files = {"pause_menu.cpp": cse().strip_comments(extra_text)}
    pages_x, warn_x = count_pages(extra_files)
    check(pages_x["RxL"]["builds"]["native renderer"][0] == 2, "an extra function that returns a named table is counted from the table")
    check(pages_x["RxU"]["builds"]["native renderer"][0] == RUNTIME_EXTRA["UpscalingPageRows"], "a run-time extra function is counted at its promised maximum")
    check(pages_x["RxV"]["builds"]["native renderer"][0] == 1 and any("MysteryRows" in w for w in warn_x), "an extra function that cannot be read: the page is counted with its table and a warning is made")
    broken = {"pause_menu.cpp": files["pause_menu.cpp"], "debug_menu_pages.cpp": ""}
    check(len(count_pages(broken)[1]) > 0, "a page whose table cannot be found is reported, not counted as zero")
    for what in failures:
        print("SELFTEST FAIL: " + what)
    print(f"check_menu_rows selftest: {'FAILED, ' + str(len(failures)) + ' check(s)' if failures else 'ok'}")
    return 1 if failures else 0


def check_tree(src, cap_override=None):
    """(cap, pages, over, worst, warnings) for the sources under <src>; cap None when kMaxRowsPerMenu is not found."""
    files = read_tree(src)
    cap = read_cap(files, cap_override)
    if cap is None:
        return None, {}, [], [], []
    pages, warnings = count_pages(files)
    over, worst = judge(pages, cap)
    return cap, pages, over, worst, warnings


def summary_line(cap, pages, worst):
    return f"{len(pages)} pages read, the cap is {cap} rows; the fullest: " + ", ".join(f"{label} {n}" for label, n, _, _ in worst[:5])


def main():
    ap = argparse.ArgumentParser(description="does every pause-menu page fit the row cap?")
    ap.add_argument("--src", default=os.path.join(HERE, "..", "src"))
    ap.add_argument("--max", type=int, default=None, help="the cap to hold the pages to (default: kMaxRowsPerMenu in pause_menu.cpp)")
    ap.add_argument("--strict", action="store_true", help="a page that cannot be read also fails")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        print(f"no such source folder: {src}")
        return 2
    cap, pages, over, worst, warnings = check_tree(src, args.max)
    if cap is None:
        print("kMaxRowsPerMenu is not found in the sources (pause_menu.cpp)")
        return 2
    print(f"check_menu_rows: {src}")
    print("  " + summary_line(cap, pages, worst))
    if args.verbose:
        for label, n, name, key in worst:
            notes = sorted({note for _, note in pages[key]["builds"].values() if note})
            print(f"    {label}: {n} ({name}){'  [' + '; '.join(notes) + ']' if notes else ''}")
    for w in warnings:
        print("  " + ("UNREADABLE " if args.strict else "warning: cannot count ") + w)
    for p in over:
        print("  " + p)
    failed = len(over) + (len(warnings) if args.strict else 0)
    if failed:
        print(f"{failed} problem(s)")
        return 1
    print("every page fits the row cap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
