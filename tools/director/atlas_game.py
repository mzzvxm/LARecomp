#!/usr/bin/env python3
"""Read atlas inputs from a local copy of xarchive_cache.rpf.

RPF3 uses AES-256 for its directory. The key is read at runtime from the
LARecomp checkout. Resources use Xbox XCompress framing over LZX.

The Python LZX decoder is adapted from Stuart Caie's libmspack lzxd
(copyright 2003-2023, LGPL 2.1). See LICENSES/larecomp_mspack_notice.txt
and LICENSES/larecomp_mspack_license.txt.
"""
import math
import os
import re
import struct
import sys
import zlib
from pathlib import Path

try:
    import numpy as np
except ImportError:  # NumPy accelerates the archive directory when available.
    np = None

RPF_NAME = "xarchive_cache.rpf"
TRAFFIC_PATH = "resources/city/sc/traffic.xrn"
LIGHTS_PATH = "resources/city/sc/lights.xlm"


# ------------------------------------------------------------------------------------------------------------------ rage hash
def rage_hash(text):
    h = 0
    for ch in text.encode("latin1"):
        if 65 <= ch <= 90:
            ch += 32
        elif ch == 92:
            ch = 47
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h


# ------------------------------------------------------------------------------------------------------------------ AES-256
def _gf_mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        if a & 0x100:
            a ^= 0x11B
        b >>= 1
    return r


def _sbox():
    inv = [0] * 256
    for a in range(1, 256):
        for b in range(1, 256):
            if _gf_mul(a, b) == 1:
                inv[a] = b
                break
    box = []
    for a in range(256):
        x = inv[a]
        y = x
        for _ in range(4):
            x = ((x << 1) | (x >> 7)) & 0xFF
            y ^= x
        box.append(y ^ 0x63)
    return box


_SBOX = None


def _tables():
    global _SBOX
    if _SBOX is None:
        sb = _sbox()
        isb = [0] * 256
        for i, v in enumerate(sb):
            isb[v] = i
        _SBOX = (sb, isb, {m: [_gf_mul(x, m) for x in range(256)] for m in (9, 11, 13, 14)})
    return _SBOX


def _expand_key256(key):
    sb = _tables()[0]
    w = [list(key[4 * i:4 * i + 4]) for i in range(8)]
    rcon = 1
    for i in range(8, 60):
        t = list(w[i - 1])
        if i % 8 == 0:
            t = [sb[t[1]], sb[t[2]], sb[t[3]], sb[t[0]]]
            t[0] ^= rcon
            rcon = ((rcon << 1) ^ (0x11B if rcon & 0x80 else 0)) & 0xFF
        elif i % 8 == 4:
            t = [sb[x] for x in t]
        w.append([w[i - 8][j] ^ t[j] for j in range(4)])
    return [sum((w[4 * r + c] for c in range(4)), []) for r in range(15)]


def _aes_decrypt_python(data, key, rounds):
    _, inv, mul = _tables()
    keys = _expand_key256(key)
    shift = [r + 4 * ((c - r) % 4) for c in range(4) for r in range(4)]
    def mix_tables(values):
        return [
            [sum(mul[m][b] << (8 * r) for r, m in enumerate(column)) for b in values]
            for column in ((14, 9, 13, 11), (11, 14, 9, 13), (13, 11, 14, 9), (9, 13, 11, 14))
        ]

    t0, t1, t2, t3 = mix_tables(inv)
    k0, k1, k2, k3 = mix_tables(range(256))
    packed_keys = [
        [k0[a] ^ k1[b] ^ k2[c] ^ k3[d] for a, b, c, d in struct.iter_unpack("4B", bytes(k))]
        for k in keys[13:0:-1]
    ]
    stop = len(data) - len(data) % 16
    result = bytearray()
    for offset in range(0, stop, 16):
        block = data[offset:offset + 16]
        for _ in range(rounds):
            state = bytes(b ^ k for b, k in zip(block, keys[14]))
            for round_key in packed_keys:
                shifted = bytes(state[i] for i in shift)
                columns = [
                    t0[a] ^ t1[b] ^ t2[c] ^ t3[d] ^ k
                    for (a, b, c, d), k in zip(struct.iter_unpack("4B", shifted), round_key)
                ]
                state = struct.pack("<4I", *columns)
            block = bytes(inv[state[i]] ^ k for i, k in zip(shift, keys[0]))
        result.extend(block)
    result.extend(data[stop:])
    return bytes(result)


def aes256_ecb_decrypt_blocks(data, key, rounds=1):
    """AES-256 ECB decrypt `rounds` times over; len(data) is cut to a multiple of 16 (the tail is returned unchanged)."""
    if np is None:
        return _aes_decrypt_python(data, key, rounds)
    n = len(data) // 16
    _, isb, mul = _tables()
    isb = np.array(isb, dtype=np.uint8)
    m9, m11, m13, m14 = (np.array(mul[m], dtype=np.uint8) for m in (9, 11, 13, 14))
    rks = [np.array(r, dtype=np.uint8) for r in _expand_key256(key)]
    inv_shift = np.array([r + 4 * ((c - r) % 4) for c in range(4) for r in range(4)], dtype=np.intp)
    s = np.frombuffer(bytes(data[:n * 16]), dtype=np.uint8).reshape(n, 16)
    for _ in range(rounds):
        s = s ^ rks[14]
        for rnd in range(13, 0, -1):
            s = isb[s[:, inv_shift]] ^ rks[rnd]
            c = s.reshape(n, 4, 4)
            a0, a1, a2, a3 = c[:, :, 0], c[:, :, 1], c[:, :, 2], c[:, :, 3]
            s = np.stack([m14[a0] ^ m11[a1] ^ m13[a2] ^ m9[a3], m9[a0] ^ m14[a1] ^ m11[a2] ^ m13[a3],
                          m13[a0] ^ m9[a1] ^ m14[a2] ^ m11[a3], m11[a0] ^ m13[a1] ^ m9[a2] ^ m14[a3]], axis=2).reshape(n, 16)
        s = isb[s[:, inv_shift]] ^ rks[0]
    return s.tobytes() + bytes(data[n * 16:])


def aes256_ecb_encrypt_block(block, key):
    """One block, for the self-test (FIPS-197 C.3)."""
    sb, _, _ = _tables()
    rks = _expand_key256(key)
    s = [b ^ k for b, k in zip(block, rks[0])]
    shift = [r + 4 * ((c + r) % 4) for c in range(4) for r in range(4)]
    for rnd in range(1, 15):
        s = [sb[s[i]] for i in shift]
        if rnd != 14:
            o = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                o += [_gf_mul(a[0], 2) ^ _gf_mul(a[1], 3) ^ a[2] ^ a[3], a[0] ^ _gf_mul(a[1], 2) ^ _gf_mul(a[2], 3) ^ a[3],
                      a[0] ^ a[1] ^ _gf_mul(a[2], 2) ^ _gf_mul(a[3], 3), _gf_mul(a[0], 3) ^ a[1] ^ a[2] ^ _gf_mul(a[3], 2)]
            s = o
        s = [b ^ k for b, k in zip(s, rks[rnd])]
    return bytes(s)


