r"""check_settings_ui_lints.py: do the game's lints fail a row the contract cannot express?  (dev only; reads files, builds nothing)

  python -I tools\check_settings_ui_lints.py [<settings-ui.json>]       (default: the dump tools\check_settings_dump.py made: C:\mcv\larecomp\perf\lfg-xcheck\dump\settings-ui.json)

A lint that passes everything is no lint. This takes the REAL dump of the game's settings UI and breaks it on purpose, one change at a time, the way a careless edit of ui_text.toml, ui_rules.toml or a menu
table would: a row of a kind nobody can draw, a rule that calls a function that does not exist, a fact that is not in the dictionary, a help line gone, a list the launcher cannot build and
has no fallback for, a value the setting does not take, and so on. Each broken copy goes through the same lints the game runs on its own dump (documentation/contract/validate_contract.py --strict
with the pause menu's own live facts, and the width lint of tools/check_settings_ui.py). The run fails (exit 1) when a break is NOT reported: that is a row the contract cannot express that the
lints let through. The dump's own findings (what the lints already say about the untouched file) are the baseline: a break counts as caught only by a message the baseline does not have.

Mutations that only change a row in place keep every other row's path the same, so the messages are comparable; the ones that remove or add a row compare by text with the row's path left out.
"""
import copy
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT = r"C:\mcv\larecomp\perf\lfg-xcheck\dump\settings-ui.json"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(module)
    return module


contract = load("validate_contract", os.path.join(ROOT, "documentation/contract/validate_contract.py"))
ui_tool = load("check_settings_ui", os.path.join(HERE, "check_settings_ui.py"))
DICTIONARY = ui_tool.merged_dictionary(contract)


def findings(doc):
    """Every message the game's lints give for a dump, as a set of texts (a row's place is part of the text)."""
    report = contract.validate(doc, "settings-ui", DICTIONARY, strict=True)
    messages = {f"ERROR {m}" for m in report.errors} | {f"WARN {m}" for m in report.warnings}
    messages |= {f"WIDTH {m}" for m in ui_tool.width_errors(doc)}
    messages |= {f"VOCABULARY {m}" for m in ui_tool.vocabulary_errors(doc, contract)}
    return messages


def rows_of(doc):
    for ti, tab in enumerate(doc["tabs"]):
        for si, section in enumerate(tab["sections"]):
            for ri, row in enumerate(section["rows"]):
                yield tab, section, row


def row(doc, row_id):
    for _, _, r in rows_of(doc):
        if r["id"] == row_id:
            return r
    raise SystemExit(f"the dump has no row {row_id}")


def first_row(doc, **conditions):
    """The first non-debug row that has every field in `conditions` (a value, or a callable)."""
    for tab, section, r in rows_of(doc):
        if tab.get("debug") or section.get("debug") or r.get("debug"):
            continue
        if all((c(r.get(k)) if callable(c) else r.get(k) == c) for k, c in conditions.items()):
            return r
    raise SystemExit(f"the dump has no row like {conditions}")


# ---- the breaks: (name, function(doc) -> None) ----------------------------------------------------------------------------------------------
def unknown_kind(doc):
    first_row(doc, kind="choice")["kind"] = "slider"


def no_help(doc):
    del first_row(doc, kind="bool")["help"]


def unknown_function(doc):
    first_row(doc, kind="bool")["hidden_unless"] = "frobnicate(gpu.class)"


def unknown_fact(doc):
    first_row(doc, kind="bool")["hidden_unless"] = "gpu.not_a_fact == 'x'"


def syntax_error(doc):
    first_row(doc, kind="bool")["hidden_unless"] = "gpu.class == == 'low'"


def not_a_condition(doc):
    first_row(doc, kind="bool")["hidden_unless"] = "gpu.name"


def unguarded_live(doc):
    first_row(doc, kind="bool")["states"] = [{"when": "live.hdr_active", "enabled": False, "reason": "Because."}]


def generator_without_fallback(doc):
    r = first_row(doc, kind="choice", values_from=lambda v: v is not None)
    r.pop("fallback_values", None)


def value_the_setting_refuses(doc):
    r = first_row(doc, kind="choice", setting=lambda s: s and doc["settings"].get(s, {}).get("allowed"), values=lambda v: bool(v))
    r["values"][0]["id"] = "not-a-word-the-setting-takes"


def default_not_listed(doc):
    r = first_row(doc, kind="choice", values=lambda v: bool(v), default=lambda d: d is not None)
    r["default"] = "not-a-listed-value"


def unregistered_setting(doc):
    first_row(doc, kind="bool")["setting"] = "a_setting_nobody_registered"


def duplicate_row_id(doc):
    ids = [r["id"] for _, _, r in rows_of(doc) if r["id"].startswith("PM_")]
    row(doc, ids[1])["id"] = ids[0]


def facts_used_gap(doc):
    doc["facts_used"] = doc["facts_used"][1:]


def undefined_def(doc):
    first_row(doc, kind="bool")["hidden_unless"] = "d.a_def_nobody_wrote"


