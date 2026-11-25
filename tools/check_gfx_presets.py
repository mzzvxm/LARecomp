r"""check_gfx_presets.py: is the graphics preset table complete, and does it agree with the menu and the settings?  (dev only, reads files, builds nothing)

  python -I tools\check_gfx_presets.py [--src <src dir>] [--selftest] [-v]

The table is src/gpu/gfx_preset.h (kRows: one cvar, its page and one cell for each of VANILLA, ENHANCED, ENHANCED ULTRA, ENHANCED RT and ENHANCED RT ULTRA). gfx_preset_test.cpp checks what
needs no source reading (the cells against the old `auto` tables and the audit's values, the rules); this reads the sources the table has to agree with:

  1. every row of the four effect pages (kImageQualityItems, kLightItems, kReflectionItems, kWorldDetailItems in pause_menu.cpp) is a covered row of the table or on its exempt list, with
     the page the table says; every covered row is on a page; RT rows name RT and raster presets never enable them;
  2. no preset names a DISPLAY or UPSCALING setting: not one of kNeverCovered, not one a row of kVideoItems, upscaling_menu.cpp or frame_generation_menu.cpp names;
  3. every cell is a value its setting takes (the setting's own Allowed(...) list, its Range{...}, true or false for a bool), and never `auto`;
  4. every row of the table is a setting that exists (mcla_gpu_preset and mcla_gpu_preset_from too: the first live and not debug-only, the second debug-only, the words of both are the table's);
  5. the pause row (kGfxHubItems) is there, native only, with the words of kIds and kLabels in the order of the enum; ui_rules.toml's row lists the same ids and labels, and its
     recommended_preset def is kClassToPreset;
  6. the three generated settings files (settings_gpu.h, manifest.json, settings_meta.inc) all have both settings.

Exit 0 when everything agrees, 1 when something differs, 2 for a usage error. --selftest breaks the table on purpose, one way at a time, and fails when a break is not reported.
"""
import argparse
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SRC = os.path.join(os.path.dirname(HERE), "src")

PRESETS = ("vanilla", "enhanced", "enhanced_ultra", "enhanced_rt", "enhanced_rt_ultra")
LABELS = ("VANILLA", "ENHANCED", "ENHANCED ULTRA", "ENHANCED RT", "ENHANCED RT ULTRA", "CUSTOM")
PAGE_TABLES = {
    "ImageQuality": "kImageQualityItems",
    "Lighting": "kLightItems",
    "Reflections": "kReflectionItems",
    "WorldDetail": "kWorldDetailItems",
}
RT_ONLY = {
    "mcla_gpu_rt_shadows", "mcla_gpu_ao_rt", "mcla_gpu_rt_gi", "mcla_gpu_rt_lights",
    "mcla_gpu_ssr_rt", "mcla_gpu_car_selfrefl_rt", "mcla_gpu_rt_reflections", "mcla_gpu_car_mirror_rt",
}
ROW = re.compile(r'\b(?:Bool|Str|Dbl|Color)\s*\(\s*"(PM_\w+)"\s*,\s*"([\w.]+)"')
ANY_STRING = re.compile(r'"([\w.]+)"')


