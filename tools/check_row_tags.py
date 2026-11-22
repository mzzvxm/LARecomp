r"""check_row_tags.py: do the words on the rows say what the settings do? (dev only, reads files, builds nothing)

  python tools\check_row_tags.py [--src <src dir>] [--config <toml>] [--catalog <SettingsSchema.cs>] [--selftest] [-v]

A setting is "restart-only" when its definition says kRestart, kStartupOnly or kUntilLaunch (the parsers of tools\check_settings_exposure.py,
which this file imports). Four rules keep the words honest (LIVE-SETTINGS.md section 9; the owner wants settings to apply at once, so a word that
says RESTART must be true, and a restart that no word admits is a silent dead row):

  T1  a pause-menu row whose static suffix says RESTART names a restart-only setting (or one of PARTLY_LIVE below).
  T2  a pause-menu row that names a restart-only setting has a suffix: a static one that says RESTART or NEXT START, or a dynamic one (.suffix in
      its RowDynamics), which the row's own code keeps honest. Only the normal menus are read: the debug tables cannot hold restart-only settings
      (check_settings_exposure.py rule 3).
  T3  (with --catalog) a launcher catalog row tagged Restart names a restart-only setting, one this tree does not define (the SDK's:
      resolution_scale, present_effect ...), or one of PARTLY_LIVE.
  T4  (with --catalog) a launcher catalog row of a restart-only setting is tagged Restart.
  T5  every page of the pause menu fits the row cap (pause_menu.cpp kMaxRowsPerMenu): a page over it is refused whole and does not open. The counting is
      tools\check_menu_rows.py's (it also runs alone); a page it cannot read is a warning here, not a failure.

The launcher catalog (SettingsSchema.cs) is in the launcher's repository: pass the file of the branch you mean. NextHost and OnReload tags are the
catalog's own kinds for a live setting that is read when something is made (a session, a district), so nothing is asked of them.
Exit code 0 when everything agrees, 1 when something differs, 2 for a usage error. --selftest runs the checks of this file's own rules.
"""
import argparse
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The settings whose row says RESTART although the setting is live: part of what it does is read at start (the reason is the row's comment).
PARTLY_LIVE = {
    "language": "the text table reloads at once; the language the UI movies were given and the mapper's flags are read at start (pause_menu.cpp ApplyLanguageNow)",
}

SUFFIX_TEXT = re.compile(r'^\s*\(([A-Z0-9 ]+)\)\s*$')   # " (RESTART)", " (NEXT START)", " (NEXT HOST)", " (ON RELOAD)"
CATALOG_CALL = re.compile(r'\b(Toggle|Pick|Num|Pct|Slider|Text|Choice)\(\s*"(\w+)"')
CATALOG_TAGS = ("Restart", "OnReload", "NextHost")


def load_exposure_tool():
    spec = importlib.util.spec_from_file_location("check_settings_exposure", os.path.join(HERE, "check_settings_exposure.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_menu_rows_tool():
    spec = importlib.util.spec_from_file_location("check_menu_rows", os.path.join(HERE, "check_menu_rows.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def suffix_words(args):
    """The suffix texts (RESTART, NEXT START, ...) among the string arguments of a row's constructor call, upper case, in order."""
    words = []
    for a in args:
        for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', a):
            m = SUFFIX_TEXT.match(lit)
            if m:
                words.append(m.group(1))
    return words


def dynamics_with_suffix(texts):
    """The names of the RowDynamics objects in <texts> (comments blanked) that set a .suffix."""
    names = set()
    for text in texts:
        for m in re.finditer(r"\bRowDynamics\s+(\w+)\s*=\s*\{", text):
            body_end = text.find("};", m.end())
            body = text[m.end():body_end if body_end >= 0 else len(text)]
            if re.search(r"\.\s*suffix\s*=", body):
                names.add(m.group(1))
    return names


def pause_rows(cse, src, cfg):
    """[(file, line, cvar, [suffix words], dynamic_suffix)] for every row of the normal menu files that a constructor names."""
    menus = cfg.get("menus", {})
    files = list(dict.fromkeys(menus.get("normal_files", [])))
    texts = {}
    for f in files:
        path = os.path.join(src, "mc_engine", f) if not os.path.exists(os.path.join(src, f)) else os.path.join(src, f)
        if os.path.exists(path):
            texts[f] = cse.strip_comments(cse.read(path))
    with_suffix = dynamics_with_suffix(texts.values())
    rows = []
    for f, text in texts.items():
        consts = dict(cse.CONST.findall(text))
        for m in cse.ROW.finditer(text):
            cvar = m.group(2) or consts.get(m.group(3))
            if not cvar:
                continue
            paren = text.index("(", m.start())
            end = cse.close_paren(text, paren + 1)
            args = cse.split_args(text[paren + 1:end - 1])
            after = re.match(r"\s*,\s*&\s*(?:\w+::)*(\w+)\s*\)", text[end:end + 120])   # WithDynamics(<row>, &kDynamics)
            dynamic = bool(after and after.group(1) in with_suffix)
            rows.append((f, cse.line_of(text, m.start()), cvar, suffix_words(args), dynamic))
    return rows


def judge_pause(rows, restart_only):
    """The T1 / T2 problems of <rows> against the set of restart-only setting names."""
    problems = []
    for f, line, cvar, words, dynamic in rows:
        says_restart = any("RESTART" in w for w in words)
        says_next = any(w == "NEXT START" for w in words)
        if says_restart and cvar not in restart_only and cvar not in PARTLY_LIVE:
            problems.append(f"T1 {f}:{line}: the row of {cvar} says RESTART, and the setting is not restart-only: make it live in the "
                            f"row's words (drop the suffix) or say why on PARTLY_LIVE in tools\\check_row_tags.py")
        if cvar in restart_only and not (says_restart or says_next or dynamic):
            problems.append(f"T2 {f}:{line}: {cvar} is restart-only and its row has no suffix that says RESTART or NEXT START "
                            f"(a dynamic one, .suffix in the row's RowDynamics, also counts)")
    return problems


def balanced(text, start):
    """The text of the call whose '(' is at <start> (string aware)."""
    depth, i, in_str = 0, start, False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    return text[start:start + 400]


def catalog_rows(text):
    """[(cvar, tag or None)] for every row call of the launcher's SettingsSchema.cs (the tag is the one of CATALOG_TAGS the call names)."""
    out, seen = [], set()
    for m in CATALOG_CALL.finditer(text):
        body = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', balanced(text, text.index("(", m.start())))
        tag = next((t for t in CATALOG_TAGS if re.search(r"\b" + t + r"\b", body)), None)
        if (m.group(2), tag) not in seen:
            seen.add((m.group(2), tag))
            out.append((m.group(2), tag))
    return out


def judge_catalog(rows, defined, restart_only):
    """The T3 / T4 problems of the catalog <rows>; <defined> are the names this tree defines, <restart_only> the restart-only ones."""
    problems = []
    tagged = {c for c, t in rows if t == "Restart"}
    for cvar, tag in rows:
        if tag == "Restart" and cvar in defined and cvar not in restart_only and cvar not in PARTLY_LIVE:
            problems.append(f"T3 catalog: {cvar} is tagged Restart, and the setting is not restart-only")
    for cvar in sorted(c for c, _ in rows if c in restart_only and c not in tagged):
        problems.append(f"T4 catalog: {cvar} is restart-only and its row has no Restart tag")
    return problems


def selftest():
    cse = load_exposure_tool()
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    check(suffix_words(['"PM_X"', '"a_cvar"', '"LABEL: "', "false", '" (RESTART)"']) == ["RESTART"], "a static RESTART suffix is read")
    check(suffix_words(['"LABEL: "', '" (NEXT START)"']) == ["NEXT START"], "NEXT START is read")
    check(suffix_words(['"SHADOWS: "', '"on"', '"(not a suffix"']) == [], "a label is no suffix")
    check(suffix_words(["kSomeVals", "nullptr"]) == [], "no string, no suffix")
    dyn = dynamics_with_suffix(["constexpr RowDynamics kA = {.enabled = E, .suffix = S};\nconstexpr RowDynamics kB = {.enabled = E};"])
    check(dyn == {"kA"}, "only the RowDynamics with a .suffix count")
    restart = {"extra_vinyl_layers", "skip_intro"}
    row = lambda cvar, words, dynamic=False: ("menus/pause_menu.cpp", 1, cvar, words, dynamic)
    check(judge_pause([row("extra_vinyl_layers", ["RESTART"])], restart) == [], "a restart-only row that says RESTART is fine")
    check(judge_pause([row("skip_intro", ["NEXT START"])], restart) == [], "a restart-only row that says NEXT START is fine")
    check(len(judge_pause([row("skip_intro", [])], restart)) == 1, "T2: a restart-only row with no word fails")
    check(judge_pause([row("skip_intro", [], True)], restart) == [], "T2: a dynamic suffix counts")
    check(len(judge_pause([row("mcla_gpu_ao", ["RESTART"])], restart)) == 1, "T1: RESTART on a live setting fails")
    check(judge_pause([row("language", ["RESTART"])], restart) == [], "T1: PARTLY_LIVE is allowed")
    check(judge_pause([row("net_lift_big_session_limits", ["NEXT HOST"])], restart) == [], "NEXT HOST on a live setting is allowed")
    sample = 'Toggle("disable_msaa", "MSAA", "d", inverted: true, tag: Restart),\nToggle("perf_no_trees", "F", "d", tag: OnReload),\nToggle("a_live", "A", "x \\"Restart\\" y"),'
    rows = catalog_rows(sample)
    check(rows == [("disable_msaa", "Restart"), ("perf_no_trees", "OnReload"), ("a_live", None)], f"the catalog parser reads the tags and ignores strings: {rows}")
    defined = {"disable_msaa", "perf_no_trees", "a_live", "skip_intro"}
    check(judge_catalog(rows, defined, {"disable_msaa"}) == [], "a consistent catalog is fine")
    check(len(judge_catalog([("a_live", "Restart")], defined, {"disable_msaa"})) == 1, "T3: Restart on a live setting fails")
    check(judge_catalog([("resolution_scale", "Restart")], defined, {"disable_msaa"}) == [], "T3: an SDK name is not judged")
    check(len(judge_catalog([("skip_intro", None)], defined, {"skip_intro"})) == 1, "T4: a restart-only setting with no tag fails")
    check(cse.ROW.search('Bool("PM_RxVinylLayers", "extra_vinyl_layers", "EXTRA VINYL LAYERS: ", false, " (RESTART)")') is not None,
          "the exposure tool's row pattern still matches a Bool row")
    for what in failures:
        print("SELFTEST FAIL: " + what)
    print(f"check_row_tags selftest: {'FAILED, ' + str(len(failures)) + ' check(s)' if failures else 'ok'}")
    rows_failed = load_menu_rows_tool().selftest()   # T5's own checks
    return 1 if failures or rows_failed else 0


def main():
    ap = argparse.ArgumentParser(description="do the words on the rows say what the settings do?")
    ap.add_argument("--src", default=os.path.join(HERE, "..", "src"))
    ap.add_argument("--config", default=os.path.join(HERE, "settings_exposure.toml"))
    ap.add_argument("--catalog", default=None, metavar="SettingsSchema.cs")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    cse = load_exposure_tool()
    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        print(f"no such source folder: {src}")
        return 2
    cfg = cse.load_config(args.config)
    defs = cse.definitions(src)
    restart_only = {n for n, ds in defs.items() if any(d["restart"] for d in ds)}
    rows = pause_rows(cse, src, cfg)
    problems = judge_pause(rows, restart_only)
    menu_rows = load_menu_rows_tool()
    cap, pages, over, worst, unreadable = menu_rows.check_tree(src)
    problems += ["T5 " + o for o in over]
    if cap is None:
        problems.append("T5 kMaxRowsPerMenu is not found in the sources (pause_menu.cpp): the pages cannot be held to the cap")
    print(f"check_row_tags: {src}")
    print(f"  {len(rows)} pause-menu rows read, {sum(1 for r in rows if r[2] in restart_only)} of them restart-only settings, "
          f"{sum(1 for r in rows if any('RESTART' in w for w in r[3]))} with a static RESTART suffix")
    if cap is not None:
        print("  " + menu_rows.summary_line(cap, pages, worst))
        for w in unreadable:
            print("  warning (T5): cannot count " + w)
    if args.verbose:
        for f, line, cvar, words, dynamic in rows:
            if words or cvar in restart_only:
                print(f"    {f}:{line} {cvar}: {', '.join(words) or '-'}{' (dynamic suffix)' if dynamic else ''}{' [restart-only]' if cvar in restart_only else ''}")
    if args.catalog:
        if not os.path.isfile(args.catalog):
            print(f"no such catalog file: {args.catalog}")
            return 2
        crows = catalog_rows(cse.read(args.catalog))
        problems += judge_catalog(crows, set(defs), restart_only)
        print(f"  {len(crows)} catalog rows read ({sum(1 for _, t in crows if t == 'Restart')} tagged Restart)")
    for p in problems:
        print("  " + p)
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print("the words on the rows agree with the settings' lifecycles, and every page fits the row cap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
