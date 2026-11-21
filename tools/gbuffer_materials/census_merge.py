r"""The census tool of the G-buffer's per-texture material table (package GB-F2, perf/LIGHT-UNIFY.md 11.2).

A game run with MCLA_GBUFFER_CENSUS=1 logs, every 300 frames, one `texcensus` line per (texture, pixel shader) the G-buffer's draws used (src/gpu/gbuffer_materials.h
TextureCensus). This tool reads the logs of any number of runs, joins the pixel shaders' identities to their group and class through the shader names file the pack build writes
(mcla_gbuffer_shader_names.v1.tsv, next to the shader pack), and writes ONE ROW PER TEXTURE: how often the city's draws used it, how much geometry (indices) it covered, with which
shader family and the word that family gave it. It prints what the census says about the whole: how many textures carry how much of the picture, how many are unnamed, where the
families' classes stand. That list is what author_materials.py turns into the table, and what the review (a contact sheet of the busiest textures per family, the owner's eye) is made from.

  python -I census_merge.py --logs <run folder or larecomp.log> [more ...] --out census.tsv
        [--shader-names <pack folder>\mcla_gbuffer_shader_names.v1.tsv] [--dump-listing <mods>\textures_dump] [--top 40]

The census must be taken with no table loaded (set MCLA_GBUFFER_TEXTURE_MATERIALS=off for the run): the word it logs is the one AFTER a row, and the tool says so when it finds
draws that a row changed. `indices` is the sum of the draws' index counts: a stand-in for the surface a texture covers (the renderer reads no pixels back), good for ranking.

THE RUN (the main session: one game at a time, the display awake, notifications off). Build the exe of the branch (perf\scripts\build-set-011.ps1 -Name gbf2 -Ref feat/gb-f2 -Sdk p011b); the
first run builds the variant pack again (the material rules version is 2) and writes mcla_gbuffer_shader_names.v1.tsv beside mcla_shaders_variants.v3.pack. Then, for each route
(sunset-and-vine, showcase3-broadway, showcase3-fountain, showcase3-wilshire; ports\scripts\routes) and each hour (12 and 22), as in chain-census2.ps1:

    $P = 'C:\mcv\larecomp\ports'; $n = 'gbf2-census-day-sunset'     # night: --time-of-day 22 and a name with -night-
    python -I $lr --shared --keep-display --game --max-min 8 --log "$P\logs\$n.log" -- python -I "$P\scripts\opt_run.py" --name $n --exe-dir "$P\exe\gbf2" --adapter 0 `
        --adapter-name 'NVIDIA GeForce RTX 5080' --backend d3d12 --log-level info --fps-limit 60 --starter "$P\scratch\starter\merged-1.bin" --time-of-day 12 `
        --drive-route sunset-and-vine.route --roam-s 120 --env MCLA_GBUFFER_CENSUS=1 --env MCLA_GBUFFER_TEXTURE_MATERIALS=off --extra --mcla_gpu_gbuffer=normals

($lr is lockrun.py of the session's tools folder.) The census needs the G-buffer on (mcla_gpu_gbuffer=normals) and no table (=off). Each run logs a census window every 300 frames (24 in 120 s).
Then merge every run and look at the summary, and write the table:

    python -I tools\gbuffer_materials\census_merge.py --logs (Get-ChildItem $P\run -Directory -Filter 'gbf2-census-*').FullName --out census.tsv `
        --shader-names <pack folder>\mcla_gbuffer_shader_names.v1.tsv --dump-listing <mods>\textures_dump
    python -I tools\gbuffer_materials\author_materials.py --census census.tsv --rules tools\gbuffer_materials\material_rules.txt --out <pack folder>\mcla_gbuffer_texture_materials.txt

(<mods>\textures_dump is filled by a run with mcla_gpu_texture_dump=true; only the file names are read.) A running game reads the table again within a second of the file being saved.

census.tsv, tab separated with a header line, most draws first:
  key          the table key of the texture: dictionary/name for a named one, hash:<16 hex> for an unnamed one whose content hash is known, else '-' (no key)
  dictionary, name, hash ('-' when unknown), width, height, format (xenos::TextureFormat number)
  draws, indices, rowdraws (draws that a table row changed: not 0 means the census was not clean)
  group        the shader group of the pair with the most draws; groups: every group with its draws (xCityDefault:120,xCityRoad:4)
  class, smooth, flags, lit, word    what that pair's word said (the class by name, the bytes, the whole word in hex)
  pairs        how many (texture, pixel shader) pairs, windows how many census windows it was seen in
"""

