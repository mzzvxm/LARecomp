"""Shared pieces of the G-buffer material tools (package GB-F2 of perf/LIGHT-UNIFY.md 11.2).

The renderer side is src/gpu/gbuffer_materials.h (the text format of the table, what a row does to a material word, the census lines) and src/gpu/gbuffer_material.h (the class
defaults). This module is their Python twin: the census line reader, the shader names file, the table text parser and writer, and the row's arithmetic. The tests
(test_gbuffer_materials.py, and src/gpu/gbuffer_materials_test.cpp which reads the same sample_table.txt) keep the two from drifting.

Run the tools with `python -I`: the isolated mode does not put this folder on the path, so each tool adds it itself.
"""

import re
import struct
from collections import namedtuple
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

# ---------------------------------------------------------------------------
# classes, the material word
# ---------------------------------------------------------------------------

CLASS_NAMES = ["none", "world", "road", "wetroad", "sidewalk", "terrain", "lod", "foliage",
               "paint", "chrome", "metal", "glass", "skin", "emissive", "interior", "water"]
FLAG_SSR, FLAG_ALPHA, FLAG_VEHICLE = 1, 2, 4

ROW_FIELDS = ("class", "rough", "smooth", "metal", "ssr", "lit")


def f32(x):
    """x rounded to a 32 bit float (the renderer does its byte arithmetic in float)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def to_byte(unit):
    """gbuffer_material.h ToByte: a number 0..1 as an unorm8, rounded."""
    unit = f32(unit)
    if unit <= 0.0:
        return 0
    if unit >= 1.0:
        return 255
    return int(f32(f32(unit * 255.0) + 0.5))


def class_name(number):
    return CLASS_NAMES[number] if 0 <= number < len(CLASS_NAMES) else "class%d" % number


def parse_class(text):
    """A class number from a name (any case) or a number 0..15; None when it is neither."""
    t = text.strip().lower()
    if t in CLASS_NAMES:
        return CLASS_NAMES.index(t)
    if t.isdigit() and len(t) <= 2 and int(t) < len(CLASS_NAMES):
        return int(t)
    return None


Word = namedtuple("Word", "smooth cls flags lit metal")


def split_word(word):
    return Word(word & 255, (word >> 8) & 31, (word >> 13) & 7, (word >> 16) & 255, (word >> 24) & 255)


def join_word(w):
    return (w.smooth & 255) | ((w.cls & 31) << 8) | ((w.flags & 7) << 13) | ((w.lit & 255) << 16) | ((w.metal & 255) << 24)


def load_class_defaults(header=None):
    """(DefaultSmoothness, DefaultLit) of every class as unorm8 lists, read from src/gpu/gbuffer_material.h so that the tool and the renderer have one table."""
    path = Path(header) if header else REPO / "src" / "gpu" / "gbuffer_material.h"
    text = path.read_text(encoding="utf-8", errors="replace")
    start = text.index("constexpr float DefaultSmoothness")
    body = text[start:text.index("\n}\n", start)]
    smooth = {}
    for m in re.finditer(r"case SurfaceClass::k(\w+):\s*return\s*([0-9.]+)f;", body):
        smooth[m.group(1).lower()] = to_byte(float(m.group(2)))
    lit_match = re.search(r"DefaultLit\(SurfaceClass cls\)\s*\{\s*return cls == SurfaceClass::k(\w+) \? ([0-9.]+)f : ([0-9.]+)f;", text)
    if lit_match is None:
        raise ValueError("DefaultLit not found in " + str(path))
    special, lit_special, lit_other = lit_match.group(1).lower(), to_byte(float(lit_match.group(2))), to_byte(float(lit_match.group(3)))
    smooth_list = []
    lit_list = []
    for name in CLASS_NAMES:
        if name not in smooth:
            raise ValueError("DefaultSmoothness has no row for class " + name)
        smooth_list.append(smooth[name])
        lit_list.append(lit_special if name == special else lit_other)
    return smooth_list, lit_list


# ---------------------------------------------------------------------------
# the table text (gbuffer_materials.h, "THE TEXT FORMAT")
# ---------------------------------------------------------------------------

TABLE_VERSION = 1
HEX_RE = re.compile(r"^(?:0[xX])?([0-9a-fA-F]{1,16})$")


def tokens_of(line):
    out = []
    for tok in line.split():
        if tok.startswith("#"):
            break
        out.append(tok)
    return out


def parse_key(text):
    """('hash', int) | ('pair', dictionary, name) | ('any', name), or raises ValueError with the renderer's wording."""
    if len(text) > 5 and text[:5].lower() == "hash:":
        m = HEX_RE.match(text[5:])
        if m is None:
            raise ValueError("a hash is 1 to 16 hexadecimal digits")
        value = int(m.group(1), 16)
        if value == 0:
            raise ValueError("the hash 0 is no texture")
        return ("hash", value)
    parts = text.split("/")
    if len(parts) != 2 or not parts[0] or not parts[1]:
        raise ValueError("a key is dictionary/name, */name or hash:<hex>")
    if parts[1].lower() == "unnamed":
        raise ValueError("an unnamed texture has no name to key by: use hash:<hex> (the census and the dumps give it)")
    if parts[0] == "*":
        return ("any", parts[1])
    return ("pair", parts[0], parts[1])


