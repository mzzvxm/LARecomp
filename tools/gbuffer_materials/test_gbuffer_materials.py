"""Tests of the G-buffer material tools (census_merge.py, author_materials.py, gbm_common.py).

    python -I tools/gbuffer_materials/test_gbuffer_materials.py

The renderer's own checks are src/gpu/gbuffer_materials_test.cpp: its golden census lines and its error cases are repeated here, and it parses sample_table.txt (made by these tools from
the sample_* files) with the renderer's parser, so the two languages read one format.
"""

import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import author_materials as am  # noqa: E402
import census_merge as cm  # noqa: E402
import gbm_common as g  # noqa: E402

SMOOTH, LIT = g.load_class_defaults()


def word(smooth, cls, flags, lit, metal=0):
    return g.join_word(g.Word(smooth, g.CLASS_NAMES.index(cls), flags, lit, metal))


class Common(unittest.TestCase):
    def test_class_defaults_are_the_headers(self):
        self.assertEqual(SMOOTH, [0, 38, 77, 230, 51, 13, 0, 26, 217, 242, 179, 242, 51, 0, 51, 242])
        self.assertEqual(LIT, [255] * 13 + [0, 255, 255])

    def test_bytes_are_the_renderers(self):
        self.assertEqual(g.to_byte(0.95), 242)
        self.assertEqual(g.to_byte(0.0), 0)
        self.assertEqual(g.to_byte(1.0), 255)
        self.assertEqual(g.to_byte(0.5), 128)
        # rough=0.9 is smoothness 1 - 0.9f in float: 25.5 + 0.5 rounds up to 26, not down to 25 as double arithmetic would
        self.assertEqual(g.parse_fields(["rough=0.9"])["smooth"], 26)
        self.assertEqual(g.parse_fields(["rough=0.05"])["smooth"], 242)
        self.assertEqual(g.parse_fields(["smooth=0.4"])["smooth"], 102)

    def test_class_names(self):
        self.assertEqual(len(g.CLASS_NAMES), 16)
        for i, name in enumerate(g.CLASS_NAMES):
            self.assertEqual(g.parse_class(name), i)
            self.assertEqual(g.parse_class(str(i)), i)
        self.assertEqual(g.parse_class("GLASS"), 11)
        for bad in ("16", "", "glas", "-1"):
            self.assertIsNone(g.parse_class(bad))

    def test_census_lines(self):
        row = g.parse_census_line("[22:14:03] [info] [gbuffer] texcensus tc1|300|shared|glass_01|0000000000000000|000000000000AAAA|256|128|18|3|450|01FF2040|1|1\r\n")
        self.assertEqual(row, g.CensusRow(300, "shared", "glass_01", 0, 0xAAAA, 256, 128, 18, 3, 450, 0x01FF2040, 1, 1))
        window = g.parse_census_line("texcensus tc1-window|300|4|3|6|468|0")
        self.assertEqual(window, g.CensusWindow(300, 4, 3, 6, 468, 0))
        for other in ("texcensus tc2|1|2", "a log line", "texcensus tc1|300|a|b", "texcensus tc1|x|a|b|0|0|1|1|1|1|1|0|0|0"):
            self.assertIsNone(g.parse_census_line(other))

    def test_the_word(self):
        w = word(102, "world", g.FLAG_VEHICLE | g.FLAG_ALPHA, 255)
        self.assertEqual(g.split_word(w), g.Word(102, 1, 6, 255, 0))
        self.assertEqual(g.join_word(g.split_word(w)), w)


