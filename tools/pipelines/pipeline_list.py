"""Reads and merges the renderer's pipeline lists (cache/mcla_gpu_pipelines_<api>.bin, src/gpu/pipelines.cpp).

  python pipeline_list.py info FILE...
  python pipeline_list.py merge -o assets/mcla_pipelines.bin FILE...
  python pipeline_list.py add --base assets/mcla_pipelines.bin [-o OUT] [--skip-level-copies] FILE...
  python pipeline_list.py check --pack mcla_shaders.v8.pack [--hdr-pack mcla_shaders_hdr.v1.pack] FILE...
  python pipeline_list.py reorder -o assets/mcla_pipelines.bin --base assets/mcla_pipelines.bin FILE...
  python pipeline_list.py selftest

A list holds no game data: each record is a pipeline key (the pack's shader hashes, spec variants,
target formats, blend/depth/stencil/raster state) and the input layout it was made with, by content.

merge writes the starter list the game precaches at start (assets/mcla_pipelines.bin, read by
Pipelines::LoadRecords as a D3D12 list): the records of every FILE, in order, without duplicates.
Keys keep their static depth bias: a device without a dynamic depth bias (D3D12 without options16)
needs it, and one with it zeroes it at load, as it does for another API's list. Give the files in the order their pipelines are usually first needed: the
precache makes them in that order. Prefer D3D12 lists from a device with 24-bit depth (NVIDIA, AMD):
a Vulkan list from a device without it holds D32 keys that a D24 device never asks for.
Rebuild the starter when the shader pack or PipelineKey changes: a record whose shader is not in the
pack is dropped at load, and a list of another version is ignored.

add puts the lists of runs (a run's cache/mcla_gpu_pipelines_<api>.bin) onto the starter. A run on a device
with the dynamic depth bias (every D3D12 and Vulkan device of today) zeroes the starter's static biases at
load and writes them zeroed, so its file holds the starter with the biases gone, then the records the starter
lacked. merge takes that copy for new records (a zeroed twin of every record that has a bias); add keeps BASE
as it is (order, biases) and appends, in each FILE's order, only the records that neither BASE nor an earlier
FILE has when the depth bias is not compared. A record from such a run has bias 0: a device with the static
bias asks for the biased key for a draw that has a bias (a dynamic device cannot record that) and makes it
itself. Without -o nothing is written, the counts only. For each FILE it prints its records, how many BASE
already had, how many are new, those by sample count, and how many of the new are a copy at another sample
count of a record the list holds with more than one sample (what the setup precache and a level change make
by themselves, Pipelines::MakeLevelCopies). --skip-level-copies leaves those out of the output: for a run at
an MSAA level other than the default 2, or under an upscaler (1 sample), whose records are the same
pipelines at 4, 8 or 1 samples.

check --pack: records whose vertex or pixel shader the pack does not have (the game drops them at load:
"N dropped: a shader the pack does not have"), a spec variant on a shader with no spec mask, and with
--hdr-pack the HDR scene variants (ps_variant 2) whose pixel shader has no entry in the HDR pack. The exit
status is 1 when a record has no shader or an HDR variant has no entry.

reorder keeps the records of --base and moves to the front those that also stand in the FILEs (the
depth bias is not compared), in the FILEs' order, the rest after them in their old order. Use it
with the list of a cold run that started with no starter: a list is written in the order the render
thread first needed each pipeline, so that run's list is the title's pipelines first (the first
frame of the title asks for about 360 at once), then the first drive's. The precache works through
the starter in order, and a first start on a weak CPU has to have the title's before the first frame.
"""
import argparse
import contextlib
import io
import os
import struct
import sys
import tempfile

MAGIC = b"MCLAPSO\0"
VERSION = 1
KEY_SIZE = 104
BACKENDS = {0: "d3d12", 1: "vulkan"}
# PipelineKey (src/gpu/pipelines.h, 104 bytes, no padding): vs u64, ps u64, then u32: vs_variant, ps_variant,
# input_layout, topology, sample_count, color_count, color_format[4], depth_format, blend[4], depth, stencil,
# stencil_masks, raster, depth_bias, slope_scaled_depth_bias (a float), reserved.
VS_AT, PS_AT = 0, 8
VS_VARIANT_AT, PS_VARIANT_AT = 16, 20
SAMPLES_AT = 32
DEPTH_FORMAT_AT = 56
DEPTH_BIAS_AT = 92  # PipelineKey::depth_bias, then slope_scaled_depth_bias (float)
HDR_VARIANT = 2  # ShaderStore::kHdrVariant