def _unit(text):
    try:
        v = float(text)
    except ValueError:
        raise ValueError("'%s' is not a number from 0 to 1" % text)
    if not (0.0 <= v <= 1.0) or v != v:
        raise ValueError("'%s' is not a number from 0 to 1" % text)
    return v


def parse_fields(tokens):
    """The fields of a row as a dict: class (number), smooth/metal/lit (unorm8), ssr (bool). Raises ValueError."""
    if not tokens:
        raise ValueError("a row with no field changes nothing (class= rough= smooth= metal= ssr= lit=)")
    row = {}
    for tok in tokens:
        if "=" not in tok or tok.startswith("=") or tok.endswith("="):
            raise ValueError("'%s' is not field=value" % tok)
        field, value = tok.split("=", 1)
        f = field.lower()
        if f == "class":
            c = parse_class(value)
            if c is None:
                raise ValueError("'%s' is not a class (%s, or 0 to 15)" % (value, " ".join(CLASS_NAMES)))
            key, val = "class", c
        elif f in ("rough", "smooth"):
            u = _unit(value)
            key, val = "smooth", to_byte(1.0 - f32(u) if f == "rough" else u)
        elif f == "metal":
            key, val = "metal", to_byte(_unit(value))
        elif f == "lit":
            key, val = "lit", to_byte(_unit(value))
        elif f == "ssr":
            v = value.lower()
            if v in ("1", "on", "true", "yes"):
                key, val = "ssr", True
            elif v in ("0", "off", "false", "no"):
                key, val = "ssr", False
            else:
                raise ValueError("'%s' is not 0 or 1" % value)
        else:
            raise ValueError("'%s' is not a field (class rough smooth metal ssr lit)" % field)
        if key in row:
            raise ValueError("the field '%s' is given twice" % field)
        row[key] = val
    return row


def parse_table(text):
    """(rows, errors): rows is a list of (key tuple, fields dict) in file order, errors a list of (line number, message). A file of another version gives no rows."""
    rows = []
    errors = []
    for number, line in enumerate(text.split("\n"), 1):
        toks = tokens_of(line)
        if not toks:
            continue
        if toks[0].startswith("@"):
            if toks[0].lower() == "@version" and len(toks) == 2:
                if toks[1] != str(TABLE_VERSION):
                    errors.append((number, "a table of version %s (this build reads %d): the file is not used" % (toks[1], TABLE_VERSION)))
                    return [], errors
            else:
                errors.append((number, "'%s' is not a directive (@version 1)" % toks[0]))
            continue
        try:
            key = parse_key(toks[0])
            fields = parse_fields(toks[1:])
        except ValueError as e:
            errors.append((number, str(e)))
            continue
        rows.append((key, fields))
    return rows, errors


def key_text(key):
    if key[0] == "hash":
        return "hash:%016X" % key[1]
    if key[0] == "any":
        return "*/" + key[1]
    return key[1] + "/" + key[2]