class Table(unittest.TestCase):
    def test_a_clean_table(self):
        rows, errors = g.parse_table(
            "# a table\n@version 1\n\nshared/glass_01   class=glass rough=0.05 ssr=1            # a window\n  common_props/rim  class=chrome smooth=0.95 metal=1\r\n"
            "*/bumper class=paint\nhash:00000000000000AB metal=0.5 lit=0 ssr=off\n\n")
        self.assertEqual(errors, [])
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0], (("pair", "shared", "glass_01"), {"class": 11, "smooth": 242, "ssr": True}))
        self.assertEqual(rows[1][1], {"class": 9, "smooth": 242, "metal": 255})
        self.assertEqual(rows[2], (("any", "bumper"), {"class": 8}))
        self.assertEqual(rows[3], (("hash", 0xAB), {"metal": 128, "lit": 0, "ssr": False}))

    def test_the_error_cases_of_the_renderers_test(self):
        for text in ("shared/x\n", "shared/x class\n", "shared/x class=\n", "shared/x =glass\n", "shared/x class=glas\n", "shared/x rough=1.5\n", "shared/x rough=-0.1\n",
                     "shared/x rough=abc\n", "shared/x rough=0.5x\n", "shared/x rough=nan\n", "shared/x metal=inf\n", "shared/x ssr=maybe\n", "shared/x lit=2\n", "shared/x speed=1\n",
                     "shared/x class=glass class=metal\n", "shared/x rough=0.5 smooth=0.5\n", "common/unnamed class=glass\n", "glass class=glass\n", "@include other.txt\n", "@version\n"):
            rows, errors = g.parse_table(text)
            self.assertEqual(rows, [], text)
            self.assertEqual([e[0] for e in errors], [1], text)

    def test_good_rows_around_bad_ones_and_versions(self):
        rows, errors = g.parse_table("shared/a class=glass\nshared/b class=nonsense\n# c\n\nshared/c metal=1\nshared/d rough=9\n")
        self.assertEqual([e[0] for e in errors], [2, 6])
        self.assertEqual(len(rows), 2)
        rows, errors = g.parse_table("shared/a class=glass\n@version 2\nshared/b class=glass\n")
        self.assertEqual(rows, [])
        self.assertEqual([e[0] for e in errors], [2])
        rows, errors = g.parse_table("@version 1\nshared/a class=glass\n")
        self.assertEqual((len(rows), errors), (1, []))

    def test_keys(self):
        self.assertEqual(g.parse_key("HASH:0xabc"), ("hash", 0xABC))
        for bad in ("hash:", "hash:0", "hash:12345678901234567", "hash:12G4", "a/b/c", "/b", "a/", "*/UNNAMED"):
            with self.assertRaises(ValueError, msg=bad):
                g.parse_key(bad)
        self.assertEqual(g.key_text(("hash", 0xAB)), "hash:00000000000000AB")

    def test_apply_row_is_the_renderers(self):
        base = word(102, "world", g.FLAG_VEHICLE | g.FLAG_ALPHA, 255)

        def apply(fields, w=base):
            return g.split_word(g.apply_row(w, fields, SMOOTH, LIT))

        self.assertEqual(g.apply_row(base, {}, SMOOTH, LIT), base)
        self.assertEqual(apply({"smooth": 200}), g.Word(200, 1, 6, 255, 0))
        self.assertEqual(apply({"class": 11}), g.Word(242, 11, 6, 255, 0))              # a class alone takes the class's default smoothness
        self.assertEqual(apply({"class": 13}), g.Word(0, 13, 6, 0, 0))                  # emissive: lit weight 0
        self.assertEqual(apply({"class": 13, "smooth": 77, "lit": 128}), g.Word(77, 13, 6, 128, 0))
        self.assertEqual(apply({"ssr": True}), g.Word(102, 1, 7, 255, 0))
        self.assertEqual(apply({"ssr": False}, g.apply_row(base, {"ssr": True}, SMOOTH, LIT)), g.Word(102, 1, 6, 255, 0))
        self.assertEqual(apply({"metal": 200}), g.Word(102, 1, 6, 255, 200))
        full = {"class": 9, "smooth": 240, "metal": 255, "ssr": True, "lit": 255}
        done = g.apply_row(base, full, SMOOTH, LIT)
        self.assertEqual(g.split_word(done), g.Word(240, 9, 7, 255, 255))
        self.assertEqual(g.apply_row(done, full, SMOOTH, LIT), done)
        no_data = word(0, "none", 0, 255)
        self.assertEqual(g.apply_row(no_data, full, SMOOTH, LIT), no_data)