# The shader pack (src/shaderpack/pack_format.h): "MSPK", version, count, formats, 16 reserved; entries of 88
# bytes (key u64, stage u8, spec_mask u8, ...). The HDR pack: "MSHV", version, count, ...; entries of 80 bytes,
# the pixel shader's key first.
PACK_MAGIC, PACK_VERSION, PACK_HEADER, PACK_ENTRY = b"MSPK", 8, 32, 88
HDR_PACK_MAGIC, HDR_PACK_VERSION, HDR_PACK_HEADER, HDR_PACK_ENTRY = b"MSHV", 1, 32, 80


class Bad(Exception):
    pass


def read_list(path):
    data = open(path, "rb").read()
    if len(data) < 32 or data[:8] != MAGIC:
        raise Bad(f"{path}: not a pipeline list")
    version, key_size, backend, layout_count, record_count, _ = struct.unpack_from("<6I", data, 8)
    if version != VERSION or key_size != KEY_SIZE:
        raise Bad(f"{path}: version {version}, key size {key_size} (this tool reads {VERSION}, {KEY_SIZE})")
    at = 32
    layouts = []  # (hash, the bytes after the hash)
    for _ in range(layout_count):
        start = at
        (h,) = struct.unpack_from("<Q", data, at)
        at += 8
        elements, slots = struct.unpack_from("<2I", data, at)
        at += 8
        for _ in range(elements):
            (n,) = struct.unpack_from("<I", data, at)
            at += 4 + n + 20
        at += 12 * slots
        if at > len(data):
            raise Bad(f"{path}: a layout is cut short")
        layouts.append((h, data[start + 8:at]))
    records = []
    for _ in range(record_count):
        if at + KEY_SIZE + 4 > len(data):
            raise Bad(f"{path}: a record is cut short")
        key = data[at:at + KEY_SIZE]
        (layout,) = struct.unpack_from("<I", data, at + KEY_SIZE)
        at += KEY_SIZE + 4
        if layout >= len(layouts):
            raise Bad(f"{path}: a record names no layout")
        records.append((key, layout))
    return BACKENDS.get(backend, str(backend)), layouts, records


def u32(key, at):
    return struct.unpack_from("<I", key, at)[0]


def samples_of(key):
    return u32(key, SAMPLES_AT)


def without_bias(key):
    return key[:DEPTH_BIAS_AT] + key[DEPTH_BIAS_AT + 8:]


def canonical(key):
    """The key without its depth bias and sample count: one pipeline across the MSAA levels."""
    return without_bias(key[:SAMPLES_AT] + b"\0\0\0\0" + key[SAMPLES_AT + 4:])


def by_samples(keys):
    counts = {}
    for key in keys:
        counts[samples_of(key)] = counts.get(samples_of(key), 0) + 1
    return counts


def fmt_counts(counts):
    return " ".join(f"{n}:{counts[n]}" for n in sorted(counts)) or "none"


def info(paths):
    for p in paths:
        backend, layouts, records = read_list(p)
        keys = [k for k, _ in records]
        biased = sum(1 for k in keys if k[DEPTH_BIAS_AT:DEPTH_BIAS_AT + 8] != b"\0" * 8)
        depth_only = sum(1 for k in keys if k[PS_AT:PS_AT + 8] == b"\0" * 8)
        hdr = sum(1 for k in keys if u32(k, PS_VARIANT_AT) == HDR_VARIANT)
        print(f"{p}: {backend}, {len(records)} records, {len(layouts)} layouts, "
              f"{depth_only} depth only, {biased} with a static depth bias")
        print(f"    {len({without_bias(k) for k in keys})} distinct without the bias (what a device with the "
              f"dynamic bias keeps); by sample count {fmt_counts(by_samples(keys))}; {hdr} HDR scene variants")


class Collector:
    """Records and the layouts they name, the layouts shared by their hash (as the game stores them)."""

    def __init__(self):
        self.layouts, self.layout_index, self.records = [], {}, []

    def push(self, key, layout_hash, body):
        if layout_hash not in self.layout_index:
            self.layout_index[layout_hash] = len(self.layouts)
            self.layouts.append((layout_hash, body))
        self.records.append((key, self.layout_index[layout_hash]))


