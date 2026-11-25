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
    dictionary["facts"].update(contract.load_json(folder / "fact-dictionary.pause-menu.json"))
    return dictionary


def rows_of(doc):
    for tab in doc["tabs"]:
        for section in tab["sections"]:
            yield from section["rows"]


# What contract v1 can say (LAUNCHER-FOLLOWS-GAME sections 1.5 and 1.6). The game's own dump never uses a word outside it: a new row kind or a new generator is a change of the contract
# (a launcher that has not heard of it draws a fallback), so it is an error here until the contract lists it.
V1_KINDS = {"choice", "bool", "number", "text", "color", "action", "link", "carbon"}
V1_GENERATORS = {"display.sizes", "render_scale.shares", "gpu.adapters"}
TEMPLATE_WORDS = {"value", "id", "resolved", "resolved:format"}
TEXT_FIELDS = ("suffix", "label", "reason", "tag", "help_append")


def forms(row):
    yield "", row
    for index, variant in enumerate(row.get("variants", [])):
        yield f".variants[{index}]", {**row, **variant}


def vocabulary_errors(doc, contract):
    """The words of the dump against the words of contract v1: kinds, generators, the defaults of bool rows, the facts a template reads."""
    report = contract.Report()
    checker = contract.ExprChecker(report, contract.FactTypes(merged_dictionary(contract)), doc.get("settings", {}), doc.get("defs", {}))
    for tab in doc["tabs"]:
        for section in tab["sections"]:
            for row in section["rows"]:
                for suffix, form in forms(row):
                    where = f'row {row["id"]}{suffix}'
                    if form.get("kind") not in V1_KINDS:
                        yield f'{where}: the row kind {form.get("kind")!r} is not in contract v1 ({sorted(V1_KINDS)})'
                    generator = form.get("values_from", {}).get("generator")
                    if generator and generator not in V1_GENERATORS:
                        yield f"{where}: the generator {generator!r} is not in contract v1 ({sorted(V1_GENERATORS)}): a launcher draws the row's fallback_values"
                    if form.get("kind") == "bool" and form.get("default") not in (None, "true", "false"):
                        yield f'{where}: the default of a bool row is {form.get("default")!r}, not true or false'
                    texts = [(f"states[{i}].{key}", state[key]) for i, state in enumerate(form.get("states", [])) for key in TEXT_FIELDS if isinstance(state.get(key), str)]
                    texts += [("label", form["label"])] if isinstance(form.get("label"), str) else []
                    for value in form.get("values", []) or []:
                        texts += [(f'values[{value.get("id")}].label_rules[{i}].label', rule["label"]) for i, rule in enumerate(value.get("label_rules", []))]
                    for place, text in texts:
                        for key in re.findall(r"\{([^{}]+)\}", text):
                            if key in TEMPLATE_WORDS:
                                continue
                            expression = key[len("value*"):] if key.startswith("value*") else key
                            before = len(report.errors)
                            checker.check(expression, f"{where}.{place}", bool_needed=False, live_only=True, rule_name="template")   # a template with a null fact is not applied, so a live fact needs no fallback
                            for message in report.errors[before:]:
                                yield f"template {{{key}}} in {message}"
                            if len(report.errors) > before:
                                del report.errors[before:]


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
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("dump", nargs="?", type=Path)
    source.add_argument("--write-dictionary", type=Path, metavar="FILE",
                        help="write the combined game fact dictionary for validate_contract.py --dictionary")
    parser.add_argument("--repeat", type=Path, help="a second dump from the same executable")
    parser.add_argument("--previous", type=Path, help="an earlier release dump to compare its commit and hash")
    parser.add_argument("--src", type=Path, help="compare the exported native markers against the game source")
    args = parser.parse_args()

    contract = import_file("lfg_contract", ROOT / "documentation/contract/validate_contract.py")
    try:
        if args.write_dictionary is not None:
            dictionary = json.dumps(merged_dictionary(contract), indent=2, ensure_ascii=False)
            args.write_dictionary.write_text(dictionary + "\n", encoding="utf-8")
            return 0
        doc = contract.load_json(args.dump)
        report = contract.validate(doc, kind="settings-ui", dictionary_doc=merged_dictionary(contract), strict=True)
        for severity, entries in (("ERROR", report.errors), ("WARN", report.warnings), ("INFO", report.infos)):
            for entry in entries:
                print(f"{severity} {args.dump}: {entry}")
        # The other lints run even when the contract validator found something: one run lists everything that is wrong.
        errors = [f"contract: {entry}" for entry in report.errors + report.warnings]
        errors.extend(width_errors(doc))
        errors.extend(vocabulary_errors(doc, contract))
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
        print(f"ERROR {args.dump or args.write_dictionary}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