class Rules(unittest.TestCase):
    def test_the_default_rules_parse(self):
        rules = am.parse_rules((HERE / "material_rules.txt").read_text(encoding="utf-8"))
        self.assertGreaterEqual(len(rules), 10)
        self.assertEqual(len({r.ident for r in rules}), len(rules))

    def test_grammar_errors(self):
        for bad in ("a: name~x", "a name~x => class=glass", "a: nonsense~x => class=glass", "a: name~x => class=nope", "a: name~x =>", "a: name>=3 => class=glass", "a: draws>=x => class=glass",
                    "a: hash=zz => class=glass", "a: name~ => class=glass", "a: class=nope => class=glass", "a: name~x => class=glass\na: name~y => class=glass"):
            with self.assertRaises(am.RulesError, msg=bad):
                am.parse_rules(bad)

    def row(self, **kw):
        r = {"key": "shared/x", "dictionary": "shared", "name": "x", "hash": "-", "width": 64, "height": 32, "format": 18, "draws": 10, "indices": 100, "rowdraws": 0, "group": "xCityDefault",
             "groups": "xCityDefault:10", "class": "world", "smooth": 38, "flags": 0, "lit": 255, "word": "%08X" % word(38, "world", 0, 255), "pairs": 1, "windows": 1}
        r.update(kw)
        return r

    def test_conditions(self):
        def hit(rule_text, **kw):
            return am.matches(am.parse_rules("r: " + rule_text + " => rough=0.5")[0], self.row(**kw))

        self.assertTrue(hit("name~glass", name="Glass_01"))
        self.assertFalse(hit("name~glass", name="unnamed"))
        self.assertTrue(hit("name!~glass", name="unnamed"))
        self.assertFalse(hit("name!~glass", name="my_glass"))
        self.assertTrue(hit("name=x,y", name="Y"))
        self.assertTrue(hit("dict~sha"))
        self.assertTrue(hit("dict=shared"))
        self.assertFalse(hit("dict=common"))
        self.assertTrue(hit("group~city"))
        self.assertTrue(hit("group!~road"))
        self.assertTrue(hit("class=world,road"))
        self.assertTrue(hit("class!=road"))
        self.assertTrue(hit("named"))
        self.assertTrue(hit("unnamed", name="unnamed"))
        self.assertTrue(hit("vehicle", flags=4))
        self.assertTrue(hit("novehicle"))
        self.assertTrue(hit("draws>=10 indices>=100 size>=64 size<=64 smooth>=38 smooth<=38"))
        self.assertFalse(hit("draws>=11"))
        self.assertTrue(hit("hash=00B2E7B3C830B0E7", hash="00B2E7B3C830B0E7"))
        self.assertFalse(hit("hash=00B2E7B3C830B0E7"))

    def test_later_rules_win_and_skip_gives_up(self):
        rules = am.parse_rules("a: named => class=glass rough=0.1\nb: name~x => rough=0.5\n")
        items, _ = am.author([self.row()], rules, SMOOTH, LIT)
        self.assertEqual(items[0][1], {"class": 11, "smooth": 128})
        self.assertEqual(items[0][3], ["a", "b"])
        rules = am.parse_rules("a: named => class=glass rough=0.1\nb: name~x => skip\n")
        self.assertEqual(am.author([self.row()], rules, SMOOTH, LIT)[0], [])
        rules = am.parse_rules("a: named => class=glass rough=0.1\nb: name~x => skip\nc: name~x => metal=1\n")
        self.assertEqual(am.author([self.row()], rules, SMOOTH, LIT)[0][0][1], {"metal": 255})

    def test_redundant_and_unkeyed_rows(self):
        rules = am.parse_rules("a: named => class=world\n")
        self.assertEqual(am.author([self.row(smooth=38)], rules, SMOOTH, LIT)[0], [])  # world at the world default is what the family already gave
        self.assertEqual(len(am.author([self.row(smooth=38)], rules, SMOOTH, LIT, keep_redundant=True)[0]), 1)
        rules = am.parse_rules("a: unnamed => rough=0.5\n")
        items, stats = am.author([self.row(name="unnamed", key="-")], rules, SMOOTH, LIT)
        self.assertEqual((items, stats["no key (unnamed, hash never known)"]), ([], 1))
        items, stats = am.author([self.row(draws=1)], am.parse_rules("a: named => rough=0.5\n"), SMOOTH, LIT, min_draws=2)
        self.assertEqual((items, stats["below min-draws"]), ([], 1))