def def_cycle(doc):
    doc["defs"]["cycle_a"] = "d.cycle_b"
    doc["defs"]["cycle_b"] = "d.cycle_a"
    first_row(doc, kind="bool")["hidden_unless"] = "d.cycle_a"


def grey_without_reason(doc):
    first_row(doc, kind="bool")["states"] = [{"when": "gpu.class == 'low'", "enabled": False}]


def restart_on_a_live_setting(doc):
    r = first_row(doc, kind="bool", setting=lambda s: s and doc["settings"].get(s, {}).get("life") == "live")
    r["apply"] = "restart"
    r.pop("apply_note", None)


def value_without_label(doc):
    r = first_row(doc, kind="choice", values=lambda v: bool(v))
    del r["values"][0]["label"]


def link_to_nowhere(doc):
    row(doc, next(r["id"] for _, _, r in rows_of(doc) if r["kind"] == "link"))["target"] = "a_section_nobody_made"


def unknown_renderer_word(doc):
    first_row(doc, kind="bool")["renderers"] = ["vulkan"]


def unknown_surface(doc):
    first_row(doc, kind="bool")["surfaces"] = ["web"]


def debug_row_without_setting_flag(doc):
    # a row on a normal page that writes a debug-only setting (the player would see a debug switch)
    debug_setting = next(n for n, s in doc["settings"].items() if s.get("debug"))
    first_row(doc, kind="bool")["setting"] = debug_setting


def schema_version_word(doc):
    doc["schema_version"] = "one"


def missing_tabs(doc):
    del doc["tabs"]


def wide_page(doc):
    # a label that no page of 41 columns can show
    first_row(doc, kind="bool")["label"] = "A LABEL THAT IS FAR TOO LONG FOR ANY PAGE OF THE MENU TO SHOW"


def two_defaults_conflict(doc):
    # the row's default is not the setting's default and says nothing
    r = first_row(doc, kind="bool", setting=lambda s: s in doc["settings"])
    r["default"] = "maybe"


def template_with_unknown_fact(doc):
    r = first_row(doc, kind="bool")
    r["states"] = [{"when": "true", "suffix": " ({gpu.not_a_fact})", "reason": "Because."}]


BREAKS = [
    ("a row of an unknown kind", unknown_kind),
    ("a player row with no help", no_help),
    ("a rule that calls a function that does not exist", unknown_function),
    ("a rule that reads a fact that is not in the dictionary", unknown_fact),
    ("a rule with a syntax error", syntax_error),
    ("a hidden_unless that is not a condition", not_a_condition),
    ("a rule that reads the running game with no fallback", unguarded_live),
    ("a generated list with no fallback list", generator_without_fallback),
    ("a value the setting does not take", value_the_setting_refuses),
    ("a default that no value of the row has", default_not_listed),
    ("a row that names a setting nobody registered", unregistered_setting),
    ("two rows with one id", duplicate_row_id),
    ("facts_used that misses a fact the rules read", facts_used_gap),
    ("a rule that reads a def nobody wrote", undefined_def),
    ("two defs that read each other", def_cycle),
    ("a state that greys a row and says nothing", grey_without_reason),
    ("a restart word on a live setting with no note", restart_on_a_live_setting),
    ("a value with no label", value_without_label),
    ("a link to a section that does not exist", link_to_nowhere),
    ("a renderer word nobody knows", unknown_renderer_word),
    ("a surface nobody knows", unknown_surface),
    ("a player row that writes a debug setting", debug_row_without_setting_flag),
    ("a schema_version that is not a number", schema_version_word),
    ("a file with no tabs", missing_tabs),
    ("a label wider than a page", wide_page),
    ("a row default that is not a word of its bool", two_defaults_conflict),
    ("a template that reads a fact nobody has", template_with_unknown_fact),
]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    doc = contract.load_json(path)
    baseline = findings(doc)
    print(f"baseline: {len(baseline)} findings on the untouched dump ({sum(1 for m in baseline if m.startswith('ERROR'))} errors, {sum(1 for m in baseline if m.startswith('WARN'))} warnings, {sum(1 for m in baseline if m.startswith('WIDTH'))} width, {sum(1 for m in baseline if m.startswith('VOCABULARY'))} vocabulary)")
    escaped = []
    for name, brk in BREAKS:
        broken = copy.deepcopy(doc)
        try:
            brk(broken)
            after = findings(broken)
        except Exception as error:   # a dump the lints cannot even read is a dump they refuse
            print(f"CAUGHT   {name}: the lints stop on it ({type(error).__name__}: {str(error)[:100]})")
            continue
        new = sorted(after - baseline)
        if new:
            print(f"CAUGHT   {name}: {new[0][:170]}")
        else:
            print(f"ESCAPED  {name}")
            escaped.append(name)
    print(f"{len(BREAKS) - len(escaped)} of {len(BREAKS)} breaks reported, {len(escaped)} escaped")
    for name in escaped:
        print("  ESCAPED:", name)
    return 1 if escaped else 0


if __name__ == "__main__":
    sys.exit(main())
