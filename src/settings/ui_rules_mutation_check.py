"""Check that the pause-menu parity test rejects valid but changed rules."""

import argparse
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory
import tomllib


MUTATIONS = {
    "shadow master inverted": ('shadows_on = "!s.perf_no_shadows"', 'shadows_on = "s.perf_no_shadows"'),
    "AO Ultra omitted": (
        "resolved(s.mcla_gpu_ao) in ['high','ultra']", "resolved(s.mcla_gpu_ao) == 'high'"),
    "RT city not-ready hint": ('suffix = " (NOT READY)", label = "RT CITY"',
                               'suffix = " (BUILDING DATA)", label = "RT CITY"'),
    "RT city disabled during build": ('enabled = true, suffix = " (BUILDING DATA)"',
                                      'enabled = false, suffix = " (BUILDING DATA)"'),
    "RT bounce GPU class": ("d.rt_field_class != 'low'", "d.rt_field_class != 'mid'"),
    "RT shadows fallback": ('!api.city_rt_shadows", label = "AUTO (OFF)"',
                            '!api.city_rt_shadows", label = "AUTO (HIGH)"'),
    "force PCSS instead of SOFT": ('label = "PCSS (SOFT)"', 'label = "PCSS"'),
    "AUTO is plain": ('[rows.PM_RxGpuAo.value."auto"]\nauto_label = true',
                      '[rows.PM_RxGpuAo.value."auto"]\nauto_label = false'),
    "display mode ignores exclusive support": (
        "s.fullscreen_exclusive && os.exclusive_fullscreen", "s.fullscreen_exclusive"),
    "no exclusive display mode": ('label = "FULLSCREEN", when = "os.exclusive_fullscreen"',
                                  'label = "FULLSCREEN", when = "false"'),
    "resolution generator changed": ('generator = "display.sizes"', 'generator = "render_scale.shares"'),
    "resolution restart prompt": (
        """when = "d.display_mode == 'windowed' && !os.live_window_resize", enabled = true, suffix = " (RESTART)", needs_restart = true""",
        """when = "d.display_mode == 'windowed' && !os.live_window_resize", enabled = true, suffix = " (RESTART)", needs_restart = false"""),
    "HDR paper white template": ('{features.hdr.auto_paper_white_nits} NITS', '{features.hdr.auto_peak_nits} NITS'),
    "MSAA uses native values on Xenia": ('[[rows.PM_RxMsaa.variants]]\nwhen = "renderer == \'xenia\'"',
                                        '[[rows.PM_RxMsaa.variants]]\nwhen = "false"'),
    "car RT uses the wrong API": ('when = "api.car_rt", writes', 'when = "api.city_rt_shadows", writes'),
    "car RT writes reversed": (
        'writes = [{ setting = "mcla_gpu_car_selfrefl_rt", value = "auto" }, { setting = "mcla_gpu_car_selfrefl", value = "auto" }]',
        'writes = [{ setting = "mcla_gpu_car_selfrefl", value = "auto" }, { setting = "mcla_gpu_car_selfrefl_rt", value = "auto" }]'),
    "Portuguese offered everywhere": ('when = "install.portuguese_text"', 'when = "true"'),
    "HUD width greying": ('!(live.ultrawide_active ?? true)', 'live.ultrawide_active ?? true'),
    "discard the kept upscaler": ("live.kept_upscaler ?? 'off'", "'off'"),
    "wrong shared FSR modes": ('values_ref = "fsr_modes"', 'values_ref = "not_a_mode_set"'),
    "DLSS refused label": ('label = "DLAA (N/A)"', 'label = "DLAA (100%)"'),
    "FSR 1 default write": ("live.fsr1_preset_custom ?? (s.mcla_gpu_fsr_preset == 'custom')", "false"),
    "frame-generation multiplier math": ('{value*d.shown_per_rendered} SHOWN', '{value} SHOWN'),
    "frame-generation multiplier bounds": ("features.frame_generation.dlss_multiplier_max >= 4",
                                         "features.frame_generation.dlss_multiplier_max >= 3"),
    "latency reason while frame generation runs": ('suffix = " (FRAME GEN)", label = "REFLEX"',
                                                  'suffix = " (PAUSED)", label = "REFLEX"'),
    "Radeon denoiser label": ('label = "AMD FSR RAY REGENERATION"', 'label = "AMD RAY REGENERATION"'),
    "launcher denoiser listing fallback": ("gpu.rx9000 ?? true", "gpu.rx9000 ?? false"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("parity_test", type=Path)
    parser.add_argument("--rules", type=Path,
                        default=Path(__file__).with_name("ui_rules.toml"))
    args = parser.parse_args()
    exe = args.parity_test.resolve()
    clean = subprocess.run([exe, args.rules.resolve()], check=False, timeout=20)
    if clean.returncode:
        return clean.returncode
    source = args.rules.read_text(encoding="utf-8")
    survived = []
    with TemporaryDirectory(prefix="larecomp-ui-rules-") as directory:
        file = Path(directory) / "ui_rules.toml"
        for name, (old, new) in MUTATIONS.items():
            if old not in source:
                raise ValueError(f"Mutation anchor missing for {name}")
            text = source.replace(old, new, 1)
            tomllib.loads(text)
            file.write_text(text, encoding="utf-8")
            result = subprocess.run([exe, file], capture_output=True, text=True, timeout=20)
            summary = re.search(r"pause menu parity: \d+ checks, ([1-9]\d*) failed", result.stdout)
            if result.returncode != 1 or not summary:
                survived.append(name)
                print(f"SURVIVED {name}")
            else:
                print(f"CAUGHT {name} ({summary[1]} checks)")
    print(f"{len(MUTATIONS) - len(survived)}/{len(MUTATIONS)} mutations caught")
    return int(bool(survived))


if __name__ == "__main__":
    raise SystemExit(main())