import argparse
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gbm_common as g  # noqa: E402

COLUMNS = ["key", "dictionary", "name", "hash", "width", "height", "format", "draws", "indices", "rowdraws", "group", "groups", "class", "smooth", "flags", "lit", "word", "pairs", "windows"]


class Texture:
    def __init__(self, dictionary, name, hash_, width, height, fmt):
        self.dictionary = dictionary
        self.name = name
        self.hash = hash_
        self.width = width
        self.height = height
        self.fmt = fmt
        self.pairs = {}  # ps -> [draws, indices, word, rows]
        self.windows = set()

    @property
    def draws(self):
        return sum(p[0] for p in self.pairs.values())

    @property
    def indices(self):
        return sum(p[1] for p in self.pairs.values())

    @property
    def rowdraws(self):
        return sum(p[3] for p in self.pairs.values())

    @property
    def named(self):
        return self.name.lower() != "unnamed"

    def key(self):
        # a named texture is keyed by name (readable, and it survives a change of the texture's pixels), an unnamed one by its content hash
        if self.named:
            return "%s/%s" % (self.dictionary, self.name)
        if self.hash:
            return "hash:%016X" % self.hash
        return "-"


def log_files(paths):
    files = []
    for p in paths:
        path = Path(p)
        files.append(path / "larecomp.log" if path.is_dir() else path)
    return files


def read_logs(paths):
    """The census rows of the logs: {identity: Texture}, and the number of windows read. A texture whose hash was not known yet in an early window (hash 0) is merged into the hashed
    texture of the same dictionary, name, size and format when there is exactly one."""
    textures = {}
    windows_total = 0
    for log_index, path in enumerate(log_files(paths)):
        window = 0
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if "texcensus tc1" not in line:
                    continue
                item = g.parse_census_line(line)
                if item is None:
                    continue
                if isinstance(item, g.CensusWindow):
                    window += 1
                    windows_total += 1
                    continue
                ident = ("h", item.hash) if item.hash else ("n", item.dictionary, item.name, item.width, item.height, item.fmt)
                tex = textures.get(ident)
                if tex is None:
                    tex = textures[ident] = Texture(item.dictionary, item.name, item.hash, item.width, item.height, item.fmt)
                pair = tex.pairs.get(item.ps)
                if pair is None:
                    tex.pairs[item.ps] = [item.draws, item.indices, item.word, item.rows]
                else:
                    pair[0] += item.draws
                    pair[1] += item.indices
                    pair[3] += item.rows
                tex.windows.add((log_index, window))
    # the early rows of textures whose hash arrived later
    by_name = defaultdict(list)
    for ident, tex in textures.items():
        if ident[0] == "h":
            by_name[(tex.dictionary, tex.name, tex.width, tex.height, tex.fmt)].append(tex)
    for ident in [i for i in textures if i[0] == "n" and textures[i].named]:
        candidates = by_name.get((ident[1], ident[2], ident[3], ident[4], ident[5]), [])
        if len(candidates) == 1:
            late = textures.pop(ident)
            target = candidates[0]
            for ps, pair in late.pairs.items():
                mine = target.pairs.get(ps)
                if mine is None:
                    target.pairs[ps] = pair
                else:
                    mine[0] += pair[0]
                    mine[1] += pair[1]
                    mine[3] += pair[3]
            target.windows |= late.windows
    return textures, windows_total


