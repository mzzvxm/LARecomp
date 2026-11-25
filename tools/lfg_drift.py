r"""lfg_drift.py: does the real settings dump say everything the game's menu says?  (dev only; reads files, builds nothing)

  python -I tools\lfg_drift.py [<settings-ui.json>] [--known <file> --known-regex <regex>]

The menu tables name the settings the player can change (Bool / Str / Dbl / Color rows). The dump (tools\check_settings_dump.py makes it from those same tables) must carry every one of them: a row
for it, or a row that reads it, or one of its variants. This lists:
  * the settings the menu tables name that no row of the dump draws or reads;
  * the settings a row of the dump names that the menu tables never did (rows from ui_rules.toml's variants and the launcher's own rows: listed, not an error);
  * with --known <file> --known-regex <regex>: the settings another catalog names (a text file; the regex has one group, the setting's name) against the dump's rows, both ways. The catalog
    is whatever a surface kept by hand until the dump replaced it.
Exit 1 when the menu names a setting the dump lacks. The rest is a report.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT = r"C:\mcv\larecomp\perf\lfg-xcheck\dump\settings-ui.json"
SOURCES = ["menus/pause_menu.cpp", "menus/debug_menu_pages.cpp", "camera/camera_look.cpp", "menus/upscaling_menu.cpp", "menus/frame_generation_menu.cpp"]
ROW = re.compile(r'\b(Bool|Str|Dbl|Color)\s*\(\s*"(PM_\w+)"\s*,\s*(?:"(\w+)"|(nullptr)|(\w+))')


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", nargs="?", default=DEFAULT)
    ap.add_argument("--known")
    ap.add_argument("--known-regex")
    args = ap.parse_args()
    doc = json.load(open(args.dump, encoding="utf-8-sig"))

    named = {}                       # setting -> the first row that names it in the tables
    unresolved = []                  # rows whose setting is a name in code (a function result): the rules say it
    for rel in SOURCES:
        text = strip_comments(open(os.path.join(ROOT, "src/mc_engine", rel), encoding="utf-8-sig").read())
        for kind, row_id, literal, nullptr, symbol in ROW.findall(text):
            if literal:
                named.setdefault(literal, row_id)
            else:
                unresolved.append(row_id)

    in_dump = {}
    ids = set()
    for tab in doc["tabs"]:
        for section in tab["sections"]:
            for row in section["rows"]:
                ids.add(row["id"])
                forms = [row] + row.get("variants", [])
                for form in forms:
                    for name in [form.get("setting")] + list(form.get("reads", [])):
                        if name:
                            in_dump.setdefault(name, row["id"])
    catalog = doc["settings"]

    missing = {s: r for s, r in named.items() if s not in in_dump}
    extra = {s: r for s, r in in_dump.items() if s not in named and not r.startswith("L_")}
    print(f"the menu tables name {len(named)} settings ({len(unresolved)} more rows get theirs from a rule); the dump's rows name {len(in_dump)}")
    for s, r in sorted(missing.items()):
        print(f"  MISSING   {s}  (row {r} in the tables; the dump has {'the setting in its catalog' if s in catalog else 'no such setting'} and no row that draws or reads it)")
    for s, r in sorted(extra.items()):
        print(f"  RULE      {s}  (row {r}: named by a rule, not by a table)")
    absent_rows = sorted({r for r in (list(named.values()) + unresolved)} - ids)
    for r in absent_rows:
        print(f"  NO ROW    {r}  (a row of the tables that the dump lacks)")

    if args.known and args.known_regex:
        text = open(args.known, encoding="utf-8-sig", errors="replace").read()
        known = set(re.findall(args.known_regex, text))
        only_known = sorted(known - set(in_dump))
        only_dump = sorted(n for n, r in in_dump.items() if n not in known and not n.startswith("launcher."))
        print(f"the other catalog names {len(known)} settings: {len(only_known)} have no row in the dump, the dump has {len(only_dump)} that it lacks")
        for s in only_known:
            print(f"  OTHER ONLY  {s}  ({'in the dump catalog, no row' if s in catalog else 'not in the dump catalog'})")
        for s in only_dump:
            print(f"  DUMP ONLY   {s}  (row {in_dump[s]})")
    return 1 if missing or absent_rows else 0


if __name__ == "__main__":
    sys.exit(main())