def load_exposure_tool():
    spec = importlib.util.spec_from_file_location("check_settings_exposure", os.path.join(HERE, "check_settings_exposure.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CSE = load_exposure_tool()


def read_stripped(path):
    return CSE.strip_comments(CSE.read(path))


# ---- the header ---------------------------------------------------------------------------------------------------------------------------------------------

def array_text(text, name):
    m = re.search(r"\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{", text)
    if not m:
        raise SystemExit(f"gfx_preset.h has no table {name}")
    return text[m.end():CSE.close_group(text, m.end(), "{", "}") - 1]


def parse_header(text):
    """The table as data: rows [(cvar, page, [5 cells])], the exempt and below-max notes, the never-covered names, the class rules, kIds and kLabels."""
    rows = []
    for m in re.finditer(r'\{\s*"(\w+)"\s*,\s*Page::(\w+)\s*,\s*\{\s*((?:"[^"]*"\s*,\s*){4}"[^"]*")\s*\}\s*\}', array_text(text, "kRows")):
        rows.append((m.group(1), m.group(2)[1:] if m.group(2).startswith("k") else m.group(2), re.findall(r'"([^"]*)"', m.group(3))))
    notes = lambda name: [(a, b) for a, b in re.findall(r'\{\s*"(\w+)"\s*,\s*"([^"]*)"\s*\}', array_text(text, name))]
    never = re.findall(r'"(\w+)"', array_text(text, "kNeverCovered"))
    rules = re.findall(r'\{\s*"([^"]*)"\s*,\s*Preset::(\w+)\s*\}', array_text(text, "kClassToPreset"))
    ids = re.findall(r'"(\w+)"', array_text(text, "kIds"))
    labels = re.findall(r'"([^"]*)"', array_text(text, "kLabels"))
    return {"rows": rows, "exempt": notes("kExempt"), "below": notes("kBelowMax"), "never": never, "rules": rules, "ids": ids, "labels": labels}


# ---- the menu -----------------------------------------------------------------------------------------------------------------------------------------------

def table_rows(text, name, rules=None):
    """(the table's text, the settings its rows name): each row's own setting, and the settings its rules write (a compound row such as CAR SELF-REFLECTIONS writes two)."""
    m = re.search(r"\bItemDef\s+" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{", text)
    if not m:
        raise SystemExit(f"pause_menu.cpp has no table {name}")
    body = text[m.end():CSE.close_group(text, m.end(), "{", "}") - 1]
    cvars = []
    for row_id, cvar in ROW.findall(body):
        cvars.append(cvar)
        for value in (rules or {}).get("rows", {}).get(row_id, {}).get("values", []):
            cvars.extend(w["setting"] for w in value.get("writes", []) if w["setting"] not in cvars)
    return body, cvars


def settings_of(src):
    """name -> {'type', 'allowed', 'range', 'debug'} for every REX_SETTING under <src>/settings/generated."""
    out = {}
    folder = os.path.join(src, "settings", "generated")
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".h"):
            continue
        text = read_stripped(os.path.join(folder, name))
        for m in CSE.SETTING.finditer(text):
            end = CSE.close_group(text, m.end(), "(", ")")
            args = CSE.split_args(text[m.end():end - 1])
            if len(args) < 3 or not re.fullmatch(r"\w+", args[1].strip()):
                continue
            traits = ",".join(args[4:])
            allowed = None
            am = re.search(r"rex::Allowed\s*\(([^)]*)\)", traits)
            if am:
                allowed = re.findall(r'"([^"]*)"', am.group(1))
            rng = None
            rm = re.search(r"rex::Range\s*\{\s*([-\d.e+]+)\s*,\s*([-\d.e+]+)\s*\}", traits)
            if rm:
                rng = (float(rm.group(1)), float(rm.group(2)))
            out[args[1].strip()] = {"type": args[0].strip(), "default": args[2].strip(), "allowed": allowed,
                                    "range": rng, "debug": "kDebugOnly" in CSE.blank_strings(traits)}
    return out


def display_settings(src):
    """Every setting a row of DISPLAY (kVideoItems), the upscaling rows or the frame generation rows names."""
    names = set()
    menu = read_stripped(os.path.join(src, "mc_engine", "menus", "pause_menu.cpp"))
    names.update(table_rows(menu, "kVideoItems")[1])
    for f in ("upscaling_menu.cpp", "frame_generation_menu.cpp", "display_menu.cpp"):
        path = os.path.join(src, "mc_engine", "menus", f)
        if os.path.exists(path):
            names.update(n for n in ANY_STRING.findall(read_stripped(path)) if n.startswith(("mcla_gpu_", "fullscreen", "resolution", "fps_limit", "aspect_ratio", "display_gamma")))
    return names


# ---- the checks ---------------------------------------------------------------------------------------------------------------------------------------------

def check(src, data=None, menu_text=None, settings=None, rules_text=None):
    problems = []
    add = problems.append
    header = data if data is not None else parse_header(read_stripped(os.path.join(src, "gpu", "gfx_preset.h")))
    menu = menu_text if menu_text is not None else read_stripped(os.path.join(src, "mc_engine", "menus", "pause_menu.cpp"))
    settings = settings if settings is not None else settings_of(src)
    rows = header["rows"]
    by_cvar = {}
    for cvar, page, cells in rows:
        if cvar in by_cvar:
            add(f"{cvar}: twice in the table")
        by_cvar[cvar] = (page, cells)
        if len(cells) != 5:
            add(f"{cvar}: {len(cells)} cells, want 5")
    exempt = {c for c, _ in header["exempt"]}

    # 1. the four pages
    import tomllib
    rules = rules_text if rules_text is not None else CSE.read(os.path.join(src, "settings", "ui_rules.toml"))
    doc = tomllib.loads(rules)
    on_page = {}
    bounded_settings = set(RT_ONLY) | {"mcla_gpu_ao"}
    for page, table in PAGE_TABLES.items():
        body, cvars = table_rows(menu, table, doc)
        for row_id, cvar in ROW.findall(body):
            if cvar in bounded_settings:
                match = re.search(r'"' + row_id + r'"\s*,\s*"\w+"\s*,\s*"([^"]+)"', body)
                if not match or not re.search(r"\bRT\b", match[1]):
                    add(f"{row_id}: its label must identify ray tracing")
                for state in doc["rows"].get(row_id, {}).get("states", []):
                    if "label" in state and not re.search(r"\bRT\b", state["label"]):
                        add(f"{row_id}: its status label must identify ray tracing")
        for c in cvars:
            on_page.setdefault(c, set()).add(page)
    for c, pages in sorted(on_page.items()):
        if c in by_cvar:
            want = by_cvar[c][0]
            if pages != {want}:
                add(f"{c}: the table says {want}, the menu has it on {sorted(pages)}")
        elif c not in exempt:
            add(f"{c}: on {sorted(pages)} but neither covered by a preset nor exempt (kExempt)")
    for c in sorted(exempt):
        if c in by_cvar:
            add(f"{c}: exempt and covered at once")
        if c not in on_page:
            add(f"{c}: exempt, but no row of the four pages has it (a stale exempt entry)")
    for cvar, (page, _) in by_cvar.items():
        if cvar not in on_page:
            add(f"{cvar}: covered, but on none of the four pages")

    # 2. DISPLAY and UPSCALING
    shown = display_settings(src) if data is None else set(header.get("display", []))
    for cvar in by_cvar:
        if cvar in header["never"]:
            add(f"{cvar}: a DISPLAY or UPSCALING setting (kNeverCovered) in a preset")
        if cvar in shown:
            add(f"{cvar}: a row of DISPLAY or UPSCALING names it, and a preset covers it")

    # 3. the cells
    for cvar, (page, cells) in by_cvar.items():
        info = settings.get(cvar)
        if info is None:
            add(f"{cvar}: not a setting")
            continue
        if info["debug"]:
            add(f"{cvar}: preset applies a debug-only setting")
        for preset, cell in zip(PRESETS, cells):
            if cell == "auto":
                add(f"{cvar} in {preset}: a preset never holds `auto`")
            if preset in PRESETS[:3] and ((cvar in RT_ONLY and cell != "off") or (cvar == "mcla_gpu_ao" and cell == "rt")):
                add(f"{cvar} in {preset}: a raster preset enables ray tracing")
            if "low" in cell.split("_"):
                add(f"{cvar} in {preset}: '{cell}': no setting has a level called low")
            if info["type"] == "bool":
                if cell not in ("true", "false"):
                    add(f"{cvar} in {preset}: '{cell}' is not true or false")
            elif info["type"] in ("f64", "double"):
                try:
                    v = float(cell)
                except ValueError:
                    add(f"{cvar} in {preset}: '{cell}' is not a number")
                    continue
                if info["range"] and not (info["range"][0] <= v <= info["range"][1]):
                    add(f"{cvar} in {preset}: {cell} is outside the setting's range {info['range']}")
            elif info["allowed"] is not None and cell not in info["allowed"]:
                add(f"{cvar} in {preset}: '{cell}' is not one of {info['allowed']}")

    ssr = by_cvar.get("mcla_gpu_ssr")
    if ssr and "off" in ssr[1][1:]:
        add("mcla_gpu_ssr: every Enhanced preset must select a concrete enabled level")
    for cvar in ("mcla_ui_sharp", "mcla_ui_minimap_scale"):
        info = settings.get(cvar, {})
        if not info.get("debug") or info.get("default") != "true":
            add(f"{cvar}: the sharpening setting must default on and be debug-only")

    # 4. the two settings
    for name, debug in (("mcla_gpu_preset", False), ("mcla_gpu_preset_from", True)):
        info = settings.get(name)
        if info is None:
            add(f"{name}: not a setting")
            continue
        if info["debug"] != debug:
            add(f"{name}: debug-only should be {debug}")
    if "mcla_gpu_preset" in by_cvar or "mcla_gpu_preset_from" in by_cvar:
        add("a preset covers the preset's own setting")
    want_allowed = list(PRESETS) + ["custom"]
    got = (settings.get("mcla_gpu_preset") or {}).get("allowed")
    if got != want_allowed:
        add(f"mcla_gpu_preset allows {got}, want {want_allowed}")
    got = (settings.get("mcla_gpu_preset_from") or {}).get("allowed")
    if got != ["none"] + list(PRESETS):
        add(f"mcla_gpu_preset_from allows {got}, want none and the five named presets")

    # 5. the row
    if header["ids"] != want_allowed:
        add(f"kIds is {header['ids']}")
    if header["labels"] != list(LABELS):
        add(f"kLabels is {header['labels']}")
    vals = re.search(r"kGfxPresetVals\s*\[\s*\]\s*=\s*\{([^}]*)\}", menu)
    names = re.search(r"kGfxPresetNames\s*\[\s*\]\s*=\s*\{([^}]*)\}", menu)
    if not vals or re.findall(r'"([^"]*)"', vals.group(1)) != header["ids"]:
        add("pause_menu.cpp kGfxPresetVals is not kIds")
    if not names or re.findall(r'"([^"]*)"', names.group(1)) != header["labels"]:
        add("pause_menu.cpp kGfxPresetNames is not kLabels")
    hub, _ = table_rows(menu, "kGfxHubItems")
    if not re.search(r'NativeOnly\(\s*Str\(\s*"PM_RxGpuPreset"\s*,\s*"mcla_gpu_preset"\s*,\s*"PRESET: "\s*,\s*kGfxPresetVals\s*,\s*kGfxPresetNames\s*,\s*6\s*\)\s*\)', hub):
        add("kGfxHubItems has no NativeOnly(Str(\"PM_RxGpuPreset\", \"mcla_gpu_preset\", ...)) row")
    if hub.find("PM_RxSubVideo") > hub.find("PM_RxGpuPreset") or hub.find("PM_RxGpuPreset") > hub.find("PM_RxSubImageQuality"):
        add("the PRESET row is not between DISPLAY and IMAGE QUALITY on the hub")
    row = doc.get("rows", {}).get("PM_RxGpuPreset")
    if row is None:
        add("ui_rules.toml has no [rows.PM_RxGpuPreset]")
    else:
        if [v["id"] for v in row.get("values", [])] != want_allowed:
            add("ui_rules.toml PM_RxGpuPreset lists other ids than the setting's")
        if [v["label"] for v in row.get("values", [])] != list(LABELS):
            add("ui_rules.toml PM_RxGpuPreset lists other labels than kLabels")
        for v in row.get("values", []):
            needs_rt = v["id"] in ("enhanced_rt", "enhanced_rt_ultra")
            if needs_rt != ("when" in v) or (needs_rt and "gpu.rt_tier != 'none'" not in v["when"]):
                add(f"ui_rules.toml PM_RxGpuPreset: {v['id']} should{' ' if needs_rt else ' not '}be listed only where the card traces rays")
        if not any("NO RT GPU" in s.get("suffix", "") for s in row.get("states", [])):
            add("ui_rules.toml PM_RxGpuPreset has no (NO RT GPU) state")
    cases = doc.get("defs", {}).get("recommended_preset", {}).get("cases", [])
    want_rules = [(r[0], r[1]) for r in header["rules"]]
    got_rules = [(c.get("when", "true"), c["value"]) for c in cases]
    word = {"kVanilla": "vanilla", "kEnhanced": "enhanced", "kEnhancedUltra": "enhanced_ultra", "kEnhancedRt": "enhanced_rt", "kEnhancedRtUltra": "enhanced_rt_ultra"}
    want_rules = [(w, word.get(p, p)) for w, p in want_rules]
    # the header's last rule is `true`; the def's last case has no `when`
    if got_rules != want_rules:
        add(f"ui_rules.toml defs.recommended_preset is {got_rules}, kClassToPreset is {want_rules}")

    # 6. the generated files
    gen = os.path.join(src, "settings", "generated")
    header_h = CSE.read(os.path.join(gen, "settings_gpu.h"))
    manifest = json.loads(CSE.read(os.path.join(gen, "manifest.json")))
    meta = CSE.read(os.path.join(gen, "settings_meta.inc"))
    for name in ("mcla_gpu_preset", "mcla_gpu_preset_from"):
        if f"inline auto& {name} = g_gpu.{name};" not in header_h:
            add(f"settings_gpu.h has no alias for {name}")
        if name not in manifest:
            add(f"manifest.json has no entry for {name}")
        if f'{{"{name}", "MCLA/GPU"}},' not in meta:
            add(f"settings_meta.inc has no line for {name}")
    return problems


def selftest(src):
    base = parse_header(read_stripped(os.path.join(src, "gpu", "gfx_preset.h")))
    menu = read_stripped(os.path.join(src, "mc_engine", "menus", "pause_menu.cpp"))
    settings = settings_of(src)
    rules = CSE.read(os.path.join(src, "settings", "ui_rules.toml"))
    import copy

    def run(mutate, label):
        data = copy.deepcopy(base)
        data["display"] = sorted(display_settings(src))
        extra = mutate(data)
        extra = extra if isinstance(extra, dict) else {}
        problems = check(src, data=data, menu_text=extra.get("menu", menu),
                         settings=extra.get("settings", settings), rules_text=extra.get("rules", rules))
        if not problems:
            print(f"SELFTEST FAILED: '{label}' was not reported")
            return 1
        return 0

    failures = 0
    clean = copy.deepcopy(base)
    clean["display"] = sorted(display_settings(src))
    if check(src, data=clean, menu_text=menu, settings=settings, rules_text=rules):
        print("SELFTEST FAILED: the unbroken table is not clean:")
        for p in check(src, data=clean, menu_text=menu, settings=settings, rules_text=rules):
            print("  " + p)
        failures += 1
    failures += run(lambda d: d["rows"].pop(0), "a covered row removed from the table")
    failures += run(lambda d: d["rows"].append(("fps_limit", "WorldDetail", ["30"] * 5)), "a DISPLAY setting added to the table")
    failures += run(lambda d: d["rows"].append(("mcla_gpu_aa", "ImageQuality", ["off"] * 5)), "the relocated Post AA still covered")
    failures += run(lambda d: d["rows"].append(("mcla_gpu_upscaler", "Lighting", ["off"] * 5)), "an UPSCALING setting added to the table")
    failures += run(lambda d: d["rows"].__setitem__(0, (d["rows"][0][0], d["rows"][0][1], ["auto"] + d["rows"][0][2][1:])), "auto as a cell")
    ao = next(i for i, r in enumerate(base["rows"]) if r[0] == "mcla_gpu_ao")
    failures += run(lambda d: d["rows"].__setitem__(ao, (d["rows"][ao][0], d["rows"][ao][1], ["nonsense"] + d["rows"][ao][2][1:])), "a word the setting does not take")
    failures += run(lambda d: d["rows"].__setitem__(0, (d["rows"][0][0], "Lighting", d["rows"][0][2])), "a row on the wrong page")
    failures += run(lambda d: d["exempt"].append(("mcla_gpu_ao", "x")), "a covered row that is also exempt")
    failures += run(lambda d: d["exempt"].pop(0), "an exempt row dropped (a row of the pages with no cell)")
    failures += run(lambda d: d["rows"].append(("mcla_gpu_new_effect", "Lighting", ["off"] * 5)), "a row for a setting that does not exist")
    failures += run(lambda d: d["labels"].reverse(), "the labels in another order")
    failures += run(lambda d: d["rules"].pop(0), "a class rule dropped")
    failures += run(lambda d: {"menu": menu.replace('"PM_RxGpuPreset"', '"PM_RxGpuPresetX"')}, "the pause row renamed")
    failures += run(lambda d: {"rules": rules.replace("gpu.rt_tier != 'none' ||", "true ||")}, "the RT presets offered everywhere")
    failures += run(lambda d: {"menu": menu.replace('"SURFACE REFLECTIONS RT: "', '"SURFACE REFLECTIONS: "')}, "RT label missing")
    for cvar in ("mcla_ui_sharp", "mcla_ui_minimap_scale"):
        failures += run(lambda d: {"settings": settings | {cvar: settings[cvar] | {"debug": False}}},
                        f"{cvar} exposed as a player setting")
    for cvar, cell in (("mcla_gpu_ssr_rt", "high"), ("mcla_gpu_ao", "rt"), ("mcla_gpu_ssr", "off")):
        def replace_cell(d):
            next(row for row in d["rows"] if row[0] == cvar)[2][1] = cell
        failures += run(replace_cell, f"{cvar} incorrectly set to {cell} in Enhanced")
    return failures


def main():
    ap = argparse.ArgumentParser(description="Check the graphics preset table against the menu and the settings.")
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    src = os.path.abspath(args.src)
    if not os.path.isdir(os.path.join(src, "gpu")):
        print(f"check_gfx_presets: {src} is not the src folder", file=sys.stderr)
        return 2
    if args.selftest:
        failures = selftest(src)
        print("check_gfx_presets --selftest: " + ("every break was reported" if failures == 0 else f"{failures} break(s) NOT reported"))
        return 1 if failures else 0
    problems = check(src)
    header = parse_header(read_stripped(os.path.join(src, "gpu", "gfx_preset.h")))
    print(f"check_gfx_presets: {src}")
    print(f"  {len(header['rows'])} rows in the table covered on four effect pages, {len(header['exempt'])} exempt, {len(header['below'])} below their top level in ENHANCED ULTRA")
    if args.verbose:
        for cvar, page, cells in header["rows"]:
            print(f"  {page:12} {cvar:34} " + " ".join(f"{c:>8}" for c in cells))
    if problems:
        print(f"{len(problems)} problem(s):")
        for p in problems:
            print("  " + p)
        return 1
    print("the table has a cell for every covered row in every preset, covers exactly the rows of the four pages, names no DISPLAY or UPSCALING setting, and agrees with the settings and the menu")
    return 0


if __name__ == "__main__":
    sys.exit(main())
