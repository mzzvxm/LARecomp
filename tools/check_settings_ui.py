"""Validate a settings UI dump and check menu width, native callbacks, and reproducibility."""

import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def import_file(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


def merged_dictionary(contract):
    """The contract's fact dictionary plus the pause menu's own live facts (fact-dictionary.pause-menu.json: facts only the game has)."""
    folder = ROOT / "documentation/contract"
    dictionary = contract.load_json(folder / "fact-dictionary.json")
    extra = folder / "fact-dictionary.pause-menu.json"
    if extra.exists():
        dictionary["facts"].update(contract.load_json(extra))
    return dictionary


def rows_of(doc):
    for tab in doc["tabs"]:
        for section in tab["sections"]:
            yield from section["rows"]


def native_callbacks(src):
    exposure = import_file("settings_exposure", ROOT / "tools/check_settings_exposure.py")
    callbacks = {}
    for path in (src / "mc_engine/menus").glob("*.cpp"):
        if path.name.endswith("_test.cpp"):
            continue
        text = exposure.strip_comments(exposure.read(path))
        for match in re.finditer(r"\bWithDynamics\s*\(", text):
            end = exposure.close_paren(text, match.end())
            args = exposure.split_args(text[match.end():end - 1])
            if len(args) != 2:
                continue
            row = re.search(r'"(PM_[^"]+)"', args[0])
            callback = re.fullmatch(r"\s*&(?:\w+::)*(\w+)\s*", args[1])
            if row and callback:
                callbacks[row[1]] = callback[1]
    return callbacks


def has_game_surface(row):
    return "game" in row.get("surfaces", ["game", "launcher"])


def literal_width(label):
    return len(re.sub(r"\{[^{}]+\}", "", label))


def width_errors(doc):
    # The game measures the widest label and value independently, per renderer.
    def forms(row):
        yield row
        for variant in row.get("variants", []):
            yield {**row, **variant}

    def values(form):
        entries = form.get("values", form.get("fallback_values", []))
        if "values_ref" in form:
            entries = doc.get("value_sets", {}).get(form["values_ref"], entries)
        if form["kind"] in ("bool", "carbon"):
            yield "OFF"
        for entry in entries:
            yield entry["label"]
            for rule in entry.get("label_rules", []):
                yield rule["label"]

    for tab in doc["tabs"]:
        for section in tab["sections"]:
            if not section.get("source_menu"):
                continue
            for renderer in section.get("renderers", ["native", "xenia"]):
                labels, choices = [], []
                for row in section["rows"]:
                    if not has_game_surface(row) or renderer not in row.get("renderers", ["native", "xenia"]):
                        continue
                    # Check the static page without guessing the expansion of a live template.
                    for form in forms(row):
                        labels.append(form["label"])
                        labels.extend(state.get("label", form["label"]) + state.get("suffix", "")
                                      for state in form.get("states", []))
                        choices.extend(values(form))
                label = max(labels, key=literal_width, default="")
                choice = max(choices, key=literal_width, default="")
                width = literal_width(label) + literal_width(choice)
                if width > 41:
                    yield f'{section["id"]} ({renderer}): {label!r} + {choice!r} is {width} columns (limit 41)'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("--repeat", type=Path, help="a second dump from the same executable")
    parser.add_argument("--previous", type=Path, help="an earlier release dump to compare its commit and hash")
    parser.add_argument("--src", type=Path, help="compare the exported native markers against the game source")
    args = parser.parse_args()

    contract = import_file("lfg_contract", ROOT / "documentation/contract/validate_contract.py")
    try:
        doc = contract.load_json(args.dump)
        report = contract.validate(doc, kind="settings-ui", dictionary_doc=merged_dictionary(contract), strict=True)
        for severity, entries in (("ERROR", report.errors), ("WARN", report.warnings), ("INFO", report.infos)):
            for entry in entries:
                print(f"{severity} {args.dump}: {entry}")
        # The other lints run even when the contract validator found something: one run lists everything that is wrong.
        errors = [f"contract: {entry}" for entry in report.errors + report.warnings]
        errors.extend(width_errors(doc))
        if args.src:
            callbacks = native_callbacks(args.src)
            for row in rows_of(doc):
                if row["id"] in callbacks and row.get("native") != callbacks[row["id"]]:
                    errors.append(f'{row["id"]}: expected native marker {callbacks[row["id"]]}')

        generated = doc.get("generated", {})
        digest = generated.get("settings_hash")
        if not digest:
            errors.append("generated.settings_hash is missing")
        if args.repeat and args.dump.read_bytes() != args.repeat.read_bytes():
            errors.append("repeat dump differs byte for byte")
        if args.previous:
            previous = contract.load_json(args.previous).get("generated", {})
            if generated.get("commit") and generated["commit"] == previous.get("commit") and digest != previous.get("settings_hash"):
                errors.append("settings_hash changed without a change to generated.commit")
        for error in errors:
            print(f"ERROR {args.dump}: {error}")
        count = sum(1 for _ in rows_of(doc))
        print(f"{len(errors)} errors, {count} rows, {len(doc['settings'])} settings, settings_hash {digest}")
        return bool(errors)
    except (OSError, ValueError) as error:
        print(f"ERROR {args.dump}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