def fill_hashes_from_dump(textures, dump):
    """A named texture the census saw with no hash gets the hash of its dump file when the dump has exactly one texture of that name in that dictionary."""
    by_pair = defaultdict(set)
    for dictionary, name, hash_ in dump:
        by_pair[(dictionary.lower(), name.lower())].add(hash_)
    filled = 0
    for tex in textures.values():
        if not tex.hash and tex.named:
            hashes = by_pair.get((tex.dictionary.lower(), tex.name.lower()), set())
            if len(hashes) == 1:
                tex.hash = next(iter(hashes))
                filled += 1
    return filled


def records(textures, names=None):
    """One dict per texture, most draws first."""
    out = []
    for tex in textures.values():
        by_group = defaultdict(int)
        main_ps = max(tex.pairs, key=lambda ps: (tex.pairs[ps][0], -ps))
        for ps, pair in tex.pairs.items():
            info = names.get(ps) if names else None
            by_group[info.group if info else "ps_%016X" % ps] += pair[0]
        info = names.get(main_ps) if names else None
        group = info.group if info else "ps_%016X" % main_ps
        word = tex.pairs[main_ps][2]
        w = g.split_word(word)
        out.append({
            "key": tex.key(), "dictionary": tex.dictionary, "name": tex.name, "hash": ("%016X" % tex.hash) if tex.hash else "-",
            "width": tex.width, "height": tex.height, "format": tex.fmt, "draws": tex.draws, "indices": tex.indices, "rowdraws": tex.rowdraws,
            "group": group, "groups": ",".join("%s:%d" % (k, v) for k, v in sorted(by_group.items(), key=lambda kv: (-kv[1], kv[0]))),
            "class": g.class_name(w.cls), "smooth": w.smooth, "flags": w.flags, "lit": w.lit, "word": "%08X" % word,
            "pairs": len(tex.pairs), "windows": len(tex.windows),
        })
    out.sort(key=lambda r: (-r["draws"], -r["indices"], r["key"]))
    return out


def write_tsv(path, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(COLUMNS) + "\n")
        for r in rows:
            f.write("\t".join(str(r[c]) for c in COLUMNS) + "\n")


def read_tsv(path):
    """The rows of a census.tsv (ints back as ints)."""
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        header = f.readline().rstrip("\r\n").split("\t")
        if header != COLUMNS:
            raise ValueError("%s is not a census table (columns %s)" % (path, ",".join(header)))
        for line in f:
            cells = line.rstrip("\r\n").split("\t")
            if len(cells) != len(COLUMNS):
                continue
            r = dict(zip(COLUMNS, cells))
            for c in ("width", "height", "format", "draws", "indices", "rowdraws", "smooth", "flags", "lit", "pairs", "windows"):
                r[c] = int(r[c])
            rows.append(r)
    return rows


def coverage(rows, shares=(0.5, 0.8, 0.9, 0.95, 0.99)):
    """[(share, textures)]: how many of the busiest textures carry that share of the draws."""
    total = sum(r["draws"] for r in rows)
    out = []
    running = 0
    n = 0
    wanted = list(shares)
    for r in rows:
        running += r["draws"]
        n += 1
        while wanted and running >= wanted[0] * total:
            out.append((wanted.pop(0), n))
    return out