def find_rpf_key(extra_dirs=()):
    """The 32-byte TOC key, read from the repo's aes256.cpp (kRpfKey). Looks beside this file's repo layout and in `extra_dirs`."""
    rel = Path("src/mc_engine/modloader/aes256.cpp")
    roots = [Path(d).expanduser() for d in extra_dirs]
    if not roots:
        roots = [Path(__file__).resolve().parents[2]]
    cands = [root / rel for root in roots]
    for c in cands:
        if os.path.isfile(c):
            txt = c.read_text(encoding="utf-8")
            m = re.search(r"kRpfKey\[32\]\s*=\s*\{([^}]*)\}", txt)
            if m:
                vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
                if len(vals) == 32:
                    return bytes(vals)
    raise FileNotFoundError("kRpfKey not found: pass --src <LARecomp source tree>")


# ------------------------------------------------------------------------------------------------------------------ RPF3
class Rpf3:
    def __init__(self, path, key):
        self.path = path
        with open(path, "rb") as f:
            hdr = f.read(20)
            magic, toc_size, count = struct.unpack("<4sII", hdr[:12])
            if magic != b"RPF3":
                raise ValueError("not an RPF3 archive: %r" % magic)
            f.seek(0x800)
            toc = f.read(toc_size)
        toc = aes256_ecb_decrypt_blocks(toc, key, 16)
        self.entries = [struct.unpack_from("<4I", toc, 16 * i) for i in range(count)]

    @staticmethod
    def is_dir(e):
        return bool(e[2] & 0x80000000)

    def children(self, e):
        first = e[2] & 0x7FFFFFFF
        return self.entries[first:first + (e[3] & 0x3FFFFFFF)]

    def walk(self, path):
        """The entry (a file or a directory) at `path`, else None."""
        cur = self.entries[0]
        for part in [p for p in re.split(r"[\\/]", path) if p]:
            if not self.is_dir(cur):
                return None
            h = rage_hash(part)
            nxt = None
            for c in self.children(cur):
                if c[0] == h:
                    nxt = c
                    break
            if nxt is None:
                return None
            cur = nxt
        return cur

    def find(self, path):
        cur = self.walk(path)
        return None if cur is None or self.is_dir(cur) else cur

    def find_dir(self, path):
        cur = self.walk(path)
        return cur if cur is not None and self.is_dir(cur) else None

    @staticmethod
    def is_resource(e):
        return bool(e[3] & 0x80000000)

    def read_plain(self, e):
        """The bytes of a plain (non-resource) file: its data starts at (offset_type >> 11) * 2048 + (offset_type & 0x7FF) (modloader/rpf3.h data_offset); bit 30 of flag = raw deflate
        of `flag & 0x3FFFFFFF` stored bytes (entry size = the inflated size)."""
        if self.is_dir(e) or self.is_resource(e):
            raise ValueError("not a plain file")
        off = (e[2] >> 11) * 2048 + (e[2] & 0x7FF)
        with open(self.path, "rb") as f:
            f.seek(off)
            if e[3] & 0x40000000:
                out = zlib.decompress(f.read(e[3] & 0x3FFFFFFF), -15)
            else:
                out = f.read(e[1])
        if len(out) != e[1]:
            raise ValueError("plain file: %d bytes, the table says %d" % (len(out), e[1]))
        return out

    def read_raw(self, e):
        """On-disk bytes of a resource entry (RSC5 header + XCompress payload)."""
        off = e[2] & ~0xFF
        with open(self.path, "rb") as f:
            f.seek(off)
            return f.read(e[1])


# ------------------------------------------------------------------------------------------------------------------ LZX
class _Bits:
    def __init__(self, data):
        self.d = data
        self.pos = 0
        self.buf = 0
        self.left = 0

    def need(self, n):
        while self.left < n:
            if self.pos + 1 < len(self.d):
                w = self.d[self.pos] | (self.d[self.pos + 1] << 8)
            else:
                w = 0
            self.pos += 2
            self.buf = ((self.buf << 16) | w) & ((1 << (self.left + 16)) - 1)
            self.left += 16

    def read(self, n):
        if n == 0:
            return 0
        self.need(n)
        v = (self.buf >> (self.left - n)) & ((1 << n) - 1)
        self.left -= n
        self.buf &= (1 << self.left) - 1
        return v

    def peek16(self):
        self.need(16)
        return (self.buf >> (self.left - 16)) & 0xFFFF

    def skip(self, n):
        self.left -= n
        self.buf &= (1 << self.left) - 1

    def align16(self):
        """Drop the bits up to the next 16-bit word boundary (what the decoder does at the end of a frame)."""
        self.skip(self.left & 15)


def _build_table(lens):
    """canonical (MSB first) code table of 2**16 entries: sym << 5 | len, len 0 = invalid."""
    tab = np.zeros(1 << 16, dtype=np.int32)
    code = 0
    for bits in range(1, 17):
        for sym, l in enumerate(lens):
            if l == bits:
                lo = code << (16 - bits)
                hi = (code + 1) << (16 - bits)
                if hi > (1 << 16):
                    raise ValueError("LZX: over-subscribed code")
                tab[lo:hi] = (sym << 5) | bits
                code += 1
        code <<= 1
    return tab.tolist()


def _sym(br, tab):
    e = tab[br.peek16()]
    l = e & 31
    if l == 0:
        raise ValueError("LZX: bad code")
    br.skip(l)
    return e >> 5