def merge(paths, out):
    c, seen = Collector(), set()
    for p in paths:
        _, file_layouts, file_records = read_list(p)
        added = 0
        for key, layout in file_records:
            if key in seen:
                continue
            seen.add(key)
            c.push(key, *file_layouts[layout])
            added += 1
        print(f"{p}: {len(file_records)} records, {added} new")
    write_list(out, c.layouts, c.records)


def add(base, files, out, skip_level_copies=False):
    """BASE as it is, then the records of each FILE that BASE and the earlier FILEs lack (the depth bias
    is not compared). Writes OUT when it is given. Returns the Collector."""
    _, base_layouts, base_records = read_list(base)
    c = Collector()
    seen = set()  # keys without the bias
    held = {}  # canonical key -> the sample counts the list holds for it
    for key, layout in base_records:
        c.push(key, *base_layouts[layout])
        seen.add(without_bias(key))
        held.setdefault(canonical(key), set()).add(samples_of(key))
    print(f"{base}: {len(base_records)} records, kept as they are")
    for p in files:
        _, file_layouts, file_records = read_list(p)
        present = skipped = copies = 0
        new = []
        for key, layout in file_records:
            if without_bias(key) in seen:
                present += 1
                continue
            s = samples_of(key)
            copy = any(t > 1 and t != s for t in held.get(canonical(key), ()))
            new.append(key)
            copies += copy
            if copy and skip_level_copies:
                skipped += 1
                continue
            c.push(key, *file_layouts[layout])
            seen.add(without_bias(key))
            held.setdefault(canonical(key), set()).add(s)
        print(f"{p}: {len(file_records)} records, {present} already in the list, {len(new)} new "
              f"(by sample count {fmt_counts(by_samples(new))}); {copies} of the new are a copy of a record at "
              f"another sample count" + (f", {skipped} left out" if skip_level_copies else ""))
    if out:
        write_list(out, c.layouts, c.records)
    else:
        print(f"no -o: nothing written; the list would have {len(c.records)} records "
              f"({len(c.records) - len(base_records)} more than {base})")
    return c


def write_list(out, layouts, records):
    blob = bytearray(MAGIC + struct.pack("<6I", VERSION, KEY_SIZE, 0, len(layouts), len(records), 0))
    for h, body in layouts:
        blob += struct.pack("<Q", h) + body
    for key, layout in records:
        blob += key + struct.pack("<I", layout)
    with open(out, "wb") as f:
        f.write(blob)
    print(f"{out}: {len(records)} records, {len(layouts)} layouts, {len(blob)} bytes")


def reorder(base, firsts, out):
    _, layouts, records = read_list(base)
    where = {}
    for i, (key, _) in enumerate(records):
        where.setdefault(without_bias(key), []).append(i)
    front, taken = [], set()
    for p in firsts:
        _, _, file_records = read_list(p)
        found = 0
        for key, _ in file_records:
            for i in where.get(without_bias(key), ()):
                if i not in taken:
                    taken.add(i)
                    front.append(i)
                    found += 1
        print(f"{p}: {len(file_records)} records, {found} of them in {base}")
    order = front + [i for i in range(len(records)) if i not in taken]
    write_list(out, layouts, [records[i] for i in order])


def read_pack(path, magic, version, header, entry):
    """The shader keys of a pack: key -> the spec mask (main pack) or None (HDR pack)."""
    data = open(path, "rb").read()
    if len(data) < header or data[:4] != magic:
        raise Bad(f"{path}: not a shader pack ({magic.decode()})")
    got, count = struct.unpack_from("<2I", data, 4)
    if got != version:
        print(f"{path}: pack version {got}, this tool knows {version}", file=sys.stderr)
    if header + count * entry > len(data):
        raise Bad(f"{path}: the entries are cut short")
    keys = {}
    for i in range(count):
        at = header + i * entry
        (key,) = struct.unpack_from("<Q", data, at)
        keys[key] = data[at + 9] if magic == PACK_MAGIC else None
    return keys


