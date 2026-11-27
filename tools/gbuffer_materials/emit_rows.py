r"""The emitter of the compiled-in texture rows (package PBR-2, perf/PBR.md section 2).

  python -I emit_rows.py --table <fixture_table.txt> [--table more ...] [--overrides fixture_overrides.txt] --out <local-output.inc> [--label text]

It turns a table and optional overrides (the text format of src/gpu/gbuffer_materials.h) into a fixture for the compiled layer's format (src/gpu/gbuffer_texture_rows.h): three arrays, one per kind of key,
each sorted by strictly increasing key and starting with an unused sentinel row, so that an empty table is a legal array.

  rows           the generated tables' rows first, in the order given; the overrides' rows after them; a later row of the same key REPLACES the earlier row whole (the renderer's rule for its files:
                 an override gives every field it wants).
  keys           hash:<hex>  -> the array of hashes, keyed by the number itself;  dictionary/name -> PairKey: FNV-1a 64 over the lower case text of the dictionary, '/', the name;
                 */name -> NameKey: FNV-1a 64 over the lower case name. The same function as src/gpu/gbuffer_materials.h (Fnv1aLower: tested against it by src/gpu/gbuffer_texture_rows_test.cpp
                 on the sample this tool made, tools/gbuffer_materials/sample_rows.inc).
  determinism    no date, no path of the machine: the same inputs give the same bytes, so that a new census's diff is only what changed. The comment of every row says its key in words, its fields
                 and the generated table's own comment (the rules that gave it, its confidence), so the .inc is readable and a reviewer can see why a texture has its row.
  never          the tool never opens an input for writing, and refuses an --out that is one of its inputs (the hand rows are safe from it).

Keep census output in the player's local runtime text table. Use synthetic inputs for fixtures in source control.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gbm_common as g  # noqa: E402

ROW_BITS = {"class": 1, "smooth": 2, "metal": 4, "ssr": 8, "lit": 16}
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


class EmitError(Exception):
    pass


def fnv1a_lower(text, h=FNV_OFFSET):
    """gbuffer_materials.h Fnv1aLower: FNV-1a 64 over the ASCII-lower-cased bytes of the text (only A-Z are lowered, as in the C++)."""
    for b in text.encode("utf-8"):
        if 65 <= b <= 90:
            b += 32
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def pair_key(dictionary, name):
    """gbuffer_materials.h PairKey: the name, hashed after '/', hashed after the dictionary."""
    return fnv1a_lower(name, fnv1a_lower("/", fnv1a_lower(dictionary)))


def name_key(name):
    return fnv1a_lower(name)


def canonical(key):
    """The identity of a key as the renderer's tables see it (the case of a name does not matter)."""
    if key[0] == "hash":
        return ("hash", key[1])
    if key[0] == "any":
        return ("name", key[1].lower())
    return ("pair", key[1].lower(), key[2].lower())


def numeric_key(canon):
    if canon[0] == "hash":
        return canon[1]
    if canon[0] == "name":
        return name_key(canon[1])
    return pair_key(canon[1], canon[2])