def lzx_decode_bitstream(src, out_len, window_bits=17):
    """Decodes one LZX bitstream (chunk headers already removed) to exactly out_len bytes."""
    slots = {20: 42, 21: 50}.get(window_bits, window_bits << 1)
    extra = []
    j = 0
    for i in range(0, 38, 2):
        extra += [j, j]
        if i != 0 and j < 17:
            j += 1
    pos_base = []
    j = 0
    for i in range(51):
        pos_base.append(j)
        j += 1 << extra[i] if i < len(extra) else 1 << 17
    nmain = 256 + slots * 8
    main_lens = [0] * nmain
    len_lens = [0] * 249
    br = _Bits(src)
    out = bytearray(out_len)
    op = 0
    R0 = R1 = R2 = 1
    if br.read(1):
        br.read(16)
        br.read(16)
    block_type = 0
    block_remaining = 0
    main_tab = len_tab = al_tab = None
    frame_end = 0
    FRAME = 0x8000

    def read_lens(lens, first, last):
        pre = [br.read(4) for _ in range(20)]
        ptab = _build_table(pre)
        x = first
        while x < last:
            z = _sym(br, ptab)
            if z == 17:
                y = br.read(4) + 4
                for _ in range(y):
                    lens[x] = 0
                    x += 1
            elif z == 18:
                y = br.read(5) + 20
                for _ in range(y):
                    lens[x] = 0
                    x += 1
            elif z == 19:
                y = br.read(1) + 4
                z = _sym(br, ptab)
                z = lens[x] - z
                if z < 0:
                    z += 17
                for _ in range(y):
                    lens[x] = z
                    x += 1
            else:
                z = lens[x] - z
                if z < 0:
                    z += 17
                lens[x] = z
                x += 1

    while op < out_len:
        frame_end = min(op + FRAME, out_len) if frame_end <= op else frame_end
        while op < frame_end:
            if block_remaining == 0:
                if block_type == 3 and (block_len & 1):
                    br.pos += 1  # the pad byte after an odd-length uncompressed block
                block_type = br.read(3)
                hi = br.read(16)
                lo = br.read(8)
                block_len = block_remaining = (hi << 8) | lo
                if block_type in (1, 2):
                    if block_type == 2:
                        al_lens = [br.read(3) for _ in range(8)]
                        al_tab = _build_table(al_lens)
                    read_lens(main_lens, 0, 256)
                    read_lens(main_lens, 256, nmain)
                    main_tab = _build_table(main_lens)
                    read_lens(len_lens, 0, 249)
                    len_tab = _build_table(len_lens)
                elif block_type == 3:
                    # libmspack: "read 1-16 (not 0-15) bits to align": a word is read when none is buffered, then all buffered bits go
                    if br.left == 0:
                        br.need(16)
                    br.left = 0
                    br.buf = 0
                    R0, R1, R2 = struct.unpack_from("<3I", src, br.pos)
                    br.pos += 12
                else:
                    raise ValueError("LZX: bad block type %d" % block_type)
            run = min(block_remaining, frame_end - op)
            if block_type == 3:
                out[op:op + run] = src[br.pos:br.pos + run]
                br.pos += run
                op += run
                block_remaining -= run
                continue
            done = 0
            while done < run:
                m = _sym(br, main_tab)
                if m < 256:
                    out[op] = m
                    op += 1
                    done += 1
                    continue
                m -= 256
                mlen = m & 7
                if mlen == 7:
                    mlen += _sym(br, len_tab)
                mlen += 2
                slot = m >> 3
                if slot == 0:
                    off = R0
                elif slot == 1:
                    off = R1
                    R1 = R0
                    R0 = off
                elif slot == 2:
                    off = R2
                    R2 = R0
                    R0 = off
                else:
                    eb = extra[slot] if slot < len(extra) else 17
                    if block_type == 2 and eb >= 3:
                        v = br.read(eb - 3) << 3
                        v += _sym(br, al_tab)
                    else:
                        v = br.read(eb)
                    off = pos_base[slot] - 2 + v
                    R2 = R1
                    R1 = R0
                    R0 = off
                if off > op or op + mlen > out_len:
                    raise ValueError("LZX: bad match (offset %d at %d, length %d)" % (off, op, mlen))
                if off >= mlen:
                    out[op:op + mlen] = out[op - off:op - off + mlen]
                else:
                    for k in range(mlen):
                        out[op + k] = out[op + k - off]
                op += mlen
                done += mlen
            block_remaining -= done
            if block_remaining < 0:
                raise ValueError("LZX: a match ran past the end of its block")
        if op != frame_end:
            raise ValueError("LZX: a match crossed a frame boundary")
        br.align16()
    return bytes(out)


def strip_chunk_headers(src):
    """XCompress chunks -> one LZX bitstream: [0xFF, out(2), len(2)] or [len(2)] big endian, then len bytes."""
    out = bytearray()
    off = 0
    while off < len(src):
        if src[off] == 0xFF:
            if off + 5 > len(src):
                raise ValueError("XCompress: cut chunk header")
            length = (src[off + 3] << 8) | src[off + 4]
            off += 5
        else:
            if off + 2 > len(src):
                raise ValueError("XCompress: cut chunk header")
            length = (src[off] << 8) | src[off + 1]
            off += 2
        if length == 0:
            break
        if off + length > len(src):
            raise ValueError("XCompress: cut chunk")
        out += src[off:off + length]
        off += length
    return bytes(out)


def read_resource(rpf, path):
    """The decompressed [virtual][physical] block of a resource, plus (type, flag, virtual_size, physical_size)."""
    e = rpf.find(path)
    if e is None:
        raise FileNotFoundError(path)
    raw = rpf.read_raw(e)
    magic, typ, flag, comp, plen = struct.unpack(">IIIII", raw[:20])
    if magic != 0x05435352 or comp != 0x0FF512EF:
        raise ValueError("%s: not an XCompress RSC5 resource" % path)
    vsize = (flag & 0x7FF) << (((flag >> 11) & 0xF) + 8)
    psize = ((flag >> 15) & 0x7FF) << (((flag >> 26) & 0xF) + 8)
    stream = strip_chunk_headers(raw[20:20 + plen])
    return lzx_decode_bitstream(stream, vsize + psize), (typ, flag, vsize, psize)


# ------------------------------------------------------------------------------------------------------------------ resources
VBASE = 0x50000000  # the virtual segment of a resource: pointers 0x5xxxxxxx index it


class Block:
    """Big-endian reads from a decompressed resource (pointer base 0x50000000) or, with base=0, from any flat buffer."""

    def __init__(self, data, base=VBASE):
        self.d = data
        self.base = base

    def u32(self, p):
        o = p - self.base
        return struct.unpack_from(">I", self.d, o)[0]

    def u16(self, p):
        return struct.unpack_from(">H", self.d, p - self.base)[0]

    def f32(self, p):
        return struct.unpack_from(">f", self.d, p - self.base)[0]

    def cstr(self, p, limit=96):
        o = p - self.base
        e = self.d.find(b"\0", o, o + limit)
        return self.d[o:e if e >= 0 else o + limit].decode("latin1")


def parse_lights(data):
    """lights.xlm (decompressed): the list at +0x50 {ptr, u16 count, u16 capacity}, 96-byte lights. -> list of dicts."""
    b = Block(data)
    ptr = b.u32(VBASE + 0x50)
    count = b.u16(VBASE + 0x54)
    out = []
    for i in range(count):
        o = ptr + 96 * i
        typ = data[o - VBASE + 88]
        on = data[o - VBASE + 89]
        out.append({
            "i": i,
            "pos": [round(b.f32(o + k), 3) for k in (0, 4, 8)],
            "color": [round(b.f32(o + 16 + k), 4) for k in (0, 4, 8)],
            "dir": [round(b.f32(o + 32 + k), 4) for k in (0, 4, 8)],
            "intensity": round(b.f32(o + 64), 3),
            "cone": round(b.f32(o + 68), 2),
            "spread": round(b.f32(o + 72), 2),
            "type": "spot" if typ == 1 else "omni",
            "on": bool(on),
        })
    return out


