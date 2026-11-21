r"""The authoring script of the G-buffer's per-texture material table (package GB-F2, perf/LIGHT-UNIFY.md 11.2; perf/GBUFFER-SURVEY.md 5.4).

  python -I author_materials.py --census census.tsv --rules material_rules.txt --out mcla_gbuffer_texture_materials.txt
        [--overrides <mods>\gbuffer_texture_materials.txt] [--min-draws N] [--keep-redundant]

census.tsv is census_merge.py's table (one row per texture the G-buffer's draws used, with its family's word). material_rules.txt is the table the owner edits: rules on the texture's
name, dictionary, shader group, the class the family gave it, its size and how much it is drawn, each giving some of the row's fields. The script applies every rule that matches,
in the file's order (a later rule's field replaces an earlier one's), drops the rows that would not change the word the family already gave, and writes the table the renderer
reads (src/gpu/gbuffer_materials.h, THE TEXT FORMAT): one row per texture, sorted, no dates, so that a new run's diff is only what changed.

THE RULES FILE, one rule per line (blank lines and # comments are free):

    <id>: <condition> <condition> ... => <field=value> ...

  conditions (all must hold; the word after ~ or = is a list separated by commas, text compared in lower case)
    name~a,b      the texture's name contains a or b          name!~a,b   contains none of them         name=a,b  is exactly one of them (a texture with no name matches only name!~)
    dict~a,b      the dictionary's name contains ...          dict=a,b    is one of them
    group~a,b     the shader group of the family (xCityRoad ...) contains ...    group=a,b  is one of them          group!~a,b  contains none
    class=a,b     the class the family gave it (world road paint ...)            class!=a,b  is none of them
    hash=a,b      the texture's content hash is one of the 16 digit numbers (how a reviewed list of unnamed textures goes into a rule)
    named  unnamed  vehicle  novehicle   the texture has / has no name; the family's word has / has not the vehicle flag
    draws>=N  indices>=N  size>=N  size<=N  smooth>=N  smooth<=N    how often it was drawn, the geometry it covered, its larger side in texels, the family's smoothness byte
  fields (the same as the table's): class=<name> rough=<0..1> smooth=<0..1> metal=<0..1> ssr=<0|1> lit=<0..1>, or the word skip, which gives up every field the earlier rules gave
  this texture (a later rule may give new ones).

A row is only written for a texture that has a key (a name, or a content hash: the census says '-' for an unnamed texture whose hash was never known) and only when it changes the
word (apply_row). The overrides file (the owner's own rows, read AFTER the generated table at run time) is only checked here: its rows are parsed with the renderer's rules and the
keys the census has never seen are listed (a typo, or a texture the tours missed).
"""

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gbm_common as g  # noqa: E402
import census_merge  # noqa: E402

COND_RE = re.compile(r"^(?P<field>[a-z]+)(?P<op>!~|~|!=|>=|<=|=)(?P<value>.+)$")
LIST_FIELDS = {"name", "dict", "group", "class", "hash"}
NUMBER_FIELDS = {"draws", "indices", "size", "smooth"}
FLAG_WORDS = {"named", "unnamed", "vehicle", "novehicle"}
OPS_FOR = {
    "name": {"~", "!~", "="}, "dict": {"~", "="}, "group": {"~", "!~", "="}, "class": {"=", "!="}, "hash": {"="},
    "draws": {">="}, "indices": {">="}, "size": {">=", "<="}, "smooth": {">=", "<="},
}


class RulesError(Exception):
    pass


class Rule:
    def __init__(self, ident, conditions, fields, skip, line):
        self.ident = ident
        self.conditions = conditions  # [(field, op, value)] value: set of lower case strings, an int, or None for the flag words
        self.fields = fields
        self.skip = skip
        self.line = line
        self.matched = 0