def check(pack, hdr_pack, paths):
    shaders = read_pack(pack, PACK_MAGIC, PACK_VERSION, PACK_HEADER, PACK_ENTRY)
    hdr = read_pack(hdr_pack, HDR_PACK_MAGIC, HDR_PACK_VERSION, HDR_PACK_HEADER, HDR_PACK_ENTRY) if hdr_pack else None
    print(f"{pack}: {len(shaders)} shaders" + (f"; {hdr_pack}: {len(hdr)} HDR variants" if hdr is not None else ""))
    bad = False
    for p in paths:
        _, _, records = read_list(p)
        no_vs = no_ps = no_spec = hdr_total = hdr_missing = 0
        for key, _ in records:
            vs, ps = struct.unpack_from("<2Q", key, 0)
            if vs not in shaders:
                no_vs += 1
            elif u32(key, VS_VARIANT_AT) == 1 and shaders[vs] == 0:
                no_spec += 1
            if ps != 0 and ps not in shaders:
                no_ps += 1
            elif ps != 0 and u32(key, PS_VARIANT_AT) == 1 and shaders[ps] == 0:
                no_spec += 1
            if u32(key, PS_VARIANT_AT) == HDR_VARIANT:
                hdr_total += 1
                if hdr is not None and ps not in hdr:
                    hdr_missing += 1
        tail = (f", {hdr_missing} without an entry in the HDR pack" if hdr is not None
                else (" (give --hdr-pack to check them)" if hdr_total else ""))
        print(f"{p}: {len(records)} records; no vertex shader {no_vs}, no pixel shader {no_ps}, "
              f"a spec variant without a spec mask {no_spec}; {hdr_total} HDR scene variants{tail}")
        bad |= bool(no_vs or no_ps or hdr_missing)
    return 1 if bad else 0


# --- selftest: made-up lists and packs, no game data -----------------------------------------------------

def _key(vs, ps, samples=2, bias=0, ps_variant=0, vs_variant=0):
    k = bytearray(KEY_SIZE)
    struct.pack_into("<2Q", k, VS_AT, vs, ps)
    struct.pack_into("<I", k, VS_VARIANT_AT, vs_variant)
    struct.pack_into("<I", k, PS_VARIANT_AT, ps_variant)
    struct.pack_into("<I", k, SAMPLES_AT, samples)
    struct.pack_into("<i", k, DEPTH_BIAS_AT, bias)
    return bytes(k)


def _layout(h):
    return (h, struct.pack("<2I", 0, 0))  # no elements, no slots


def _pack(path, magic, version, entries, entry_size):
    blob = bytearray(magic + struct.pack("<3I", version, len(entries), 1) + bytes(16))
    for key, spec in entries:
        e = bytearray(entry_size)
        struct.pack_into("<Q", e, 0, key)
        e[8], e[9] = ord("p"), spec
        blob += e
    with open(path, "wb") as f:
        f.write(blob)


def _quiet(fn, *a):
    with contextlib.redirect_stdout(io.StringIO()) as s:
        r = fn(*a)
    return r, s.getvalue()