# ------------------------------------------------------------------------------------------------------------------ the street lamps
# There is NO file of street lamps (DIRECTOR-LAMPS, 2026-11-22; the old note "not found in the RPF" was right about the file and wrong to look for one). The game makes them at the
# city's load from the PROPS: sub_82733A38 (the constructor of the props resource, `$/resources/prop/<level>_props.#ck`, RSC type 131) walks its prop objects in order and calls
# sub_82735898 on each; a prop with lamp points (+156) whose model has a light template (sub_82735BE0 reads `<tune/lighting>/<model basename>.xml` into the model's +348) gets ONE lamp
# from sub_822F7AC0, appended to the light set's lamp list (sub_822FD568), which the loader packs into the manager's +8 object's +24 list (the 6,298 the live atlas.dump reads, in this order).
#   props resource (virtual segment, pointers 0x5xxxxxxx; big endian): root +0x0C = array of prop pointers, +0x10 = u16 count (22,596) and capacity.
#   prop (160 bytes): +16 world matrix, 4 rows of 4 floats (row vectors: the 3x3 rotation, row 3 = the position), +92 the model object, +156 lamp points {ptr, u16 count at +4}, each a
#     16-byte local position; only the FIRST point makes a lamp (7,364 points, 6,298 lamps).
#   model: +176 its name (the XML is `<name>.xml`; a '/' in it cuts the path off).
#   <Light> XML: Type (ignored: the type follows from the cone, > 359.9999 degrees = omni, else spot), Position (added to the lamp point), Colour, Intensity, ConeAngle, Penumbra (= the
#     light's spread), Direction (Euler degrees, see euler_direction), DropOff (unused here).
#   lamp: position = (point + template position, 1) x matrix; direction = the template's direction turned by the 3x3 and normalised; colour, intensity, cone as the template.
LAMP_DEG = 0.017455555498600006  # the game's degrees-to-radians constant (0x820410C4; pi/180 = 0.0174533)
OMNI_CONE = 359.9999084472656  # sub_822FD4E0: a cone angle above this is an omni light (0x82040BB8)
PROPS_PATH = "resources/prop/%s_props.xck"
LIGHTING_DIR = "tune/lighting"
PROP_STRIDE = 160


def euler_direction(x, y, z):
    """The direction a <Light> XML's Direction (Euler degrees) gives (sub_822FCCC0: the rotation built from the angles turns the vector (0, 0, -1)). Validated for (90, 0, 171.469223), the
    one Direction every street lamp template has: Rx first, then Ry, then Rz. CHECK: the order for a non-zero y (no template has one)."""
    ax, ay, az = (float(v) * LAMP_DEG for v in (x, y, z))
    v = (0.0, 0.0, -1.0)
    v = (v[0], v[1] * math.cos(ax) - v[2] * math.sin(ax), v[1] * math.sin(ax) + v[2] * math.cos(ax))
    v = (v[0] * math.cos(ay) + v[2] * math.sin(ay), v[1], -v[0] * math.sin(ay) + v[2] * math.cos(ay))
    return (v[0] * math.cos(az) - v[1] * math.sin(az), v[0] * math.sin(az) + v[1] * math.cos(az), v[2])


def parse_light_xml(text):
    """One <Light> template (tune/lighting/<model>.xml) -> {type, pos, color, intensity, cone, spread, dir (the Euler angles)}. The game's parser reads the attributes of Type, Position,
    Colour, Intensity, ConeAngle, Penumbra, Direction (the members of its parser table 0x827E2AD0); a missing tag is a ValueError."""
    if isinstance(text, bytes):
        text = text.decode("latin1")

    def tag(name, keys):
        m = re.search(r"<%s\b([^>]*)>" % name, text)
        if not m:
            raise ValueError("light xml: no <%s>" % name)
        out = []
        for k in keys:
            a = re.search(r'\b%s\s*=\s*"([^"]*)"' % k, m.group(1))
            if not a:
                raise ValueError("light xml: <%s> has no %s" % (name, k))
            out.append(float(a.group(1)))
        return out

    return {"type": int(tag("Type", ["value"])[0]), "pos": tag("Position", "xyz"), "color": tag("Colour", "xyz"), "intensity": tag("Intensity", ["value"])[0],
            "cone": tag("ConeAngle", ["value"])[0], "spread": tag("Penumbra", ["value"])[0], "dir": tag("Direction", "xyz")}


def parse_lamp_props(data):
    """The props resource (decompressed) -> (props with a lamp point, number of props, number skipped as unreadable). A prop = {k (index), name (the model's), pt (the first lamp point,
    local), mat (4 rows of 4 floats)}. Pointers outside the virtual segment make a prop unreadable (counted, not raised)."""
    n_total = struct.unpack_from(">H", data, 0x10)[0]
    arr = struct.unpack_from(">I", data, 0x0C)[0] - VBASE
    out, bad = [], 0

    def ok_ptr(p, size=4):
        return VBASE <= p and p - VBASE + size <= len(data)

    for k in range(n_total):
        try:
            p = struct.unpack_from(">I", data, arr + 4 * k)[0]
            if not ok_ptr(p, PROP_STRIDE):
                raise ValueError
            o = p - VBASE
            lp = struct.unpack_from(">I", data, o + 156)[0]
            if not lp:
                continue
            model = struct.unpack_from(">I", data, o + 92)[0]
            if not ok_ptr(lp, 6) or not ok_ptr(model, 180):
                raise ValueError
            count = struct.unpack_from(">H", data, lp - VBASE + 4)[0]
            pts = struct.unpack_from(">I", data, lp - VBASE)[0]
            name_p = struct.unpack_from(">I", data, model - VBASE + 176)[0]
            if not count:
                continue
            if not ok_ptr(pts, 16) or not ok_ptr(name_p, 2):
                raise ValueError
            e = data.find(b"\0", name_p - VBASE)
            if e < 0:
                raise ValueError
            out.append({"k": k, "name": data[name_p - VBASE:e].decode("latin1"), "pt": list(struct.unpack_from(">3f", data, pts - VBASE)),
                        "mat": [list(struct.unpack_from(">4f", data, o + 16 + 16 * r)) for r in range(4)]})
        except (ValueError, struct.error):
            bad += 1
    return out, n_total, bad


def model_xml_name(name):
    """The template file name of a model: the part after the last '/' (sub_82735BE0), plus .xml."""
    return name.rsplit("/", 1)[-1] + ".xml"


