"""Check that pause-menu parity and the preset lint reject valid but changed rules."""

import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory
import tomllib


MUTATIONS = {
    "shadow master inverted": ('shadows_on = "!s.perf_no_shadows"', 'shadows_on = "s.perf_no_shadows"'),
    "AO Ultra omitted": (
        '''ao_on = "s.mcla_gpu_ao in ['auto','on','high','ultra','rt']"''',
        '''ao_on = "s.mcla_gpu_ao in ['auto','on','high','rt']"'''),
    "RT city not-ready hint": ('suffix = " (NOT READY)", label = "RT CITY"',
                               'suffix = " (BUILDING DATA)", label = "RT CITY"'),
    "RT city disabled during build": ('enabled = true, suffix = " (BUILDING DATA)"',
                                      'enabled = false, suffix = " (BUILDING DATA)"'),
    "RT shadows legacy gate": ('decode = "resolved(s.mcla_gpu_rt_shadows)"',
                               'decode = "s.mcla_gpu_rt_shadows"'),
    "PCSS Auto skips resolution": ('decode = "resolved(s.mcla_gpu_shadow_filter)"',
                                   'decode = "s.mcla_gpu_shadow_filter"'),
    "world AO skips legacy decode": (
        '''decode = { cases = [{ when = "s.mcla_gpu_ao in ['auto','on','high','ultra']", value = "on" }, { value = { expr = "s.mcla_gpu_ao" } }] }''',
        'decode = "s.mcla_gpu_ao"'),
    "display mode ignores exclusive support": (
        "s.fullscreen_exclusive && os.exclusive_fullscreen", "s.fullscreen_exclusive"),
    "no exclusive display mode": ('label = "FULLSCREEN", when = "os.exclusive_fullscreen"',
                                  'label = "FULLSCREEN", when = "false"'),
    "resolution generator changed": ('generator = "display.sizes"', 'generator = "render_scale.shares"'),
    "resolution restart prompt": (
        """when = "d.display_mode == 'windowed' && !os.live_window_resize", enabled = true, suffix = " (RESTART)", needs_restart = true""",
        """when = "d.display_mode == 'windowed' && !os.live_window_resize", enabled = true, suffix = " (RESTART)", needs_restart = false"""),
    "HDR paper white decode": ('expr = "features.hdr.auto_paper_white_nits"', 'expr = "features.hdr.auto_peak_nits"'),
    "MSAA uses native values on Xenia": ('[[rows.PM_RxMsaa.variants]]\nwhen = "renderer == \'xenia\'"',
                                        '[[rows.PM_RxMsaa.variants]]\nwhen = "false"'),
    "car RT uses the wrong API": ("car_rt_runs = \"api.car_rt", "car_rt_runs = \"api.city_rt_shadows"),
    "car RT accepts the legacy hidden setting": ("s.mcla_gpu_car_selfrefl_rt == 'on'", "s.mcla_gpu_car_selfrefl_rt != 'off'"),
    "Portuguese offered everywhere": ('when = "install.portuguese_text"', 'when = "true"'),
    "HUD width greying": ('!(live.ultrawide_active ?? true)', 'live.ultrawide_active ?? true'),
    "discard the kept upscaler": ("live.kept_upscaler ?? 'off'", "'off'"),
    "wrong shared FSR modes": ('setting = "mcla_gpu_fsr31_mode"\nvalues_ref = "fsr_modes"',
                               'setting = "mcla_gpu_fsr31_mode"\nvalues_ref = "not_a_mode_set"'),
    "DLSS refused label": ('label = "DLAA (N/A)"', 'label = "DLAA (100%)"'),
    "FSR 1 default write": ("live.fsr1_preset_custom ?? (s.mcla_gpu_fsr_preset == 'custom')", "false"),
    "frame-generation multiplier math": (
        '[rows.PM_RxGpuFrameGenFps.value."30"]\nlabel_rules = [\n'
        '  { when = "true", label = "{value} ({value*d.shown_per_rendered} SHOWN)" },',
        '[rows.PM_RxGpuFrameGenFps.value."30"]\nlabel_rules = [\n'
        '  { when = "true", label = "{value} ({value} SHOWN)" },'),
    "frame-generation multiplier bounds": ("features.frame_generation.dlss_multiplier_max >= 4",
                                         "features.frame_generation.dlss_multiplier_max >= 3"),
    "latency reason while frame generation runs": ('suffix = " (FRAME GEN)", label = "REFLEX"',
                                                  'suffix = " (PAUSED)", label = "REFLEX"'),
    "Radeon denoiser label": ('label = "AMD FSR RAY REGENERATION"', 'label = "AMD RAY REGENERATION"'),
    "NVIDIA fallback allows unknown GPU generation": (
        "gpu.vendor == 'nvidia' && (gpu.rtx ?? false)", "gpu.vendor == 'nvidia' && (gpu.rtx ?? true)"),
    "AMD fallback allows unknown GPU generation": (
        "gpu.vendor == 'amd' && (gpu.rx9000 ?? false)", "gpu.vendor == 'amd' && (gpu.rx9000 ?? true)"),
}