def read_rows(path):
    """[(key tuple, fields dict, comment)] of a table file; raises EmitError for any line the renderer's parser would refuse."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    rows = []
    for number, line in enumerate(text.split("\n"), 1):
        toks = g.tokens_of(line)
        if not toks:
            continue
        if toks[0].startswith("@"):
            if toks[0].lower() == "@version" and len(toks) == 2 and toks[1] == str(g.TABLE_VERSION):
                continue
            raise EmitError("%s:%d: '%s' is not the directive @version %d" % (path, number, " ".join(toks), g.TABLE_VERSION))
        try:
            key = g.parse_key(toks[0])
            fields = g.parse_fields(toks[1:])
        except ValueError as e:
            raise EmitError("%s:%d: %s" % (path, number, e))
        comment = line.split("#", 1)[1].strip() if "#" in line else ""
        rows.append((key, fields, comment))
    return rows


def merge(sources):
    """{canonical key: (key tuple, fields, comment, source label)}: later rows replace earlier ones of the same key whole. `sources` is [(label, rows)]."""
    merged = {}
    for label, rows in sources:
        for key, fields, comment in rows:
            merged[canonical(key)] = (key, fields, comment, label)
    return merged


def row_initializer(fields):
    """The TextureRow aggregate of a row, in the struct's field order (fields, cls, smooth, metal, lit, ssr); a field the row does not give holds the default of TextureRow{} so that an emitted row
    equals a parsed one."""
    mask = 0
    for name, bit in ROW_BITS.items():
        if name in fields:
            mask |= bit
    return "{%d, %d, %d, %d, %d, %s}" % (mask, fields.get("class", 0), fields.get("smooth", 0), fields.get("metal", 0), fields.get("lit", 255), "true" if fields.get("ssr") else "false")


def emit(sources, label=""):
    """The text of gbuffer_texture_rows.inc for the sources; raises EmitError for two keys with one number."""
    merged = merge(sources)
    arrays = {"hash": [], "pair": [], "name": []}
    seen = {}
    for canon, (key, fields, comment, source) in merged.items():
        number = numeric_key(canon)
        kind = canon[0]
        if number == 0:
            raise EmitError("the key %s has the number 0, which the sentinel row owns" % g.key_text(key))
        if (kind, number) in seen:
            raise EmitError("%s and %s have the same key number %016X in the %s array: give one of them another way to be found (a hash:)" % (g.key_text(key), seen[(kind, number)], number, kind))
        seen[(kind, number)] = g.key_text(key)
        arrays[kind].append((number, key, fields, comment, source))
    total = sum(len(v) for v in arrays.values())
    out = [
        "// GENERATED by tools/gbuffer_materials/emit_rows.py from a census and tools/gbuffer_materials/table_overrides.txt: do not edit, the next run overwrites it (our hand rows are table_overrides.txt).",
        "// %s%d rows: %d by hash, %d by dictionary/name, %d by name. Read by src/gpu/gbuffer_texture_rows.h; the layers and the lookup are described there." %
        ("%s. " % label if label else "", total, len(arrays["hash"]), len(arrays["pair"]), len(arrays["name"])),
        "inline constexpr uint32_t kCompiledRowsFormat = 1;",
    ]
    names = {"hash": "kCompiledHashRows", "pair": "kCompiledPairRows", "name": "kCompiledNameRows"}
    for kind in ("hash", "pair", "name"):
        out.append("inline constexpr CompiledRow %s[] = {" % names[kind])
        out.append("    {0, {}},  // unused sentinel: an empty table is a legal array")
        for number, key, fields, comment, source in sorted(arrays[kind], key=lambda item: item[0]):
            text = "%s %s" % (g.key_text(key), g.fields_text(fields))
            tail = ("  " + comment) if comment else ""
            if source:
                tail += " <%s>" % source
            out.append("    {0x%016Xull, %s},  // %s%s" % (number, row_initializer(fields), text, tail))
        out.append("};")
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="write the compiled-in texture rows (src/gpu/gbuffer_texture_rows.inc) from the generated table and our hand rows")
    ap.add_argument("--table", action="append", default=[], help="a generated table (author_materials.py's output); repeat for several, a later one replaces an earlier one's rows")
    ap.add_argument("--overrides", help="our hand rows (tools/gbuffer_materials/table_overrides.txt): read, never written; replace the generated rows of the same key")
    ap.add_argument("--out", required=True, help="the .inc to write")
    ap.add_argument("--label", default="", help="one line of provenance for the header comment (no date, no path)")
    a = ap.parse_args(argv)
    inputs = [Path(p).resolve() for p in a.table + ([a.overrides] if a.overrides else [])]
    if Path(a.out).resolve() in inputs:
        print("refusing to write %s: it is one of the inputs (the hand rows and the generated tables are never overwritten by this tool)" % a.out, file=sys.stderr)
        return 2
    try:
        sources = [(Path(p).name, read_rows(p)) for p in a.table]
        if a.overrides:
            sources.append(("hand", read_rows(a.overrides)))
        text = emit(sources, a.label)
    except EmitError as e:
        print("emit_rows: %s" % e, file=sys.stderr)
        return 2
    Path(a.out).write_text(text, encoding="utf-8", newline="\n")
    merged = merge(sources)
    print("wrote %s: %d rows (%d from the overrides)" % (a.out, len(merged), sum(1 for v in merged.values() if v[3] == "hand")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