def build_lamps(props, templates, first_index=0):
    """Lamp lights (the lights.json shape, src "lamp", plus "model") from lamp props and their parsed templates ({model name (lower case) -> parse_light_xml}). A prop whose model has no
    template makes no lamp (the game: no light template, no lamp); those names are returned. -> (lamps, [missing model names]). Positions are the unclamped world positions: the running
    game keeps them as 16-bit steps inside a box (x -3467..2853, y -31..79, z -2358..2105.75) and clamps what lies beyond (atlas.lights_equal knows)."""
    lamps, missing = [], []
    for p in props:
        t = templates.get(p["name"].rsplit("/", 1)[-1].lower())
        if t is None:
            if p["name"] not in missing:
                missing.append(p["name"])
            continue
        loc = [p["pt"][i] + t["pos"][i] for i in range(3)]
        m = p["mat"]
        pos = [loc[0] * m[0][j] + loc[1] * m[1][j] + loc[2] * m[2][j] + m[3][j] for j in range(3)]
        d0 = euler_direction(*t["dir"])
        d = [d0[0] * m[0][j] + d0[1] * m[1][j] + d0[2] * m[2][j] for j in range(3)]
        n = math.sqrt(sum(c * c for c in d)) or 1.0
        lamps.append({"i": first_index + len(lamps), "pos": [round(c, 3) for c in pos], "color": [round(c, 4) for c in t["color"]], "dir": [round(c / n, 4) for c in d],
                      "intensity": round(t["intensity"], 3), "cone": round(t["cone"], 2), "spread": round(t["spread"], 2), "type": "omni" if t["cone"] > OMNI_CONE else "spot",
                      "on": True, "src": "lamp", "model": p["name"]})
    return lamps, missing


def read_lamps(rpf, level="sc", first_index=0, log=print):
    """The street lamps of a city from the game's files: its props resource and the <Light> templates of the models that have lamp points. -> (lamps, info {props, lamp_props,
    unreadable, missing, templates})."""
    data, _info = read_resource(rpf, PROPS_PATH % level)
    props, n_total, bad = parse_lamp_props(data)
    folder = rpf.find_dir(LIGHTING_DIR)
    if folder is None:
        raise FileNotFoundError(LIGHTING_DIR)
    by_hash = {c[0]: c for c in rpf.children(folder) if not rpf.is_dir(c)}
    templates = {}
    for name in sorted({p["name"].rsplit("/", 1)[-1] for p in props}):
        e = by_hash.get(rage_hash(model_xml_name(name)))
        if e is not None:
            templates[name.lower()] = parse_light_xml(rpf.read_plain(e))
    lamps, missing = build_lamps(props, templates, first_index)
    log("street lamps: %d lamps from %d props (of %d), %d models with a template%s" % (len(lamps), len(props), n_total, len(templates),
                                                                                       "; %d unreadable props" % bad if bad else ""))
    if missing:
        log("  models without a template (no lamp): %s" % ", ".join(missing))
    return lamps, {"props": n_total, "lamp_props": len(props), "unreadable": bad, "missing": missing, "templates": len(templates)}


NODE_KINDS = ["rails", "road", "intersection", "trashgroup", "left_turns", "right_turns", "u_turns", "traffic", "gps", "gps_street",
              "trash", "dynamic_rail", "dynamic_rail_tcp", "branching_left_turn_lane"]
RAIL_STRIDE = 112


def _bspline(cvs, knots, degree, t):
    """de Boor at parameter t (non-rational: the CV w of the file is a NaN marker)."""
    n = len(cvs) - 1
    # span
    if t >= knots[n + 1]:
        k = n
    else:
        k = degree
        while k < n and knots[k + 1] <= t:
            k += 1
    d = [list(cvs[j + k - degree]) for j in range(degree + 1)]
    for r in range(1, degree + 1):
        for j in range(degree, r - 1, -1):
            den = knots[j + 1 + k - r] - knots[j + k - degree]
            a = 0.0 if den == 0 else (t - knots[j + k - degree]) / den
            d[j] = [(1 - a) * d[j - 1][c] + a * d[j][c] for c in range(3)]
    return d[degree]


def parse_railnet(data, step_m=4.0, max_pts=40):
    """traffic.xrn (decompressed) -> {'nodes': [...], 'rails': [...]}.

    rails[i]: id, node (index into nodes), kind, flags, len, pts (sampled polyline x,y,z along the travel direction), next, prev
    nodes[i]: id, kind (road|intersection|left_turn|right_turn|traffic), name (roads), speed (m/s), cls, rails [ids], dir (+1/-1 per rail is
    on the rail: its direction against the road's own direction vector)."""
    b = Block(data)
    R = VBASE
    cv_ptr, kv_ptr, cvi_ptr, joint_ptr = b.u32(R + 0x2C), b.u32(R + 0x30), b.u32(R + 0x34), b.u32(R + 0x38)
    n_cv = b.u32(R + 0x40)
    arrays = {}
    for k in range(14):
        ptr = b.u32(R + 80 + 4 * k)
        if ptr:
            arrays[k] = (ptr, b.u32(R + 136 + 4 * k), b.u32(R + 0xF8 + 4 * k))
    rails_ptr, _, n_rails = arrays[0]
    knots = {}

    def knot_vec(i):
        if i not in knots:
            o = kv_ptr + 12 * i
            kp, kn = b.u32(o + 4), b.u32(o + 8)
            knots[i] = [b.f32(kp + 4 * j) for j in range(kn)]
        return knots[i]

    # nodes in connection-group order: roads, intersections, left turns, right turns, u turns, traffic
    order = [(1, "road"), (2, "intersection"), (4, "left_turn"), (5, "right_turn"), (6, "u_turn"), (7, "traffic")]
    nodes = []
    node_of = {}  # node address -> node index
    for k, kind in order:
        if k not in arrays:
            continue
        ptr, stride, cnt = arrays[k]
        for i in range(cnt):
            a = ptr + stride * i
            nd = {"id": len(nodes), "kind": kind, "rails": []}
            if kind == "road":
                nd["name"] = b.cstr(b.u32(a + 144)) if b.u32(a + 144) else ""
                nd["speed"] = round(b.f32(a + 148), 3)
                nd["cls"] = b.u32(a + 152)
                nd["_dir"] = (b.f32(a + 96), b.f32(a + 104))
            node_of[a] = nd["id"]
            nodes.append(nd)
    rails = []
    for i in range(n_rails):
        o = rails_ptr + RAIL_STRIDE * i
        ncv1 = b.u16(o + 76)
        deg = b.u16(o + 78)
        kvi = b.u16(o + 84)
        idxp = b.u32(o + 80)
        idx = [b.u16(idxp + 2 * j) for j in range(ncv1 + 1)]
        cvs = []
        for j in idx:
            a = cv_ptr + 16 * j
            cvs.append((b.f32(a), b.f32(a + 4), b.f32(a + 8)))
        length = b.f32(o + 12)
        npts = max(2, min(max_pts, int(length / step_m) + 2))
        kn = knot_vec(kvi)
        t0, t1 = kn[deg], kn[len(cvs)]
        pts = []
        for s in range(npts):
            t = t0 + (t1 - t0) * s / (npts - 1)
            p = _bspline(cvs, kn, deg, t) if len(cvs) > deg else cvs[min(s, len(cvs) - 1)]
            pts.append([round(p[0], 2), round(p[1], 2), round(p[2], 2)])
        owner = node_of.get(b.u32(o + 8), -1)
        rails.append({"id": i, "node": owner, "kind": nodes[owner]["kind"] if owner >= 0 else "?", "flags": b.u32(o + 4), "len": round(length, 2),
                      "deg": deg, "cv": idx, "pts": pts, "next": [], "prev": []})
        if owner >= 0:
            nodes[owner]["rails"].append(i)
    # connectivity from the joints (one per control vertex): rails that END there feed the rails that START there
    starts = {}
    ends = {}
    for r in rails:
        starts.setdefault(r["cv"][0], []).append(r["id"])
        ends.setdefault(r["cv"][-1], []).append(r["id"])
    for cvi, ins in ends.items():
        outs = starts.get(cvi, [])
        for a in ins:
            for c in outs:
                if c != a:
                    rails[a]["next"].append(c)
                    rails[c]["prev"].append(a)
    # side: a road rail's direction against its road's reference direction
    for r in rails:
        nd = nodes[r["node"]] if r["node"] >= 0 else None
        if nd and nd["kind"] == "road" and len(r["pts"]) >= 2:
            dx = r["pts"][-1][0] - r["pts"][0][0]
            dz = r["pts"][-1][2] - r["pts"][0][2]
            rd = nd["_dir"]
            r["dir"] = 1 if dx * rd[0] + dz * rd[1] >= 0 else -1
    for nd in nodes:
        nd.pop("_dir", None)
    return {"nodes": nodes, "rails": rails, "cv_count": n_cv}


