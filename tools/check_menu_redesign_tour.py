"""Check the menu-redesign tour's route and preset changes from the game log."""

import argparse
from pathlib import Path
import re


def check(log):
    expected = (
        r"\[pause-menu\] opened 'GRAPHICS'",
        r"\[preset\] enhanced_rt held over \d+ rows",
        r"\[pause-menu\] opened 'DISPLAY'",
        r"\[pause-menu\] cvar 'mcla_gpu_aa' -> 'fxaa'",
        r"\[pause-menu\] opened 'IMAGE QUALITY'",
        r"\[preset\] mcla_gpu_anisotropy changed by the player: enhanced_rt becomes custom",
        r"\[pause-menu\] cvar 'mcla_gpu_anisotropy' -> 'game'",
        r"\[pause-menu\] opened 'LIGHTING AND SHADOWS'",
        r"\[pause-menu\] opened 'REFLECTIONS'",
        r"\[pause-menu\] opened 'WORLD DETAIL'",
    )
    remaining = iter(log.splitlines())
    for pattern in expected:
        if not any(re.search(pattern, line) for line in remaining):
            raise ValueError(f"missing expected log line in order: {pattern}")
    custom_writes = [line for line in log.splitlines() if "[preset]" in line and "becomes custom" in line]
    if len(custom_writes) != 1:
        raise ValueError(f"expected only the Texture Filtering edit to switch to Custom, found {len(custom_writes)} changes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    try:
        check(args.log.read_text(encoding="utf-8-sig", errors="replace"))
    except (OSError, ValueError) as error:
        print(f"FAIL {error}")
        return 1
    print("PASS menu-redesign tour")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