class Samples(unittest.TestCase):
    def census(self):
        names = g.load_shader_names(HERE / "sample_shader_names.tsv")
        textures, windows = cm.read_logs([HERE / "sample_census_log.txt"])
        return cm.records(textures, names), windows

    def test_shader_names(self):
        names = g.load_shader_names(HERE / "sample_shader_names.tsv")
        self.assertEqual(names[0xA0004].group, "xRimMain")
        self.assertEqual(names[0xA0004].cls, "chrome")
        self.assertEqual(names[0xB0001].stage, "v")

    def test_census_merge_is_the_committed_table(self):
        rows, windows = self.census()
        self.assertEqual(windows, 2)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "census.tsv"
            cm.write_tsv(out, rows)
            self.assertEqual(out.read_text(encoding="utf-8"), (HERE / "sample_census.tsv").read_text(encoding="utf-8"))
        # the early rows of a texture whose hash arrived in a later window are one texture
        puddle = [r for r in rows if r["name"] == "puddle_decal"]
        self.assertEqual(len(puddle), 1)
        self.assertEqual((puddle[0]["draws"], puddle[0]["windows"], puddle[0]["hash"]), (300, 2, "4444444444444444"))
        self.assertEqual(cm.coverage(rows)[0][0], 0.5)

    def test_the_tsv_reads_back(self):
        rows, _ = self.census()
        back = cm.read_tsv(HERE / "sample_census.tsv")
        self.assertEqual(back, rows)

    def test_author_is_the_committed_table(self):
        rows = cm.read_tsv(HERE / "sample_census.tsv")
        rules = am.parse_rules((HERE / "sample_rules.txt").read_text(encoding="utf-8"))
        items, stats = am.author(rows, rules, SMOOTH, LIT)
        text = am.table_text(items, "sample_census.tsv", "sample_rules.txt", len(rules), len(rows), sum(r["draws"] for r in rows))
        self.assertEqual(text, (HERE / "sample_table.txt").read_text(encoding="utf-8"))
        parsed, errors = g.parse_table(text)
        self.assertEqual(errors, [])
        self.assertEqual(len(parsed), 6)
        self.assertEqual(stats["no key (unnamed, hash never known)"], 1)
        self.assertEqual(stats["row would not change the word"], 1)
        # the hash row the renderer's test looks for
        self.assertIn((("hash", 0x00B2E7B3C830B0E7), {"class": 11, "smooth": g.to_byte(0.92), "ssr": True}), parsed)

    def test_overrides_check(self):
        rows = cm.read_tsv(HERE / "sample_census.tsv")
        lines, bad = am.check_overrides(HERE / "sample_overrides.txt", rows)
        self.assertEqual(bad, 0)
        self.assertTrue(any("DEADBEEFDEADBEEF" in line for line in lines))
        self.assertFalse(any("glass_storefront_01" in line for line in lines))
        with tempfile.TemporaryDirectory() as tmp:
            broken = Path(tmp) / "o.txt"
            broken.write_text("shared/a class=glas\n", encoding="utf-8")
            self.assertEqual(am.check_overrides(broken, rows)[1], 1)

    def test_main_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            census = Path(tmp) / "census.tsv"
            table = Path(tmp) / "table.txt"
            self.assertEqual(cm.main(["--logs", str(HERE / "sample_census_log.txt"), "--out", str(census), "--shader-names", str(HERE / "sample_shader_names.tsv"), "--top", "3"]), 0)
            self.assertEqual(am.main(["--census", str(census), "--rules", str(HERE / "sample_rules.txt"), "--out", str(table), "--overrides", str(HERE / "sample_overrides.txt")]), 0)
            self.assertEqual(len(g.parse_table(table.read_text(encoding="utf-8"))[0]), 6)
            self.assertEqual(am.main(["--census", str(census), "--rules", str(HERE / "material_rules.txt"), "--out", str(Path(tmp) / "t2.txt")]), 0)
            bad_rules = Path(tmp) / "bad.txt"
            bad_rules.write_text("a: nonsense => class=glass\n", encoding="utf-8")
            self.assertEqual(am.main(["--census", str(census), "--rules", str(bad_rules), "--out", str(Path(tmp) / "t3.txt")]), 2)
            self.assertFalse((Path(tmp) / "t3.txt").exists())

    def test_dump_listing_fills_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            for rel in ("shared/glass_storefront_01__AAAAAAAAAAAAAAAA.dds", "shared/other__BBBBBBBBBBBBBBBB.dds", "common/unnamed__CCCCCCCCCCCCCCCC.dds", "common/__sky_x.dds__DDDDDDDDDDDDDDDD.dds",
                        "common/readme.txt"):
                p = Path(tmp) / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b"")
            dump = g.load_dump_listing(tmp)
            self.assertEqual(len(dump), 4)
            self.assertIn(("common", "__sky_x.dds", 0xDDDDDDDDDDDDDDDD), dump)
            textures, _ = cm.read_logs([HERE / "sample_census_log.txt"])
            # a named texture the census saw without a hash gets the dump's, only when the dump has exactly one
            for tex in textures.values():
                if tex.name == "glass_storefront_01":
                    tex.hash = 0
            self.assertEqual(cm.fill_hashes_from_dump(textures, dump), 1)
            self.assertTrue(any(t.hash == 0xAAAAAAAAAAAAAAAA for t in textures.values()))

    def test_summary_mentions_a_dirty_census(self):
        rows, windows = self.census()
        rows[0]["rowdraws"] = 5
        text = "\n".join(cm.summary(rows, windows, 1, 3, None, True))
        self.assertIn("WARNING", text)
        self.assertIn("unnamed textures", text)
        self.assertIn("coverage:", text)
        self.assertIn("no census lines", "\n".join(cm.summary([], 0, 1, 3)))


if __name__ == "__main__":
    unittest.main(verbosity=1)