# ------------------------------------------------------------------------------------------------------------------ build
def build_from_game(game_dir, key, log=print, lamps=True):
    """-> (lights list, railnet dict) read from <game_dir>/xarchive_cache.rpf. The lights are lights.xlm's, then (lamps=True) the street lamps decoded from the props (src "lamp",
    the order of a live atlas.dump: the file's lights first, the lamps after). A city whose props or templates cannot be read keeps the file's lights and says so."""
    rpf = Rpf3(os.path.join(game_dir, RPF_NAME), key)
    log("toc: %d entries" % len(rpf.entries))
    ld, info = read_resource(rpf, LIGHTS_PATH)
    log("lights.xlm: %d bytes (type %d)" % (len(ld), info[0]))
    rd, info = read_resource(rpf, TRAFFIC_PATH)
    log("traffic.xrn: %d bytes (type %d)" % (len(rd), info[0]))
    lights = parse_lights(ld)
    if lamps:
        try:
            lamp_list, _ = read_lamps(rpf, "sc", len(lights), log)
            lights += lamp_list
        except (FileNotFoundError, ValueError, struct.error, zlib.error) as ex:
            log("street lamps: NOT decoded (%s: %s); the atlas has the file's lights only" % (type(ex).__name__, ex))
    return lights, parse_railnet(rd)