def summary(rows, windows, logs, top, dump=None, names_loaded=False):
    total_draws = sum(r["draws"] for r in rows)
    total_indices = sum(r["indices"] for r in rows)
    lines = []
    lines.append("census: %d log(s), %d window(s), %d textures, %d draws, %d indices" % (logs, windows, len(rows), total_draws, total_indices))
    if total_draws == 0:
        lines.append("  no census lines: was MCLA_GBUFFER_CENSUS=1 set, and did the run reach the city with the G-buffer on?")
        return lines

    def share(part):
        return 100.0 * part / total_draws

    unnamed = [r for r in rows if r["name"].lower() == "unnamed"]
    nohash = [r for r in rows if r["hash"] == "-"]
    lines.append("  unnamed textures: %d (%.1f%% of draws): keyed by hash only; no hash yet: %d (%.1f%% of draws)" % (len(unnamed), share(sum(r["draws"] for r in unnamed)), len(nohash), share(sum(r["draws"] for r in nohash))))
    contaminated = sum(r["rowdraws"] for r in rows)
    if contaminated:
        lines.append("  WARNING: %d draws were changed by a table row (rowdraws): run the census with MCLA_GBUFFER_TEXTURE_MATERIALS=off" % contaminated)
    by_class = defaultdict(int)
    by_group = defaultdict(int)
    for r in rows:
        by_class[r["class"]] += r["draws"]
        by_group[r["group"]] += r["draws"]
    lines.append("  draws by class: " + ", ".join("%s %.1f%%" % (k, share(v)) for k, v in sorted(by_class.items(), key=lambda kv: -kv[1])))
    shown = sorted(by_group.items(), key=lambda kv: -kv[1])[:12]
    lines.append("  draws by group: " + ", ".join("%s %.1f%%" % (k, share(v)) for k, v in shown) + (" ..." if len(by_group) > len(shown) else ""))
    if not names_loaded:
        lines.append("  (no shader names file: the groups are pixel shader identities; pass --shader-names <pack folder>\\mcla_gbuffer_shader_names.v1.tsv)")
    lines.append("  coverage: " + ", ".join("%d%% of the draws in %d textures" % (round(s * 100), n) for s, n in coverage(rows)))
    lines.append("  the %d busiest textures:" % min(top, len(rows)))
    lines.append("    %-30s %-9s %-26s %-12s %7s %9s  %s" % ("key", "size", "group", "class", "smooth", "draws", "name"))
    for r in rows[:top]:
        lines.append("    %-30s %-9s %-26s %-12s %7d %9d  %s/%s" % (r["key"], "%dx%d" % (r["width"], r["height"]), r["group"][:26], r["class"], r["smooth"], r["draws"], r["dictionary"], r["name"]))
    if dump is not None:
        seen = {int(r["hash"], 16) for r in rows if r["hash"] != "-"}
        by_dict = defaultdict(lambda: [0, 0])
        for dictionary, _name, hash_ in dump:
            by_dict[dictionary][0] += 1
            by_dict[dictionary][1] += 1 if hash_ in seen else 0
        lines.append("  the dump listing has %d textures, %d of them seen by the census:" % (len(dump), sum(v[1] for v in by_dict.values())))
        lines.append("    " + ", ".join("%s %d/%d" % (k, v[1], v[0]) for k, v in sorted(by_dict.items(), key=lambda kv: -kv[1][0])[:10]))
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description="merge the logs' G-buffer texture census lines into one row per texture (see the top of this file)")
    ap.add_argument("--logs", nargs="+", required=True, help="run folders (their larecomp.log) or log files")
    ap.add_argument("--out", help="the census table to write (census.tsv)")
    ap.add_argument("--shader-names", help="mcla_gbuffer_shader_names.v1.tsv, next to the shader pack: joins pixel shader identities to groups")
    ap.add_argument("--dump-listing", help="a texture dump folder (<mods>\\textures_dump): only the file names are read; gives the hash of named textures the census saw without one")
    ap.add_argument("--top", type=int, default=40, help="how many of the busiest textures the summary lists")
    a = ap.parse_args(argv)
    names = g.load_shader_names(a.shader_names) if a.shader_names else None
    dump = g.load_dump_listing(a.dump_listing) if a.dump_listing else None
    textures, windows = read_logs(a.logs)
    if dump is not None:
        filled = fill_hashes_from_dump(textures, dump)
        print("dump listing: %d textures, %d named census textures got their hash from it" % (len(dump), filled))
    rows = records(textures, names)
    if a.out:
        write_tsv(a.out, rows)
        print("wrote %s (%d textures)" % (a.out, len(rows)))
    for line in summary(rows, windows, len(a.logs), a.top, dump, names is not None):
        print(line)
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