def selftest():
    failures = []

    def expect(ok, what):
        if not ok:
            failures.append(what)
            print("FAIL:", what)

    with tempfile.TemporaryDirectory() as d:
        def path(n):
            return os.path.join(d, n)

        def write(n, recs, layouts):
            _quiet(write_list, path(n), layouts, recs)

        # the starter: a record with a static bias (a), one without (b), a 4-sample one (c4)
        a, b, c4 = _key(1, 2, 2, bias=-7), _key(3, 4, 2), _key(5, 6, 4)
        write("base.bin", [(a, 0), (b, 0), (c4, 1)], [_layout(10), _layout(11)])
        # a run on a dynamic-bias device: the starter with the biases zeroed, then what it lacked
        a0 = _key(1, 2, 2, bias=0)
        new1, new2, copy1 = _key(7, 8, 2), _key(9, 10, 1), _key(5, 6, 2)  # copy1: c4 at 2 samples
        write("run1.bin", [(a0, 0), (b, 0), (c4, 1), (new1, 0), (copy1, 1), (new2, 2)],
              [_layout(10), _layout(11), _layout(12)])
        new3 = _key(11, 12, 2)
        write("run2.bin", [(a0, 0), (new1, 0), (new3, 0)], [_layout(10)])

        # merge compares whole keys: the zeroed twin counts as new
        _quiet(merge, [path("base.bin"), path("run1.bin")], path("merged.bin"))
        expect(len(read_list(path("merged.bin"))[2]) == 3 + 4, "merge adds the zeroed twin and the 3 new records")

        # add does not
        _, text = _quiet(add, path("base.bin"), [path("run1.bin"), path("run2.bin")], path("added.bin"))
        _, layouts, recs = read_list(path("added.bin"))
        keys = [k for k, _ in recs]
        expect(keys[:3] == [a, b, c4], "add keeps the base's records, order and bias")
        expect(keys[3:] == [new1, copy1, new2, new3], f"add appends the new records in file order ({len(keys) - 3} added)")
        expect(a0 not in keys, "add does not take the zeroed twin of a biased record")
        expect([h for h, _ in layouts] == [10, 11, 12] and all(i < len(layouts) for _, i in recs), "add shares layouts by hash")
        run1_line = [ln for ln in text.splitlines() if ln.startswith(path("run1.bin"))][0]
        expect("3 already in the list, 3 new" in run1_line and "1 of the new are a copy" in run1_line,
               f"add counts the new records and the level copy ({run1_line})")
        run2_line = [ln for ln in text.splitlines() if ln.startswith(path("run2.bin"))][0]
        expect("2 already in the list, 1 new" in run2_line, f"a later file only adds what the earlier ones did not ({run2_line})")

        _quiet(add, path("base.bin"), [path("run1.bin")], path("nocopy.bin"), True)
        keys = [k for k, _ in read_list(path("nocopy.bin"))[2]]
        expect(copy1 not in keys and len(keys) == 5, "--skip-level-copies leaves the level copy out")

        _, text = _quiet(add, path("base.bin"), [path("run1.bin")], "")
        expect("nothing written" in text and "6 records" in text, f"without -o only the counts come ({text.splitlines()[-1]})")

        # reorder
        _quiet(reorder, path("base.bin"), [path("run1.bin")], path("re.bin"))
        expect([k for k, _ in read_list(path("re.bin"))[2]] == [a, b, c4], "reorder keeps the records")

        # info
        _, text = _quiet(info, [path("base.bin")])
        expect("3 records" in text and "2:2 4:1" in text, f"info says the sample counts ({text.strip()})")

        # check against a pack and an HDR pack
        _pack(path("pack"), PACK_MAGIC, PACK_VERSION, [(n, 0) for n in range(1, 9)], PACK_ENTRY)
        _pack(path("hdr"), HDR_PACK_MAGIC, HDR_PACK_VERSION, [(2, 0)], HDR_PACK_ENTRY)
        write("chk.bin", [(_key(1, 2), 0), (_key(1, 99), 0), (_key(77, 2), 0), (_key(1, 2, ps_variant=1), 0),
                          (_key(3, 4, ps_variant=2), 0)], [_layout(10)])
        status, text = _quiet(check, path("pack"), path("hdr"), [path("chk.bin")])
        expect(status == 1, "check fails when a shader is missing")
        expect("no vertex shader 1, no pixel shader 1, a spec variant without a spec mask 1; 1 HDR scene variants, "
               "1 without" in text, f"check counts ({text.splitlines()[-1]})")
        write("ok.bin", [(_key(1, 2), 0), (_key(3, 4), 0)], [_layout(10)])
        expect(_quiet(check, path("pack"), "", [path("ok.bin")])[0] == 0, "check passes a list whose shaders are all there")

        # junk is refused
        with open(path("junk.bin"), "wb") as f:
            f.write(b"nope")
        try:
            read_list(path("junk.bin"))
            expect(False, "a junk file is refused")
        except Bad:
            pass
    print("selftest:", "FAILED" if failures else "ok")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info").add_argument("files", nargs="+")
    m = sub.add_parser("merge")
    m.add_argument("-o", "--out", required=True)
    m.add_argument("files", nargs="+")
    ad = sub.add_parser("add")
    ad.add_argument("--base", required=True)
    ad.add_argument("-o", "--out", default="")
    ad.add_argument("--skip-level-copies", action="store_true")
    ad.add_argument("files", nargs="+")
    ck = sub.add_parser("check")
    ck.add_argument("--pack", required=True)
    ck.add_argument("--hdr-pack", default="")
    ck.add_argument("files", nargs="+")
    r = sub.add_parser("reorder")
    r.add_argument("-o", "--out", required=True)
    r.add_argument("--base", required=True)
    r.add_argument("files", nargs="+")
    sub.add_parser("selftest")
    a = ap.parse_args()
    try:
        if a.cmd == "info":
            info(a.files)
        elif a.cmd == "merge":
            merge(a.files, a.out)
        elif a.cmd == "add":
            add(a.base, a.files, a.out, a.skip_level_copies)
        elif a.cmd == "check":
            sys.exit(check(a.pack, a.hdr_pack, a.files))
        elif a.cmd == "selftest":
            sys.exit(selftest())
        else:
            reorder(a.base, a.files, a.out)
    except (Bad, OSError, struct.error) as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
