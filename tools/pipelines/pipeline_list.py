"""Reads and merges the renderer's pipeline lists (cache/mcla_gpu_pipelines_<api>.bin, src/gpu/pipelines.cpp).

  python pipeline_list.py info FILE...
  python pipeline_list.py merge -o assets/mcla_pipelines.bin FILE...

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
"""
import argparse
import struct
import sys

MAGIC = b"MCLAPSO\0"
VERSION = 1
KEY_SIZE = 104
BACKENDS = {0: "d3d12", 1: "vulkan"}
DEPTH_BIAS_AT = 92  # PipelineKey::depth_bias, then slope_scaled_depth_bias (float)


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


def info(paths):
    for p in paths:
        backend, layouts, records = read_list(p)
        biased = sum(1 for k, _ in records if k[DEPTH_BIAS_AT:DEPTH_BIAS_AT + 8] != b"\0" * 8)
        depth_only = sum(1 for k, _ in records if k[8:16] == b"\0" * 8)
        print(f"{p}: {backend}, {len(records)} records, {len(layouts)} layouts, "
              f"{depth_only} depth only, {biased} with a static depth bias")


def merge(paths, out):
    layouts, layout_index, records, seen = [], {}, [], set()
    for p in paths:
        _, file_layouts, file_records = read_list(p)
        added = 0
        for key, layout in file_records:
            if key in seen:
                continue
            h, body = file_layouts[layout]
            if h not in layout_index:
                layout_index[h] = len(layouts)
                layouts.append((h, body))
            seen.add(key)
            records.append((key, layout_index[h]))
            added += 1
        print(f"{p}: {len(file_records)} records, {added} new")
    blob = bytearray(MAGIC + struct.pack("<6I", VERSION, KEY_SIZE, 0, len(layouts), len(records), 0))
    for h, body in layouts:
        blob += struct.pack("<Q", h) + body
    for key, layout in records:
        blob += key + struct.pack("<I", layout)
    with open(out, "wb") as f:
        f.write(blob)
    print(f"{out}: {len(records)} records, {len(layouts)} layouts, {len(blob)} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info").add_argument("files", nargs="+")
    m = sub.add_parser("merge")
    m.add_argument("-o", "--out", required=True)
    m.add_argument("files", nargs="+")
    a = ap.parse_args()
    try:
        info(a.files) if a.cmd == "info" else merge(a.files, a.out)
    except (Bad, OSError, struct.error) as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