def parse_rules(text):
    """[Rule]; raises RulesError('line N: ...')."""
    rules = []
    seen = set()
    for number, raw in enumerate(text.split("\n"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue

        def fail(why, number=number):
            raise RulesError("line %d: %s" % (number, why))

        if "=>" not in line or ":" not in line.split("=>", 1)[0]:
            fail("a rule is <id>: <conditions> => <fields>")
        head, tail = line.split("=>", 1)
        ident, _, cond_text = head.partition(":")
        ident = ident.strip()
        if not ident or " " in ident or ident in seen:
            fail("the rule needs an id of its own (no spaces)")
        seen.add(ident)
        conditions = []
        for tok in cond_text.split():
            if tok in FLAG_WORDS:
                conditions.append((tok, None, None))
                continue
            m = COND_RE.match(tok)
            if m is None or m.group("field") not in OPS_FOR or m.group("op") not in OPS_FOR[m.group("field")]:
                fail("'%s' is not a condition" % tok)
            field, op, value = m.group("field"), m.group("op"), m.group("value")
            if field in LIST_FIELDS:
                items = {x.strip().lower() for x in value.split(",") if x.strip()}
                if not items:
                    fail("'%s' has an empty list" % tok)
                if field == "class":
                    for item in items:
                        if g.parse_class(item) is None:
                            fail("'%s' is not a class" % item)
                    items = {g.class_name(g.parse_class(i)) for i in items}
                if field == "hash":
                    try:
                        items = {int(i, 16) for i in items}
                    except ValueError:
                        fail("'%s': a hash is hexadecimal" % tok)
                conditions.append((field, op, items))
            else:
                try:
                    conditions.append((field, op, int(value)))
                except ValueError:
                    fail("'%s' is not a number" % value)
        toks = tail.split()
        skip = False
        if "skip" in toks:
            skip = True
            toks.remove("skip")
        try:
            fields = g.parse_fields(toks) if toks else {}
        except ValueError as e:
            fail(str(e))
        if not fields and not skip:
            fail("a rule gives at least one field, or skip")
        rules.append(Rule(ident, conditions, fields, skip, number))
    return rules


def row_hash(row):
    return int(row["hash"], 16) if row["hash"] != "-" else 0


def matches(rule, row):
    name = row["name"].lower()
    named = name != "unnamed"
    dictionary = row["dictionary"].lower()
    group = row["group"].lower()
    for field, op, value in rule.conditions:
        if field == "named":
            ok = named
        elif field == "unnamed":
            ok = not named
        elif field == "vehicle":
            ok = (row["flags"] & g.FLAG_VEHICLE) != 0
        elif field == "novehicle":
            ok = (row["flags"] & g.FLAG_VEHICLE) == 0
        elif field == "name":
            if op == "~":
                ok = named and any(k in name for k in value)
            elif op == "!~":
                ok = not (named and any(k in name for k in value))
            else:
                ok = named and name in value
        elif field == "dict":
            ok = any(k in dictionary for k in value) if op == "~" else dictionary in value
        elif field == "group":
            if op == "~":
                ok = any(k in group for k in value)
            elif op == "!~":
                ok = not any(k in group for k in value)
            else:
                ok = group in value
        elif field == "class":
            ok = (row["class"] in value) if op == "=" else (row["class"] not in value)
        elif field == "hash":
            ok = row_hash(row) in value
        else:
            actual = {"draws": row["draws"], "indices": row["indices"], "size": max(row["width"], row["height"]), "smooth": row["smooth"]}[field]
            ok = actual >= value if op == ">=" else actual <= value
        if not ok:
            return False
    return True


def author(rows, rules, smooth_defaults, lit_defaults, min_draws=1, keep_redundant=False):
    """([(key, fields, row, rule ids)], statistics) for the census rows."""
    out = []
    stats = defaultdict(int)
    for row in rows:
        if row["draws"] < min_draws:
            stats["below min-draws"] += 1
            continue
        fields = {}
        applied = []
        for rule in rules:
            if not matches(rule, row):
                continue
            rule.matched += 1
            if rule.skip:
                fields = {}
                applied = []
            fields.update(rule.fields)
            if rule.fields:
                applied.append(rule.ident)
        if not fields:
            stats["no rule"] += 1
            continue
        if row["key"] == "-":
            stats["no key (unnamed, hash never known)"] += 1
            continue
        word = int(row["word"], 16)
        if not keep_redundant and g.apply_row(word, fields, smooth_defaults, lit_defaults) == word:
            stats["row would not change the word"] += 1
            continue
        out.append((row["key"], fields, row, applied))
    out.sort(key=lambda item: item[0].lower())
    stats["rows written"] = len(out)
    return out, stats


def table_text(items, census_path, rules_path, rule_count, texture_count, draws):
    lines = [
        "# LARecomp G-buffer texture materials: GENERATED by tools/gbuffer_materials/author_materials.py, do not edit. Your own rows go in <mods>/gbuffer_texture_materials.txt, read after this file.",
        "# census %s: %d textures, %d draws; rules %s: %d rules; %d rows" % (census_path, texture_count, draws, rules_path, rule_count, len(items)),
        "@version %d" % g.TABLE_VERSION,
    ]
    for key, fields, row, applied in items:
        label = "%s %dx%d %s %d draws" % ("unnamed" if row["name"].lower() == "unnamed" else row["dictionary"], row["width"], row["height"], row["group"], row["draws"])
        lines.append("%s %s  # %s [%s]" % (key, g.fields_text(fields), label, ",".join(applied)))
    return "\n".join(lines) + "\n"


def check_overrides(path, rows):
    """Lines for the report: the overrides file's errors, and its keys that no census row has."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    parsed, errors = g.parse_table(text)
    lines = ["overrides %s: %d rows, %d bad" % (path, len(parsed), len(errors))]
    for number, why in errors[:20]:
        lines.append("  line %d: %s" % (number, why))
    known = {r["key"].lower() for r in rows}
    known_hash = {row_hash(r) for r in rows if row_hash(r)}
    known_names = {r["name"].lower() for r in rows}
    for key, _fields in parsed:
        if key[0] == "hash":
            hit = key[1] in known_hash
        elif key[0] == "any":
            hit = key[1].lower() in known_names
        else:
            hit = g.key_text(key).lower() in known
        if not hit:
            lines.append("  %s: no texture of the census has this key (a typo, or a texture the tours never drew)" % g.key_text(key))
    return lines, len(errors)


def main(argv=None):
    ap = argparse.ArgumentParser(description="turn a census table and the rules file into the generated material table (see the top of this file)")
    ap.add_argument("--census", required=True, help="census.tsv from census_merge.py")
    ap.add_argument("--rules", required=True, help="the rules file (material_rules.txt)")
    ap.add_argument("--out", required=True, help="the table to write (mcla_gbuffer_texture_materials.txt, beside the shader pack)")
    ap.add_argument("--overrides", help="the owner's rows file: checked, not merged")
    ap.add_argument("--min-draws", type=int, default=1, help="ignore textures drawn fewer times than this")
    ap.add_argument("--keep-redundant", action="store_true", help="also write rows that would not change the family's word")
    ap.add_argument("--class-header", help="gbuffer_material.h (default: the repository's): where the classes' default smoothness and lit weight are read")
    a = ap.parse_args(argv)
    try:
        rules = parse_rules(Path(a.rules).read_text(encoding="utf-8"))
    except RulesError as e:
        print("%s: %s" % (a.rules, e), file=sys.stderr)
        return 2
    rows = census_merge.read_tsv(a.census)
    smooth_defaults, lit_defaults = g.load_class_defaults(a.class_header)
    items, stats = author(rows, rules, smooth_defaults, lit_defaults, a.min_draws, a.keep_redundant)
    text = table_text(items, a.census, a.rules, len(rules), len(rows), sum(r["draws"] for r in rows))
    parsed, errors = g.parse_table(text)
    if errors:
        print("internal error: the generated table does not parse: %s" % errors[:3], file=sys.stderr)
        return 3
    Path(a.out).write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s: %d rows from %d textures" % (a.out, len(items), len(rows)))
    print("  not written: " + ", ".join("%s %d" % (k, v) for k, v in sorted(stats.items()) if k != "rows written") if len(stats) > 1 else "  every census texture got a row")
    for rule in rules:
        print("  rule %-28s matched %5d textures" % (rule.ident, rule.matched))
    status = 0
    if a.overrides:
        lines, bad = check_overrides(a.overrides, rows)
        for line in lines:
            print(line)
        status = 2 if bad else 0
    return status


if __name__ == "__main__":
    sys.exit(main())