def fields_text(fields, rough_decimals=2):
    """The field tokens of a row in the canonical order; smoothness is written as rough= (the file's own word for it)."""
    out = []
    if "class" in fields:
        out.append("class=" + class_name(fields["class"]))
    if "smooth" in fields:
        out.append("rough=%.*f" % (rough_decimals, 1.0 - fields["smooth"] / 255.0))
    if "metal" in fields:
        out.append("metal=%.2f" % (fields["metal"] / 255.0))
    if "ssr" in fields:
        out.append("ssr=%d" % (1 if fields["ssr"] else 0))
    if "lit" in fields:
        out.append("lit=%.2f" % (fields["lit"] / 255.0))
    return " ".join(out)


def apply_row(word, fields, smooth_defaults, lit_defaults):
    """gbuffer_materials.h ApplyTextureRow on a word (an int)."""
    if not fields:
        return word
    w = split_word(word)
    if w.cls == 0:
        return word
    cls, flags, smooth, lit, metal = w.cls, w.flags, w.smooth, w.lit, w.metal
    if "class" in fields:
        cls = fields["class"] & 31
        if "smooth" not in fields:
            smooth = smooth_defaults[cls] if cls < len(smooth_defaults) else 0
        if "lit" not in fields:
            lit = lit_defaults[cls] if cls < len(lit_defaults) else 255
    if "smooth" in fields:
        smooth = fields["smooth"]
    if "lit" in fields:
        lit = fields["lit"]
    if "metal" in fields:
        metal = fields["metal"]
    if "ssr" in fields:
        flags = (flags | FLAG_SSR) if fields["ssr"] else (flags & ~FLAG_SSR)
    return join_word(Word(smooth, cls, flags, lit, metal))


# ---------------------------------------------------------------------------
# the census (gbuffer_materials.h TextureCensus)
# ---------------------------------------------------------------------------

CensusRow = namedtuple("CensusRow", "frames dictionary name hash ps width height fmt draws indices word words_changed rows")
CensusWindow = namedtuple("CensusWindow", "frames textures pairs draws indices omitted")

LINE_MARK = "texcensus "


def parse_census_line(text):
    """A CensusRow or a CensusWindow for a log line that carries a census line, None for any other line."""
    at = text.find(LINE_MARK + "tc1")
    if at < 0:
        return None
    fields = text[at + len(LINE_MARK):].rstrip("\r\n").split("|")
    try:
        if fields[0] == "tc1" and len(fields) == 14:
            return CensusRow(int(fields[1]), fields[2], fields[3], int(fields[4], 16), int(fields[5], 16), int(fields[6]), int(fields[7]), int(fields[8]),
                             int(fields[9]), int(fields[10]), int(fields[11], 16), int(fields[12]), int(fields[13]))
        if fields[0] == "tc1-window" and len(fields) == 7:
            return CensusWindow(*[int(x) for x in fields[1:]])
    except ValueError:
        return None
    return None


ShaderInfo = namedtuple("ShaderInfo", "stage group entry cls flags smooth lit")


def load_shader_names(path):
    """The shader names file the pack build writes (mcla_gbuffer_shader_names.v1.tsv): {identity: ShaderInfo}."""
    names = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline().rstrip("\r\n").split("\t")
        if header[:4] != ["key", "stage", "group", "entry"]:
            raise ValueError("%s is not a shader names file" % path)
        for line in f:
            c = line.rstrip("\r\n").split("\t")
            if len(c) < 8:
                continue
            names[int(c[0], 16)] = ShaderInfo(c[1], c[2], c[3], c[4], c[5], c[6], c[7])
    return names


DUMP_RE = re.compile(r"^(?P<name>.*)__(?P<hash>[0-9a-fA-F]{16})\.dds$")


def load_dump_listing(folder):
    """[(dictionary, name, hash)] from the file NAMES of a texture dump folder (<dictionary>/<name>__<hash16>.dds): no texture is opened."""
    out = []
    root = Path(folder)
    for sub in sorted(p for p in root.iterdir() if p.is_dir()):
        for f in sorted(sub.iterdir()):
            m = DUMP_RE.match(f.name)
            if m:
                out.append((sub.name, m.group("name"), int(m.group("hash"), 16)))
    return out