# ------------------------------------------------------------------------------------------------------------------ selftest
def _selftest():
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + name)
        ok = ok and cond

    check("rage_hash('traffic.xrn') = 0x9960fcd8", rage_hash("traffic.xrn") == 0x9960FCD8)
    check("rage_hash folds case and backslash", rage_hash("Resources\\City") == rage_hash("resources/city"))
    # FIPS-197 C.3 (AES-256): key 00..1f, plaintext 00112233445566778899aabbccddeeff -> 8ea2b7ca516745bfeafc49904b496089
    key = bytes(range(32))
    ct = aes256_ecb_encrypt_block(bytes.fromhex("00112233445566778899aabbccddeeff"), key)
    check("AES-256 FIPS-197 C.3 encrypt", ct.hex() == "8ea2b7ca516745bfeafc49904b496089")
    plain = bytes.fromhex("00112233445566778899aabbccddeeff")
    check("AES-256 Python decrypt (3 blocks and a tail)", _aes_decrypt_python(ct * 3 + b"tail", key, 1) == plain * 3 + b"tail")
    twice = aes256_ecb_encrypt_block(ct, key)
    check("AES-256 Python decrypt (two passes)", _aes_decrypt_python(twice, key, 2) == plain)
    check("AES-256 archive decoder uses the available backend", aes256_ecb_decrypt_blocks(twice + b"x", key, 2) == plain + b"x")
    if np is not None:
        pt = aes256_ecb_decrypt_blocks(ct * 3, key, 1)
        check("AES-256 numpy decrypt (3 blocks)", pt == bytes.fromhex("00112233445566778899aabbccddeeff") * 3)
    # a stored (uncompressed) LZX block: type 3, then R0..R2, then the raw bytes: 24 bits of size
    payload = b"hello lzx stored block"  # 22 bytes (even: no pad)
    # bits: header bit 0 (no intel), block type 3 (3 bits), size 24 bits = 22 -> then align to 16 bits
    bits = "0" + "011" + format(22, "024b")
    bits += "0" * ((16 - len(bits) % 16) % 16)
    words = b"".join(struct.pack("<H", int(bits[i:i + 16], 2)) for i in range(0, len(bits), 16))
    stream = words + struct.pack("<3I", 1, 1, 1) + payload
    try:
        check("LZX stored block", lzx_decode_bitstream(stream, len(payload)) == payload)
    except Exception as ex:  # noqa: BLE001
        check("LZX stored block (%s)" % ex, False)
    # chunk header stripping
    check("strip_chunk_headers", strip_chunk_headers(b"\x00\x03abc\xff\x00\x10\x00\x02xy\x00\x00") == b"abcxy")
    # B-spline: a cubic Bezier (knots 0,0,0,0,1,1,1,1) at t=0.5 of a straight line stays on the line
    cvs = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0)]
    p = _bspline(cvs, [0, 0, 0, 0, 1, 1, 1, 1], 3, 0.5)
    check("de Boor midpoint of a straight Bezier", abs(p[0] - 1.5) < 1e-9 and p[1] == 0)
    p = _bspline(cvs, [0, 0, 0, 0, 1, 1, 1, 1], 3, 1.0)
    check("de Boor end point", abs(p[0] - 3.0) < 1e-9)
    # a synthetic railnet resource: two chained rails
    data = synth_railnet()
    rn = parse_railnet(data)
    check("synthetic railnet: 2 rails, 1 road", len(rn["rails"]) == 2 and rn["nodes"][0]["name"] == "SYN_Test_St." and rn["nodes"][0]["speed"] == 20.0)
    check("synthetic railnet: rail 0 -> rail 1", rn["rails"][0]["next"] == [1] and rn["rails"][1]["prev"] == [0])
    check("synthetic railnet: polyline ends", rn["rails"][0]["pts"][0] == [0.0, 0.0, 0.0] and rn["rails"][0]["pts"][-1] == [30.0, 0.0, 0.0])
    lt = parse_lights(synth_lights())
    check("synthetic lights", len(lt) == 2 and lt[1]["type"] == "spot" and lt[0]["pos"] == [1.0, 2.0, 3.0] and lt[0]["on"])
    # the street lamps: a <Light> template, the Euler direction, a synthetic props resource (3 props: a lamp, a prop without lamp points, a lamp turned 90 degrees)
    t = parse_light_xml(SYNTH_LAMP_XML)
    check("light xml: type, colour (as written, '.5' too), intensity, cone, penumbra, direction angles, position",
          t["type"] == 1 and t["color"] == [0.75, 0.8, 0.5] and t["intensity"] == 55.0 and t["cone"] == 125.0 and t["spread"] == -50.0 and t["dir"] == [90.0, 0.0, 171.469223]
          and t["pos"] == [0.0, 0.0, 0.0])
    try:
        parse_light_xml(b'<Light><Type value ="1"/></Light>')
        check("light xml: a missing tag is an error", False)
    except ValueError:
        check("light xml: a missing tag is an error", True)
    d0 = euler_direction(90.0, 0.0, 171.469223)
    check("euler_direction(90, 0, 171.469223) = (-0.1480, -0.9890, 0): a lamp points down, tilted 8.5 degrees (the game's degree constant)", abs(d0[0] + 0.1480) < 2e-4 and abs(d0[1] + 0.9890) < 2e-4 and abs(d0[2]) < 5e-4)
    check("euler_direction(90, 0, 180) = straight down", abs(euler_direction(90.0, 0.0, 180.0)[1] + 1.0) < 1e-3)
    props, n_total, bad = parse_lamp_props(synth_props())
    check("props: 3 in the resource, 2 with a lamp point, none unreadable", n_total == 3 and len(props) == 2 and bad == 0 and [p["k"] for p in props] == [0, 2])
    check("props: model name, first lamp point, matrix", props[0]["name"] == "synth/lamp_a" and props[0]["pt"] == [0.0, 4.0, 1.0] and props[0]["mat"][3][:3] == [10.0, 20.0, 30.0])
    lamps, missing = build_lamps(props, {"lamp_a": t}, 100)
    check("lamps: two, indices continue the file's lights, src lamp, on, spot (the cone is 125)", [l["i"] for l in lamps] == [100, 101] and all(l["src"] == "lamp" and l["on"] and l["type"] == "spot" for l in lamps)
          and not missing)
    check("lamps: position = (lamp point + template) x matrix (10, 24, 31) and, turned 90 degrees, (99, 4, 50)", lamps[0]["pos"] == [10.0, 24.0, 31.0] and lamps[1]["pos"] == [99.0, 4.0, 50.0])
    check("lamps: colour, intensity, cone, spread as the template", lamps[0]["color"] == [0.75, 0.8, 0.5] and lamps[0]["intensity"] == 55.0 and lamps[0]["cone"] == 125.0 and lamps[0]["spread"] == -50.0)
    check("lamps: direction = the template's, turned with the prop (down and 8.5 degrees one way; turned 90 degrees, the tilt goes along z)",
          abs(lamps[0]["dir"][0] + 0.1480) < 5e-4 and abs(lamps[0]["dir"][1] + 0.9890) < 5e-4 and abs(lamps[1]["dir"][0]) < 5e-4 and abs(lamps[1]["dir"][2] + 0.1480) < 5e-4)
    check("lamps: a model without a template makes no lamp and is named", build_lamps(props, {}, 0) == ([], ["synth/lamp_a"]))
    omni = dict(t, cone=360.0)
    check("lamps: a cone above 359.9999 degrees is an omni light, whatever the XML's Type says", build_lamps(props[:1], {"lamp_a": omni})[0][0]["type"] == "omni")
    check("model_xml_name cuts the path: 'a/b/lamp_x' -> 'lamp_x.xml'", model_xml_name("a/b/lamp_x") == "lamp_x.xml" and model_xml_name("lamp_x") == "lamp_x.xml")
    bad_props = bytearray(synth_props())
    struct.pack_into(">I", bad_props, 0x40, VBASE + 0x7FFFF0)  # the first prop's pointer leaves the segment
    check("props: a pointer outside the resource makes one prop unreadable, not an exception", parse_lamp_props(bytes(bad_props))[2] == 1)
    print("selftest: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


SYNTH_LAMP_XML = """<?xml version="1.0" encoding="UTF-8"?>
<Light>
    <Type value ="1" />
    <Position x="0.0" y="0.0" z="0.0"/>
    <Colour x="0.75" y="0.8" z=".5"/>
    <Intensity value ="55"/>
    <ConeAngle value ="125.0" />
    <Penumbra value ="-50"/>
    <Direction x="90.0" y="0.0" z="171.469223"/>
    <DropOff value ="-50.0" />
</Light>
"""


def synth_props():
    """A tiny props resource with the real layout (see the street-lamp notes above): root, an array of 3 prop pointers, 3 props of 160 bytes (a lamp, one without lamp points, a lamp turned
    90 degrees about y), a lamp-point list, a model with its name. For tests only."""
    buf = bytearray(0x800)
    P = VBASE
    struct.pack_into(">I", buf, 0x0C, P + 0x40)
    struct.pack_into(">HH", buf, 0x10, 3, 3)
    for k, off in enumerate((0x100, 0x1A0, 0x240)):
        struct.pack_into(">I", buf, 0x40 + 4 * k, P + off)
    nan = 0x7F800001
    rows = {0: [(1, 0, 0), (0, 1, 0), (0, 0, 1), (10.0, 20.0, 30.0)], 2: [(0, 0, 1), (0, 1, 0), (-1, 0, 0), (100.0, 0.0, 50.0)]}
    for k, off in ((0, 0x100), (2, 0x240)):
        for r, (x, y, z) in enumerate(rows[k]):
            struct.pack_into(">3f", buf, off + 16 + 16 * r, x, y, z)
            struct.pack_into(">I", buf, off + 16 + 16 * r + 12, nan)
        struct.pack_into(">I", buf, off + 92, P + 0x400)
        struct.pack_into(">I", buf, off + 156, P + 0x3C0)
    struct.pack_into(">I", buf, 0x1A0 + 92, P + 0x400)  # prop 1: a model but no lamp points (+156 = 0)
    struct.pack_into(">I", buf, 0x3C0, P + 0x3E0)
    struct.pack_into(">HH", buf, 0x3C4, 1, 1)
    struct.pack_into(">4f", buf, 0x3E0, 0.0, 4.0, 1.0, 0.0)
    struct.pack_into(">I", buf, 0x400 + 176, P + 0x600)
    buf[0x600:0x600 + 13] = b"synth/lamp_a\0"
    return bytes(buf)


def synth_railnet():
    """A tiny railnet resource with the real layout: root at 0, rails, one road, CVs, knot vectors, CV indices, joints. For tests only."""
    buf = bytearray(0x1000)

    def w32(off, v):
        struct.pack_into(">I", buf, off, v & 0xFFFFFFFF)

    def wf(off, v):
        struct.pack_into(">f", buf, off, v)

    def w16(off, v):
        struct.pack_into(">H", buf, off, v)

    P = VBASE
    # layout: root 0..0x140, rails 0x150 (2 x 112), road 0x240 (176), CVs 0x300 (7 x 16), knots 0x380 (1 x 12 + 8 floats at 0x3A0),
    # CV indices 0x3E0, joints 0x400 (7 x 8) + lists 0x440, name 0x4A0
    w32(0x2C, P + 0x300)
    w32(0x30, P + 0x380)
    w32(0x34, P + 0x3E0)
    w32(0x38, P + 0x400)
    w32(0x40, 7)
    w32(0x44, 1)
    w32(80 + 4 * 0, P + 0x150)
    w32(136 + 4 * 0, 112)
    w32(0xF8 + 4 * 0, 2)
    w32(80 + 4 * 1, P + 0x240)
    w32(136 + 4 * 1, 176)
    w32(0xF8 + 4 * 1, 1)
    # road
    w32(0x240 + 144, P + 0x4A0)
    wf(0x240 + 148, 20.0)
    w32(0x240 + 152, 3)
    wf(0x240 + 96, 1.0)
    wf(0x240 + 104, 0.0)
    buf[0x4A0:0x4A0 + 13] = b"SYN_Test_St.\0"
    # CVs: rail 0 = (0..30, 0, 0) with 4 CVs 0,10,20,30 ; rail 1 = 30..60 with CVs 3,4,5,6 (CV 3 is shared with rail 0's last)
    xs = [0, 10, 20, 30, 40, 50, 60]
    for i, x in enumerate(xs):
        wf(0x300 + 16 * i + 0, float(x))
        wf(0x300 + 16 * i + 4, 0.0)
        wf(0x300 + 16 * i + 8, 0.0)
        w32(0x300 + 16 * i + 12, 0x7F800001)
    # knot vector 0 = bezier
    w32(0x380 + 4, P + 0x3A0)
    w32(0x380 + 8, 8)
    for j, k in enumerate([0, 0, 0, 0, 1, 1, 1, 1]):
        wf(0x3A0 + 4 * j, float(k))
    # CV indices
    for j, c in enumerate([0, 1, 2, 3, 3, 4, 5, 6]):
        w16(0x3E0 + 2 * j, c)
    for r in range(2):
        o = 0x150 + 112 * r
        w32(o + 4, 1)
        w32(o + 8, P + 0x240)
        wf(o + 12, 30.0)
        w16(o + 76, 3)
        w16(o + 78, 3)
        w32(o + 80, P + 0x3E0 + 8 * r)
        w16(o + 84, 0)
    # joints: only CV 3 has a list [rail 0, rail 1]
    w32(0x400 + 8 * 3, P + 0x440)
    w16(0x400 + 8 * 3 + 4, 2)
    w16(0x400 + 8 * 3 + 6, 2)
    w32(0x440, P + 0x150)
    w32(0x444, P + 0x150 + 112)
    return bytes(buf)


def synth_lights():
    buf = bytearray(0x300)
    P = VBASE
    struct.pack_into(">I", buf, 0x50, P + 0x80)
    struct.pack_into(">HH", buf, 0x54, 2, 2)
    for i, (typ, pos) in enumerate([(0, (1.0, 2.0, 3.0)), (1, (4.0, 5.0, 6.0))]):
        o = 0x80 + 96 * i
        struct.pack_into(">3f", buf, o, *pos)
        struct.pack_into(">3f", buf, o + 16, 1.0, 0.5, 0.25)
        struct.pack_into(">3f", buf, o + 32, 0.0, -1.0, 0.0)
        struct.pack_into(">f", buf, o + 64, 20.0)
        struct.pack_into(">f", buf, o + 68, 360.0 if typ == 0 else 60.0)
        buf[o + 88] = typ
        buf[o + 89] = 1
    return bytes(buf)


def _cmd_lamps(argv):
    """`lamps --game <folder> [--src <source tree>] [--live <lights.json | atlas folder>]`: decode the street lamps from the game's files and, with a live atlas.dump, compare them."""
    import json
    game = src = live = None
    it = iter(argv)
    for a in it:
        if a == "--game":
            game = next(it, None)
        elif a == "--src":
            src = next(it, None)
        elif a == "--live":
            live = next(it, None)
        else:
            print("lamps: unknown argument %s" % a)
            return 2
    if not game:
        print("usage: atlas_game.py lamps --game <folder with xarchive_cache.rpf> [--src <LARecomp source tree>] [--live <lights.json or atlas folder>]")
        return 2
    rpf = Rpf3(os.path.join(game, RPF_NAME), find_rpf_key([src] if src else []))
    lamps, info = read_lamps(rpf)
    print("lamps: %d (props %d, with lamp points %d, models without a template %d)" % (len(lamps), info["props"], info["lamp_props"], len(info["missing"])))
    if not live:
        return 0
    path = os.path.join(live, "lights.json") if os.path.isdir(live) else live
    with open(path, encoding="utf-8") as f:
        theirs = [l for l in json.load(f)["lights"] if l.get("src") == "lamp"]
    print("live lamps: %d" % len(theirs))
    if len(theirs) != len(lamps):
        print("FAIL lamp count %d / %d" % (len(lamps), len(theirs)))
        return 1
    worst = {"dir": 0.0, "color": 0.0, "intensity": 0.0, "cone": 0.0}
    other = 0
    for a, b in zip(lamps, theirs):
        worst["dir"] = max(worst["dir"], max(abs(x - y) for x, y in zip(a["dir"], b["dir"])))
        worst["color"] = max(worst["color"], max(abs(x - y) for x, y in zip(a["color"], b["color"][:3])))
        worst["intensity"] = max(worst["intensity"], abs(a["intensity"] - b["intensity"]))
        worst["cone"] = max(worst["cone"], abs(a["cone"] - b["cone"]))
        other += a["type"] != b["type"] or a["on"] != b["on"]
    print("largest differences: " + ", ".join("%s %.4f" % kv for kv in worst.items()) + "; type/on differing: %d" % other)
    return 0 if other == 0 and worst["dir"] < 0.002 and worst["color"] < 0.002 and worst["intensity"] < 0.01 and worst["cone"] < 0.01 else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        sys.exit(_selftest())
    if len(sys.argv) > 1 and sys.argv[1] == "lamps":
        sys.exit(_cmd_lamps(sys.argv[2:]))
    print(__doc__)
