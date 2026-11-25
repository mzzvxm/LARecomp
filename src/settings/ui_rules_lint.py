"""Lint the pause menu's rule overlays and ensure the row callbacks stay retired."""

import importlib.util
import json
from pathlib import Path
import re
import sys
import tomllib


def main():
    sys.dont_write_bytecode = True
    root = Path(__file__).resolve().parents[2]
    contract = root / "documentation" / "contract"
    spec = importlib.util.spec_from_file_location("validate_contract", contract / "validate_contract.py")
    validator = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = validator
    spec.loader.exec_module(validator)

    document = tomllib.loads((root / "src/settings/ui_rules.toml").read_text(encoding="utf-8"))
    dictionary = json.loads((contract / "fact-dictionary.json").read_text(encoding="utf-8"))
    dictionary["facts"].update(json.loads((contract / "fact-dictionary.pause-menu.json").read_text(encoding="utf-8")))
    rows = document["rows"]
    defs = document["defs"]
    report = validator.Report()
    menus = root / "src/mc_engine/menus"
    row_ids = set()
    for path in sorted(menus.glob("*")):
        if path.suffix not in (".h", ".cpp") or path.stem.endswith("_test"):
            continue
        code = path.read_text(encoding="utf-8")
        row_ids.update(re.findall(r'\b(?:Bool|Str|Dbl|Color)\s*\(\s*"(PM_\w+)"', code))
        if re.search(r"\b(?:struct RowDynamics|WithDynamics\s*\(|RowDynamics\s*\*)", code):
            report.error(path.name, "a native row callback is still present (lint 7)")

    # The dump lint checks setting names and types against the live registry.
    # This lint also runs without an executable, so it uses the flat names
    # authored in the settings source and the SDK alias table.
    generated = root / "src/settings/generated"
    names = set()
    for path in generated.glob("settings_*.h"):
        names.update(re.findall(r"\bREX_SETTING\(\s*[^,]+,\s*(\w+)\s*,", path.read_text(encoding="utf-8")))
    sdk_names = (root / "src/settings/sdk_names.inc").read_text(encoding="utf-8")
    names.update(re.findall(r'"(\w+)"\s*,', sdk_names))
    settings = dict.fromkeys(names, {})
    checker = validator.ExprChecker(report, validator.FactTypes(dictionary), settings=settings, defs=defs)

    def walk(rule, where):
        if isinstance(rule, list):
            for index, item in enumerate(rule):
                walk(item, f"{where}[{index}]")
        if not isinstance(rule, dict):
            return
        if "native" in rule:
            report.error(where, "native row override remains (lint 7)")
        if "values_from" in rule and not rule.get("fallback_values"):
            report.error(where, "a generated list needs fallback values")
        if "RESTART" in rule.get("suffix", "") and rule.get("needs_restart") is not True:
            report.error(where, "RESTART requires needs_restart")
        for key, value in rule.items():
            at = f"{where}.{key}"
            if key in ("hidden_unless", "when", "decode", "expr") and isinstance(value, str):
                checker.check(value, at, bool_needed=key in ("when", "hidden_unless"), live_only=rule.get("live_only", False))
            else:
                walk(value, at)

    for name, rule in defs.items():
        if isinstance(rule, str):
            checker.check(rule, f"defs.{name}", bool_needed=False)
        else:
            walk(rule, f"defs.{name}")
    for key, row in rows.items():
        if key not in row_ids:
            report.error(key, "overlay has no pause menu row")
        walk(row, f"rows.{key}")
    for error in report.errors:
        print(error)
    print(f"{len(rows)} row overlays, {len(defs)} definitions, {len(checker.fact_paths)} facts")
    return int(bool(report.errors))


if __name__ == "__main__":
    raise SystemExit(main())
