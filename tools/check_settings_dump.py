r"""check_settings_dump.py: make the REAL settings-ui.json from the sources and lint it  (dev only; builds a small standalone program, never the game)

  python -I tools\check_settings_dump.py [--out <dir>] [--no-build] [--keep]

Why this exists. `larecomp.exe --dump_settings_ui` needs the whole game build. Everything the dump says comes from four places that a small program can
use as they are: the SDK's settings registry (the real src/settings/larecomp_settings.cpp registered into the SDK's own registry, so every setting,
default, life and constraint is the real one), the embedded data files (ui_text.toml, launcher_rows.toml, phase_a.toml, ui_rules.toml: read from the
tree), the exporter (src/settings_ui/export.cpp + action.cpp, the real files) and the menu tables. The tables are the one part that lives in the game
translation units (pause_menu.cpp, upscaling_menu.cpp, frame_generation_menu.cpp, debug_menu_pages.cpp, camera_look.cpp), which cannot be compiled
without the game: this tool copies the row tables out of their source text (the same readers tools\check_menu_rows.py and check_settings_exposure.py
use), builds `AllSettingsMenus()` from the `kMenus` table and its `extra` functions exactly as pause_menu.cpp's own AllSettingsMenus does, and links
that with the real exporter. A row the copy misses fails the cross-check below (every PM_ row of the source tables must be in the dump).

What it runs, in order, and what fails the run (exit 1):
  1. builds the program (clang++ from the labuild tools, the SDK install the settings model was built with), runs it twice;
  2. the two dumps are the same bytes (the dump is deterministic);
  3. documentation/contract/validate_contract.py --strict on the dump: 0 errors, 0 warnings;
  4. tools/check_settings_ui.py on the dump (the 41-column width lint, the native markers, the settings hash);
  5. every PM_ row id the source tables name is a row of the dump, and the reverse;
  6. every setting the menu rows name is in the dump's `settings` catalog.
The dump lands in <out>\settings-ui.json (and settings-ui.2.json, the repeat). Default <out>: C:\mcv\larecomp\perf\lfg-xcheck\dump.

The paths of this PC's toolchain are the defaults below; --clang / --sdk / --toml / --alloc override them.
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CLANG = r"C:\mcv\larecomp\labuild\tools\clang+llvm-20.1.8-x86_64-pc-windows-msvc\bin\clang++.exe"
SDK = r"C:\mcv\larecomp\labuild\sdk-setm\out\install\win-amd64"
TOML = r"C:\mcv\larecomp\native\tree-t11\thirdparty\tomlplusplus\include"
ALLOC = r"C:\mcv\larecomp\sdk\wt-settings-model\cmake\rexglue_host_alloc_stub.cpp"
DEFAULT_OUT = r"C:\mcv\larecomp\perf\lfg-xcheck\dump"

SOURCES = ["menus/pause_menu.cpp", "menus/debug_menu_pages.cpp", "camera/camera_look.cpp", "menus/upscaling_menu.cpp",
           "menus/frame_generation_menu.cpp"]
# The extra functions that build their rows at run time, and the tables whose union they can return (the exporter walks the superset).
ALTERNATIVES = {"DisplayPageRows": ["kReflexRows", "kAntiLagRows", "kRows", "fg"], "CameraLookMenuItems": ["kCameraLookRows"]}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(module)
    return module


def read(path):
    with open(path, encoding="utf-8-sig") as f:
        return f.read()


def write_data_header(out, commit):
    data = read(os.path.join(ROOT, "src/settings_ui/ui_data.h.in"))
    for key, rel in (("UI_TEXT", "src/settings/ui_text.toml"), ("LAUNCHER_ROWS", "src/settings/launcher_rows.toml"),
                     ("PHASE_A", "src/settings_ui/phase_a.toml"), ("UI_RULES", "src/settings/ui_rules.toml")):
        data = data.replace("@GX_" + key + "@", read(os.path.join(ROOT, rel)))
    data = data.replace("@GX_COMMIT@", commit)
    with open(os.path.join(out, "settings_ui_data.h"), "w", encoding="utf-8") as f:
        f.write(data)


def table_text(whole, name, cse):
    m = re.search(r"ItemDef\s+" + name + r"\s*\[\]\s*=\s*\{", whole)
    if not m:
        raise SystemExit(f"the table {name} is not in the sources")
    return whole[m.end():cse.close_group(whole, m.end(), "{", "}") - 1]


def write_fixture(out):
    menus = load("check_menu_rows", os.path.join(HERE, "check_menu_rows.py"))
    cse = menus.cse()
    menus.KNOWN_MACROS.add("REXGLUE_HAS_XEO3_TARGET")
    root = os.path.join(ROOT, "src/mc_engine")
    source = [menus.resolve(cse.strip_comments(read(os.path.join(root, p))), True) for p in SOURCES]
    whole = "\n".join(source)

    # Value arrays (const char* const kXVals[] = {...}; constexpr double kXVals[] = {...}) the tables name.
    values = {}
    for src in source:
        for m in re.finditer(r"(?:constexpr\s+|const\s+)*(?:const char\s*\*\s*(?:const)?|double)\s*(\w+)\s*\[\s*\]\s*=\s*\{", src):
            end = cse.close_group(src, m.end(), "{", "}")
            values[m[1]] = src[m.start():end] + ";"
    tables = {}
    for src in source:
        for m in cse.TABLE.finditer(src):
            end = cse.close_group(src, m.end(), "{", "}")
            tables[m[1]] = src[m.end():end - 1]
    alts = {}
    for src in source:
        for m in menus.DEBUG_ROWS_FN.finditer(src):
            alts[m[1]] = [m[2]]
    alts.update(ALTERNATIVES)
    # frame generation's three rows are single ItemDef objects, not a table: one pseudo table in the real order.
    gen = source[-1]
    tables["fg"] = ""
    for n in ("FrameGeneration", "Multiplier", "FpsLimit"):
        m = re.search(r"const ItemDef k" + n + r"Row\s*=\s*", gen)
        if not m:
            raise SystemExit(f"frame_generation_menu.cpp has no k{n}Row")
        tables["fg"] += gen[m.end():gen.index(";", m.end())] + ",\n"

    k = menus.MENU_STRUCT.search(source[0])
    entries = menus.split_entries(source[0][k.end():cse.close_group(source[0], k.end(), "{", "}") - 1])
    names = []
    for e in entries:
        a = [x.strip() for x in cse.split_args(e.strip()[1:e.rfind("}")])]
        extra = a[6].rsplit("::", 1)[-1].strip() if len(a) > 6 else "nullptr"
        if extra != "nullptr" and extra not in alts:
            raise SystemExit(f"the extra function {extra} has no table list in ALTERNATIVES")
        names += ([a[4]] if a[4] != "nullptr" else []) + alts.get(extra, [])

    lines = ['#include "settings_ui/action.h"', '#include "settings_ui/menu_catalog.h"', "#include <iterator>", "#include <optional>", '#include "mc_engine/menus/pause_parity_rows.h"',
             "using namespace mcla_menu;", "using larecomp::settings_ui::Menu;",
             "template<class T, size_t N> constexpr int Len(const T (&)[N]) { return int(N); }"]
    normalized = {}
    for name in dict.fromkeys(names):
        value = tables.get(name)
        if value is None:
            value = table_text(whole, name, cse)
        value = re.sub(r"Carbon\((\".*?\"),\s*\w+", r"Carbon(\1, 0", value)
        value = re.sub(r'(Color\("[^"]+",\s*"[^"]+",\s*"[^"]+",)\s*g_theme_vals,\s*kNumThemeColors', r"\1 nullptr, 0", value)
        for m in re.finditer(r"\b(k\w+)\b", value):
            if m[1] in values:
                lines.append(values.pop(m[1]))
        normalized[name] = value
    # constants the tables name that the real files define as expressions of other game code: stand-ins with the same text.
    lines.extend([
        'constexpr const char* kNativeLightShadowVals[] = {"auto","off","medium","high","ultra"};',
        'constexpr const char* kNativeLightShadowNames[] = {"AUTO","OFF","MEDIUM","HIGH","ULTRA"};',
        "constexpr struct { int count; } kLightShadowWords{5};",
        "constexpr int kNumLangs = std::size(kLangVals);",
        "constexpr auto kMenuCamSlots = [] { struct Slots { double v[64]; }; Slots out{}; for (int i = 0; i < 64; ++i) out.v[i] = i; return out; }();",
        'const char* CutsceneIdsStorage[] = {"cut_story_district_champs"}; const char* CutsceneLabelsStorage[] = {"DISTRICT CHAMPS"};',
        "auto CutsceneIds() { return CutsceneIdsStorage; } auto CutsceneLabels() { return CutsceneLabelsStorage; } int CutsceneCount() { return 1; }",
    ])
    constants = {}
    for src in source:
        for m in re.finditer(r'constexpr\s+const\s+char\s*\*\s*(\w+)\s*=\s*("[^"]+")\s*;', src):
            constants[m[1]] = m[2]
    lines.extend("constexpr auto " + n + " = " + v + ";" for n, v in constants.items())
    lines.append("std::vector<Menu> larecomp::settings_ui::AllSettingsMenus() { return {")
    for e in entries:
        a = [x.strip() for x in cse.split_args(e.strip()[1:e.rfind("}")])]
        extra = a[6].rsplit("::", 1)[-1].strip() if len(a) > 6 else "nullptr"
        ns = ([a[4]] if a[4] != "nullptr" else []) + alts.get(extra, [])
        debug = a[9] if len(a) > 9 else "false"
        lines.append("{" + ", ".join([a[0], a[3], a[1], a[2], debug]) + ", {")
        lines.extend(normalized[n] for n in ns)
        lines.append("}},")
    lines.append("}; }")
    lines.append("int main(int argc, char** argv) { return larecomp::settings_ui::DumpSettingsUi(argc > 1 ? "
                 "std::optional<std::string_view>{argv[1]} : std::nullopt); }")
    with open(os.path.join(out, "fixture.cpp"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return normalized


def build(out, args):
    cmd = [args.clang, "-fuse-ld=lld", "-std=c++23", "-Isrc", "-I" + out, "-isystem", args.toml, "-isystem", args.sdk + "/include",
           "src/settings_ui/action.cpp", "src/settings_ui/export.cpp", "src/citypack/file_replace.cpp", "src/settings/larecomp_settings.cpp",
           os.path.join(out, "fixture.cpp"), args.alloc, args.sdk + "/lib/rexbase.lib", "-o", os.path.join(out, "fixture.exe")]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        print((r.stdout + r.stderr)[-6000:])
        raise SystemExit("the dump program does not build")


def run_dump(out, path, args):
    env = dict(os.environ)
    env["PATH"] = args.sdk + "\\bin;" + env.get("PATH", "")
    r = subprocess.run([os.path.join(out, "fixture.exe"), path], cwd=ROOT, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
    result = [ln for ln in r.stdout.splitlines() if ln.startswith("@settingsui|")]
    if r.returncode or not result or "|ok|" not in result[-1]:
        print(r.stdout[-3000:] + r.stderr[-3000:])
        raise SystemExit("the dump program failed")


def python_tool(*argv):
    r = subprocess.run([sys.executable, "-I", *argv], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout + r.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--no-build", action="store_true", help="reuse fixture.exe from --out")
    ap.add_argument("--clang", default=CLANG)
    ap.add_argument("--sdk", default=SDK)
    ap.add_argument("--toml", default=TOML)
    ap.add_argument("--alloc", default=ALLOC)
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip() or ("0" * 40)
    failures = []
    if not args.no_build:
        write_data_header(out, commit)
        tables = write_fixture(out)
        build(out, args)
    first, second = os.path.join(out, "settings-ui.json"), os.path.join(out, "settings-ui.2.json")
    run_dump(out, first, args)
    run_dump(out, second, args)
    if open(first, "rb").read() != open(second, "rb").read():
        failures.append("two dumps of the same program differ")

    contract = load("validate_contract", os.path.join(ROOT, "documentation/contract/validate_contract.py"))
    ui = load("check_settings_ui", os.path.join(HERE, "check_settings_ui.py")) if os.path.exists(os.path.join(HERE, "check_settings_ui.py")) else None
    dictionary = ui.merged_dictionary(contract) if ui else contract.dictionary()
    report = contract.validate(contract.load_json(first), "settings-ui", dictionary, strict=True)
    for severity, entries in (("ERROR", report.errors), ("WARN", report.warnings)):
        for entry in entries:
            print(f"{severity} {entry}")
    print(f"validate_contract --strict: {len(report.errors)} errors, {len(report.warnings)} warnings, {len(report.infos)} notes")
    if report.errors or report.warnings:
        failures.append(f"validate_contract --strict: {len(report.errors)} errors, {len(report.warnings)} warnings")

    ui_tool = os.path.join(HERE, "check_settings_ui.py")
    if os.path.exists(ui_tool):
        code, text = python_tool(ui_tool, first, "--src", os.path.join(ROOT, "src"), "--repeat", second)
        print(text.strip()[-1500:])
        if code:
            failures.append("tools/check_settings_ui.py")

    # Cross-check against the sources: every PM_ row of the menu tables is in the dump, and no PM_ row of the dump is missing from them.
    doc = json.load(open(first, encoding="utf-8"))
    rows = [r for t in doc["tabs"] for s in t["sections"] for r in s["rows"]]
    dump_ids = {r["id"] for r in rows if r["id"].startswith("PM_")}
    menus = load("check_menu_rows", os.path.join(HERE, "check_menu_rows.py"))
    cse = menus.cse()
    text = "\n".join(cse.strip_comments(read(os.path.join(ROOT, "src/mc_engine", p))) for p in SOURCES)
    wrappers = re.compile(r'\b(?:Bool|Dbl|Str|Action|Color|Carbon|Submenu)\s*\(\s*"(PM_\w+)"')
    source_ids = set(wrappers.findall(text))
    if source_ids - dump_ids:
        failures.append("rows in the source tables, not in the dump: " + ", ".join(sorted(source_ids - dump_ids)))
    if dump_ids - source_ids:
        failures.append("rows in the dump, not in the source tables: " + ", ".join(sorted(dump_ids - source_ids)))
    named = {r["setting"] for r in rows if r.get("setting")}
    for r in rows:
        named.update(r.get("reads", []))
    missing = sorted(n for n in named if n not in doc["settings"])
    if missing:
        failures.append("settings a row names that the catalog lacks: " + ", ".join(missing))
    print(f"{len(rows)} rows ({len(dump_ids)} game, {len(rows) - len(dump_ids)} launcher), {len(doc['settings'])} settings, "
          f"{len(doc.get('facts_used', []))} facts, settings_hash {doc['generated']['settings_hash'][:12]}")
    for f in failures:
        print("FAIL", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