PRESET_MUTATIONS = {
    "RT preset recommendation accepts weak RT": (
        "rules_text",
        '''when = "gpu.class == 'high' && gpu.rt_tier == 'strong'", value = "enhanced_rt"''',
        '''when = "gpu.class == 'high' && gpu.rt_tier in ['weak','strong']", value = "enhanced_rt"'''),
    "low class no longer recommends Vanilla": (
        "rules_text",
        '''when = "gpu.class == 'low'", value = "vanilla"''',
        '''when = "gpu.class == 'low'", value = "enhanced"'''),
    "class_to_preset fallback masks hardware rules": (
        "header_text",
        '''    {"gpu.class == 'high' && gpu.rt_tier == 'strong'", Preset::kEnhancedRt},\n'''
        '''    {"gpu.class == 'low'", Preset::kVanilla},\n'''
        '    {"true", Preset::kEnhanced},',
        '    {"true", Preset::kEnhanced},\n'
        '''    {"gpu.class == 'high' && gpu.rt_tier == 'strong'", Preset::kEnhancedRt},\n'''
        '''    {"gpu.class == 'low'", Preset::kVanilla},'''),
    "legacy keys omit post AA": (
        "header_text",
        'kLegacyCoveredKeys[] = {"mcla_gpu_msaa", "mcla_gpu_aa", "mcla_gpu_car_mirror_rt"}',
        'kLegacyCoveredKeys[] = {"mcla_gpu_msaa", "mcla_gpu_car_mirror_rt"}'),
    "legacy_file_keys omitted from export": (
        "export_text", '    presets.insert("legacy_file_keys", std::move(legacy_keys));\n', ""),
    "legacy_file_keys exports preset IDs instead of settings": (
        "export_text",
        "for (const auto key : gp::kLegacyCoveredKeys) legacy_keys.push_back",
        "for (const auto key : gp::kIds) legacy_keys.push_back"),
    "legacy config starts as Enhanced instead of Custom": (
        "export_text",
        'presets.insert("legacy_file_preset", std::string(gp::Id(gp::Preset::kCustom)));',
        'presets.insert("legacy_file_preset", std::string(gp::Id(gp::Preset::kEnhanced)));'),
    "first_start_by omitted from export": (
        "export_text", '    presets.insert("first_start_by", "class_to_preset");\n', ""),
}


def mutate(source, name, old, new):
    matches = source.count(old)
    if matches != 1:
        raise SystemExit(f"Mutation anchor for {name!r}: expected one match, found {matches}")
    if old == new:
        raise SystemExit(f"Mutation for {name!r} leaves the source unchanged")
    return source.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("parity_test", type=Path)
    parser.add_argument("--rules", type=Path,
                        default=Path(__file__).with_name("ui_rules.toml"))
    args = parser.parse_args()
    src = Path(__file__).resolve().parents[1]
    source = args.rules.read_text(encoding="utf-8")
    sources = {
        "rules_text": source,
        "header_text": (src / "gpu/gfx_preset.h").read_text(encoding="utf-8"),
        "export_text": (src / "settings_ui/export.cpp").read_text(encoding="utf-8"),
    }
    parity_mutants = {name: mutate(source, name, old, new) for name, (old, new) in MUTATIONS.items()}
    preset_mutants = {
        name: (target, mutate(sources[target], name, old, new))
        for name, (target, old, new) in PRESET_MUTATIONS.items()
    }

    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("check_gfx_presets", src.parent / "tools/check_gfx_presets.py")
    presets = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(presets)
    baseline_problems = presets.check(src, **sources)
    if baseline_problems:
        print("Baseline preset lint failed:", file=sys.stderr)
        for problem in baseline_problems:
            print(problem, file=sys.stderr)
        return 1

    exe = args.parity_test.resolve()
    clean = subprocess.run([exe, args.rules.resolve()], check=False, timeout=20)
    if clean.returncode:
        return clean.returncode
    survived = []
    with TemporaryDirectory(prefix="larecomp-ui-rules-") as directory:
        file = Path(directory) / "ui_rules.toml"
        for name, text in parity_mutants.items():
            tomllib.loads(text)
            file.write_text(text, encoding="utf-8", newline="\n")
            result = subprocess.run([exe, file], capture_output=True, text=True, timeout=20)
            summary = re.search(r"pause menu parity: \d+ checks, ([1-9]\d*) failed", result.stdout)
            if result.returncode != 1 or not summary:
                survived.append(name)
                print(f"SURVIVED {name}")
            else:
                print(f"CAUGHT {name} ({summary[1]} checks)")
    for name, (target, text) in preset_mutants.items():
        problems = presets.check(src, **(sources | {target: text}))
        if problems:
            print(f"CAUGHT {name} (preset lint)")
        else:
            survived.append(name)
            print(f"SURVIVED {name}")
    total = len(MUTATIONS) + len(PRESET_MUTATIONS)
    print(f"{total - len(survived)}/{total} mutations caught")
    return int(bool(survived))


if __name__ == "__main__":
    raise SystemExit(main())
