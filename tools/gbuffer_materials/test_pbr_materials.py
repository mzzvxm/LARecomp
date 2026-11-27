"""Tests of the PBR-2 additions to the G-buffer material tools (perf/PBR.md section 2): the confidence of the rules and the review list (author_materials.py), the classifier on a made-up list of
texture names with the real rules file (material_rules.txt), the city name list (census_merge.py --names), and the emitter of the compiled-in rows (emit_rows.py).

    python -I tools/gbuffer_materials/test_pbr_materials.py

The renderer's side of the same table is src/gpu/gbuffer_texture_rows_test.cpp: it includes sample_rows.inc (made here from sample_table.txt and sample_overrides.txt) and looks every row of it up with
the renderer's own keys, so that this file's FNV and the C++ one cannot drift apart.
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.dont_write_bytecode = True
import author_materials as am  # noqa: E402
import census_merge as cm  # noqa: E402
import emit_rows as er  # noqa: E402
import gbm_common as g  # noqa: E402

SMOOTH, LIT = g.load_class_defaults()
METAL = g.load_class_metal()
RULES = am.parse_rules((HERE / "material_rules.txt").read_text(encoding="utf-8"))


def census_row(name, group, cls="world", flags=0, smooth=38, draws=500, dictionary="shared", width=256, height=256, hash_="-", lit=255, metal=0):
    """A row of census.tsv as author_materials reads it."""
    named = name.lower() != "unnamed"
    cls_number = g.CLASS_NAMES.index(cls)
    word = g.join_word(g.Word(smooth, cls_number, flags, lit, metal))
    if named:
        key = "%s/%s" % (dictionary, name)
    else:
        key = "hash:%s" % hash_ if hash_ != "-" else "-"
    return {"key": key, "dictionary": dictionary, "name": name, "hash": hash_, "width": width, "height": height, "format": 54, "draws": draws, "indices": draws * 100, "rowdraws": 0,
            "group": group, "groups": "%s:%d" % (group, draws), "class": cls, "smooth": smooth, "flags": flags, "lit": lit, "word": "%08X" % word, "pairs": 1, "windows": 1}


def classify(*rows, min_conf="medium"):
    """({key: (fields, confidence)}, review list) for census rows, with the real rules."""
    for rule in RULES:
        rule.matched = 0
    items, review, _stats = am.author_with_review(list(rows), RULES, SMOOTH, LIT, 1, False, min_conf)
    return {item[0]: (item[1], item[4]) for item in items}, review


def smooth_byte(rough):
    return g.to_byte(1.0 - g.f32(rough))


class Confidence(unittest.TestCase):
    def test_rules_file_grammar(self):
        self.assertTrue(RULES)
        for rule in RULES:
            self.assertIn(rule.conf, am.CONF_LEVELS)
            self.assertTrue(rule.fields or rule.skip, rule.ident)
        # every level is used: the file has high, medium and low evidence
        self.assertEqual({r.conf for r in RULES}, {"high", "medium", "low"})

    def test_conf_token(self):
        rules = am.parse_rules("a: name~x => rough=0.5\nb: name~y => rough=0.5 conf=high\nc: name~z => rough=0.5 conf=LOW\n")
        self.assertEqual([r.conf for r in rules], ["medium", "high", "low"])
        self.assertEqual(rules[1].fields, {"smooth": g.to_byte(0.5)})  # conf= is not a table field
        for bad in ("a: name~x => rough=0.5 conf=sure\n", "a: name~x => conf=high\n", "a: name~x => conf=\n"):
            with self.assertRaises(am.RulesError):
                am.parse_rules(bad)

    def test_the_lowest_rule_decides_and_low_rows_are_listed_not_written(self):
        rules = am.parse_rules("a: name~wet => class=wetroad rough=0.1 conf=high\nb: name~tile => rough=0.3 conf=low\n")
        rows = [census_row("wet_floor", "xCityRoad", "road", 1, 77), census_row("wet_tile", "xCityRoad", "road", 1, 77), census_row("tile_only", "xCityDefault")]
        items, review, stats = am.author_with_review(rows, rules, SMOOTH, LIT, 1, False, "medium")
        self.assertEqual([i[0] for i in items], ["shared/wet_floor"])
        self.assertEqual(items[0][4], "high")
        self.assertEqual(sorted(r[0] for r in review), ["shared/tile_only", "shared/wet_tile"])
        self.assertEqual(stats["held for review (confidence below medium)"], 2)
        why = {r[0]: r[5] for r in review}
        self.assertEqual(why["shared/wet_tile"], "low confidence: b")  # a high rule and a low one: the low one is the weakest
        # --min-confidence low writes them all, high writes only the high ones
        low_items, low_review, _ = am.author_with_review(rows, rules, SMOOTH, LIT, 1, False, "low")
        self.assertEqual(len(low_items), 3)
        self.assertEqual(low_review, [])
        high_items, high_review, _ = am.author_with_review(rows, rules, SMOOTH, LIT, 1, False, "high")
        self.assertEqual([i[0] for i in high_items], ["shared/wet_floor"])
        self.assertEqual(len(high_review), 2)

    def test_the_first_version_of_author_is_unchanged(self):
        rules = am.parse_rules("a: name~tile => rough=0.3 conf=low\n")
        items, stats = am.author([census_row("tile", "xCityDefault")], rules, SMOOTH, LIT)
        self.assertEqual(len(items), 1)
        self.assertEqual(len(items[0]), 4)
        self.assertEqual(stats["rows written"], 1)

    def test_a_low_row_with_no_key_is_listed_too(self):
        rules = am.parse_rules("a: unnamed => rough=0.3 conf=low\nb: unnamed group~xCityRoad => rough=0.2\n")
        rows = [census_row("unnamed", "xCityDefault"), census_row("unnamed", "xCityRoad", "road", 1, 77)]
        items, review, stats = am.author_with_review(rows, rules, SMOOTH, LIT)
        self.assertEqual(items, [])
        self.assertEqual([r[0] for r in review], ["-", "-"])  # both are listed, though neither has a key; the road row's rule b is medium but a is low: the weakest decides
        self.assertEqual(stats["held for review (confidence below medium)"], 2)

    def test_review_list_text(self):
        rules = am.parse_rules("a: name~tile => rough=0.3 conf=low\n")
        rows = [census_row("tile_a", "xCityDefault", draws=10), census_row("tile_b", "xCityDefault", draws=900)]
        _items, review, _ = am.author_with_review(rows, rules, SMOOTH, LIT)
        lines = am.review_text(review).split("\n")
        self.assertEqual(lines[0].split("\t"), am.REVIEW_COLUMNS)
        self.assertTrue(lines[1].startswith("shared/tile_b\t"))  # the busiest first
        self.assertEqual(lines[1].split("\t")[am.REVIEW_COLUMNS.index("confidence")], "low")
        self.assertEqual(lines[-1], "")
        self.assertEqual(len(lines), 4)

    def test_the_table_comment_says_the_confidence(self):
        rules = am.parse_rules("a: name~tile => rough=0.3 conf=high\n")
        items, _review, _ = am.author_with_review([census_row("tile", "xCityDefault")], rules, SMOOTH, LIT)
        text = am.table_text(items, "census.tsv", "rules.txt", 1, 1, 500)
        self.assertIn("[a] conf=high", text)
        parsed, errors = g.parse_table(text)
        self.assertEqual(errors, [])
        self.assertEqual(len(parsed), 1)


class Classifier(unittest.TestCase):
    """The real rules on a made-up list of names, the way the review's first look is made."""

    def table(self):
        rows = [
            census_row("BMW_Chrome_Trim_D", "xBumpSpec", "world", g.FLAG_VEHICLE, 38, dictionary="vehicles"),
            census_row("windshield_front", "xCarGlass", "glass", g.FLAG_VEHICLE, 242, dictionary="vehicles"),
            census_row("Asphalt_Dry_01", "xCityRoad", "road", g.FLAG_SSR, 77),
            census_row("concrete_wall_02", "xCityBumpSpec", smooth=100),
            census_row("brick_red", "xCityBumpSpec", smooth=100),
            census_row("Neon_Sign_01", "xCityDefault"),
            census_row("headlight_lens", "xBumpSpec", "world", g.FLAG_VEHICLE, 38, dictionary="vehicles"),
            census_row("tyre_sidewall", "xBumpSpec", "world", g.FLAG_VEHICLE, 38, dictionary="vehicles"),
            census_row("puddle_01", "xCityRoad", "road", g.FLAG_SSR, 77),
            census_row("water_stain_wall", "xCityDefault"),
            census_row("glass_block", "xCityDefault"),
            census_row("exhaust_tip", "xBumpSpec", "world", g.FLAG_VEHICLE, 38, dictionary="vehicles"),
            census_row("RIM_Alloy_5spoke", "xBumpSpec", "world", g.FLAG_VEHICLE, 38, dictionary="vehicles"),
            census_row("Wheel_Hubcap", "xBumpSpec", "world", g.FLAG_VEHICLE, 38, dictionary="vehicles"),
            census_row("rusty_metal_grate", "xPropMetal", "metal", g.FLAG_SSR, 179),
            census_row("wet_tile_floor", "xCityRoad", "road", g.FLAG_SSR, 77),
            census_row("plastic_trim", "xBumpSpec", "world", g.FLAG_VEHICLE, 38, dictionary="vehicles"),
            census_row("unnamed", "xCityDefault", smooth=230, draws=300, hash_="00000000000000A1"),
            census_row("unnamed", "xCityDefault", smooth=38, draws=300, hash_="00000000000000A2"),
            census_row("unnamed", "xCityRoad", "road", g.FLAG_SSR, 20, draws=300, hash_="00000000000000A3"),
            census_row("grass_01", "xCityGrass", "foliage", 0, 26),
        ]
        return classify(*rows)

    def test_names_become_rows(self):
        table, _review = self.table()
        chrome, conf = table["vehicles/BMW_Chrome_Trim_D"]
        self.assertEqual((chrome["class"], chrome["metal"], chrome["ssr"], conf), (g.CLASS_NAMES.index("chrome"), 255, False, "high"))
        self.assertEqual(chrome["smooth"], smooth_byte(0.05))
        shield, conf = table["vehicles/windshield_front"]
        self.assertEqual((shield["class"], shield["smooth"], shield["ssr"], conf), (g.CLASS_NAMES.index("glass"), smooth_byte(0.04), False, "high"))
        asphalt, conf = table["shared/Asphalt_Dry_01"]
        self.assertEqual((asphalt["smooth"], conf), (smooth_byte(0.65), "high"))
        self.assertNotIn("class", asphalt)  # the family already says road
        for name, rough in (("concrete_wall_02", 0.85), ("brick_red", 0.90)):
            fields, conf = table["shared/" + name]
            self.assertEqual((fields["smooth"], conf), (smooth_byte(rough), "medium"), name)
        neon, conf = table["shared/Neon_Sign_01"]
        self.assertEqual((neon["class"], conf), (g.CLASS_NAMES.index("emissive"), "medium"))
        tyre, conf = table["vehicles/tyre_sidewall"]
        self.assertEqual((tyre["smooth"], tyre["metal"], conf), (smooth_byte(0.92), 0, "high"))
        wet, conf = table["shared/puddle_01"]
        self.assertEqual((wet["class"], wet["ssr"], conf), (g.CLASS_NAMES.index("wetroad"), True, "medium"))
        glass, conf = table["shared/glass_block"]
        self.assertEqual((glass["class"], glass["ssr"], conf), (g.CLASS_NAMES.index("glass"), True, "medium"))
        exhaust, conf = table["vehicles/exhaust_tip"]
        self.assertEqual((exhaust["class"], exhaust["metal"], exhaust["ssr"], conf), (g.CLASS_NAMES.index("metal"), 255, False, "medium"))
        rim, conf = table["vehicles/RIM_Alloy_5spoke"]
        self.assertEqual((rim["class"], rim["metal"], conf), (g.CLASS_NAMES.index("chrome"), 255, "medium"))
        plastic, conf = table["vehicles/plastic_trim"]
        self.assertEqual((plastic["smooth"], plastic["metal"], conf), (smooth_byte(0.55), 0, "medium"))

    def test_later_rules_win_and_keep_the_earlier_class(self):
        table, _review = self.table()
        grate, conf = table["shared/rusty_metal_grate"]
        self.assertEqual(grate["smooth"], smooth_byte(0.80))  # rust after metal-props
        self.assertEqual(grate["class"], g.CLASS_NAMES.index("metal"))
        self.assertEqual(grate["metal"], 255)
        self.assertEqual(conf, "medium")

    def test_caliper_keeps_paint_when_a_brake_texture_name_suggests_metal(self):
        caliper = census_row("chrome_rotor_shared", "xbrakecaliper", "paint", g.FLAG_VEHICLE, 153, dictionary="calipers")
        rotor = census_row("chrome_rotor_shared", "xWheelBrakeRotor", "metal", g.FLAG_VEHICLE, 178, dictionary="rotors", metal=255)
        table, review = classify(caliper, rotor)
        self.assertNotIn(caliper["key"], table)
        self.assertNotIn(caliper["key"], {row[0] for row in review})
        self.assertEqual(table[rotor["key"]][0]["class"], g.CLASS_NAMES.index("metal"))

    def test_doubtful_ones_are_in_the_review_not_the_table(self):
        table, review = self.table()
        held = {r[0]: r for r in review}
        for key in ("vehicles/headlight_lens", "shared/water_stain_wall", "vehicles/Wheel_Hubcap", "shared/wet_tile_floor", "hash:00000000000000A1", "hash:00000000000000A3"):
            self.assertIn(key, held, key)
            self.assertNotIn(key, table, key)
            self.assertEqual(held[key][4], "low", key)
        self.assertIn("lamp-lenses", held["vehicles/headlight_lens"][5])
        self.assertIn("water-names", held["shared/water_stain_wall"][5])
        self.assertIn("tiles", held["shared/wet_tile_floor"][5])  # puddles (medium) + tiles (low): the weakest is named
        self.assertIn("gloss-polished", held["hash:00000000000000A1"][5])
        self.assertIn("gloss-rough-road", held["hash:00000000000000A3"][5])
        # the glossy unnamed wall texture is only a candidate for SSR: its proposal is what the rule says
        self.assertEqual(held["hash:00000000000000A1"][1], {"ssr": True})

    def test_nothing_else_gets_a_row(self):
        table, review = self.table()
        self.assertNotIn("hash:00000000000000A2", table)  # an unnamed texture with a plain gloss
        self.assertFalse([r for r in review if r[0] == "hash:00000000000000A2"])
        self.assertNotIn("shared/grass_01", table)

    def test_every_row_the_classifier_writes_survives_the_renderers_parser(self):
        table, _review = self.table()
        items = []
        for key, (fields, conf) in table.items():
            items.append((key, fields, census_row("x", "y"), [], conf))
        text = am.table_text(items, "census.tsv", "material_rules.txt", len(RULES), len(items), 1)
        rows, errors = g.parse_table(text)
        self.assertEqual(errors, [])
        self.assertEqual(len(rows), len(table))

    def test_metal_follows_the_class_when_a_rule_gives_none(self):
        # a texture named "mirror" on a plain wall becomes chrome and takes metal 1 from the rule; one that a class-only row (emissive) changes takes the class's metalness
        rules = am.parse_rules("a: name~glow => class=chrome\n")
        items, _r, _s = am.author_with_review([census_row("glow", "xCityDefault", metal=0)], rules, SMOOTH, LIT)
        word = g.apply_row(int(census_row("glow", "xCityDefault")["word"], 16), items[0][1], SMOOTH, LIT, METAL)
        self.assertEqual(g.split_word(word).metal, 255)
        self.assertEqual(g.split_word(word).smooth, SMOOTH[g.CLASS_NAMES.index("chrome")])


class CityNames(unittest.TestCase):
    def test_name_list_and_aliases(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "city_texture_names.tsv"
            path.write_text("hash\tname\n00000000000000A1\tbrick_red\n00000000000000A1\tother\n0000000000000BB2\tmetal_door\nnot a row\n# comment\n", encoding="utf-8")
            names = g.load_name_list(path)
        self.assertEqual(names, {0xA1: "brick_red", 0xBB2: "metal_door"})
        tex = cm.Texture("common", "unnamed", 0xA1, 256, 256, 54)
        tex.pairs[1] = [10, 100, 0x00FF0126, 0]
        other = cm.Texture("common", "unnamed", 0xC3, 256, 256, 54)
        other.pairs[1] = [5, 50, 0x00FF0126, 0]
        named = cm.Texture("vehicles", "rim_main", 0xA1, 64, 64, 18)  # a named texture keeps its own name even when the list has its hash
        named.pairs[1] = [1, 10, 0x00FF0126, 0]
        rows = cm.records({1: tex, 2: other, 3: named}, None, names)
        by_hash = {(r["hash"], r["dictionary"]): r for r in rows}
        r = by_hash[("%016X" % 0xA1, "common")]
        self.assertEqual((r["name"], r["key"]), ("brick_red", "hash:%016X" % 0xA1))  # named for the rules, keyed by its hash
        self.assertEqual(by_hash[("%016X" % 0xC3, "common")]["name"], "unnamed")
        self.assertEqual(by_hash[("%016X" % 0xA1, "vehicles")]["name"], "rim_main")
        # without the list nothing changes
        plain = cm.records({1: tex}, None, None)
        self.assertEqual(plain[0]["name"], "unnamed")

    def test_a_named_by_alias_texture_reaches_the_name_rules_and_is_keyed_by_hash(self):
        rows = [census_row("brick_red", "xCityDefault", dictionary="common", hash_="00000000000000A1")]
        rows[0]["key"] = "hash:00000000000000A1"  # what census_merge writes for an alias
        table, _review = classify(*rows)
        self.assertEqual(list(table), ["hash:00000000000000A1"])
        self.assertEqual(table["hash:00000000000000A1"][0]["smooth"], smooth_byte(0.90))


class Emitter(unittest.TestCase):
    def test_garage_floor_round_trips_as_class_16(self):
        from gbm_common import class_name, parse_class

        self.assertEqual(parse_class("garagefloor"), 16)
        self.assertEqual(parse_class("16"), 16)
        self.assertEqual(class_name(16), "garagefloor")
        self.assertIsNone(parse_class("17"))

    def test_fnv_is_the_renderers(self):
        self.assertEqual(er.fnv1a_lower(""), 0xCBF29CE484222325)
        self.assertEqual(er.fnv1a_lower("a"), 0xAF63DC4C8601EC8C)
        self.assertEqual(er.fnv1a_lower("foobar"), 0x85944171F73967E8)
        self.assertEqual(er.fnv1a_lower("FooBar"), er.fnv1a_lower("foobar"))
        self.assertEqual(er.pair_key("Shared", "Glass_01"), er.pair_key("shared", "glass_01"))
        self.assertNotEqual(er.pair_key("a", "bc"), er.pair_key("ab", "c"))
        self.assertNotEqual(er.pair_key("shared", "x"), er.name_key("x"))
        self.assertEqual(er.pair_key("d", "n"), er.fnv1a_lower("n", er.fnv1a_lower("/", er.fnv1a_lower("d"))))

    def sample_text(self):
        sources = [("sample_table.txt", er.read_rows(HERE / "sample_table.txt")), ("hand", er.read_rows(HERE / "sample_overrides.txt"))]
        return er.emit(sources, "sample (made up)"), sources

    def test_the_sample_is_the_committed_golden(self):
        text, _ = self.sample_text()
        golden = (HERE / "sample_rows.inc").read_text(encoding="utf-8")
        self.assertEqual(text, golden, "sample_rows.inc is out of date: python -I emit_rows.py --table sample_table.txt --overrides sample_overrides.txt --out sample_rows.inc --label \"sample (made up)\"")

    def test_shape_and_determinism(self):
        text, sources = self.sample_text()
        self.assertEqual(er.emit(sources, "sample (made up)"), text)
        self.assertEqual(er.emit(list(reversed(sources[:1])) + sources[1:], "sample (made up)"), text)  # one table only: the same
        lines = text.split("\n")
        self.assertEqual(lines[-1], "")
        for name in ("kCompiledHashRows", "kCompiledPairRows", "kCompiledNameRows"):
            self.assertIn("inline constexpr CompiledRow %s[] = {" % name, text)
        self.assertEqual(text.count("{0, {}},  // unused sentinel"), 3)
        # the keys of each array strictly increase
        import re
        for block in re.findall(r"CompiledRow \w+\[\] = \{(.*?)\n\};", text, re.S):
            keys = [int(k, 16) for k in re.findall(r"\{0x([0-9A-F]{16})ull,", block)]
            self.assertEqual(keys, sorted(set(keys)))
        self.assertNotIn("\r", text)
        self.assertNotRegex(text, r"\d{4}-\d\d-\d\d")  # no date
        self.assertNotIn(str(HERE), text)  # no path of the machine

    def test_an_override_replaces_the_row_whole(self):
        text, _ = self.sample_text()
        self.assertIn("shared/glass_storefront_01 rough=0.02", text)
        self.assertIn("{2, 0, 250, 0, 255, false}", text)  # only the smoothness field: every other field the default of TextureRow{}
        self.assertNotIn("class=glass rough=0.05 ssr=1  shared 256x256 xCityDefault 3000 draws", text)

    def test_row_initializer_defaults(self):
        self.assertEqual(er.row_initializer({"class": 9, "smooth": 242, "metal": 255, "ssr": True, "lit": 0}), "{31, 9, 242, 255, 0, true}")
        self.assertEqual(er.row_initializer({"lit": 0}), "{16, 0, 0, 0, 0, false}")
        self.assertEqual(er.row_initializer({"smooth": 7}), "{2, 0, 7, 0, 255, false}")

    def test_keys_of_every_kind(self):
        rows = er.read_rows(HERE / "sample_table.txt")
        text = er.emit([("t", rows + [(("any", "Manhole"), {"class": 10}, "")])], "x")
        self.assertIn("{0x%016Xull, {1, 10, 0, 0, 255, false}},  // */Manhole class=metal" % er.name_key("manhole"), text)
        self.assertIn("0x%016Xull" % er.pair_key("shared", "puddle_decal"), text)

    def test_two_keys_with_one_number_are_refused(self):
        real = er.fnv1a_lower
        try:
            er.fnv1a_lower = lambda text, h=er.FNV_OFFSET: 7
            rows = [(("pair", "a", "b"), {"class": 1}, ""), (("any", "c"), {"class": 1}, ""), (("pair", "d", "e"), {"class": 1}, "")]
            with self.assertRaises(er.EmitError):
                er.emit([("t", rows)])
        finally:
            er.fnv1a_lower = real

    def test_the_sentinel_number_is_refused(self):
        real = er.fnv1a_lower
        try:
            er.fnv1a_lower = lambda text, h=er.FNV_OFFSET: 0
            with self.assertRaises(er.EmitError):
                er.emit([("t", [(("any", "c"), {"class": 1}, "")])])
        finally:
            er.fnv1a_lower = real

    def test_bad_rows_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.txt"
            for text in ("common/unnamed class=glass\n", "hash:0 class=glass\n", "x/y class=nothing\n", "@version 2\nx/y class=glass\n", "x/y\n"):
                bad.write_text(text, encoding="utf-8")
                with self.assertRaises(er.EmitError, msg=text):
                    er.read_rows(bad)

    def test_an_empty_table_is_three_sentinels(self):
        text = er.emit([("t", [])], "")
        self.assertEqual(text.count("{0, {}}"), 3)
        self.assertIn("0 rows: 0 by hash, 0 by dictionary/name, 0 by name", text)

    def test_the_tool_never_writes_its_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            table = tmp / "table.txt"
            hand = tmp / "hand.txt"
            table.write_text((HERE / "sample_table.txt").read_text(encoding="utf-8"), encoding="utf-8")
            hand.write_text((HERE / "sample_overrides.txt").read_text(encoding="utf-8"), encoding="utf-8")
            before = (table.read_bytes(), hand.read_bytes(), os.stat(table).st_mtime_ns, os.stat(hand).st_mtime_ns)
            out = tmp / "rows.inc"
            self.assertEqual(er.main(["--table", str(table), "--overrides", str(hand), "--out", str(out)]), 0)
            self.assertTrue(out.exists())
            # an --out that is an input is refused, and nothing is written to it
            self.assertEqual(er.main(["--table", str(table), "--overrides", str(hand), "--out", str(hand)]), 2)
            self.assertEqual(er.main(["--table", str(table), "--overrides", str(hand), "--out", str(table)]), 2)
            after = (table.read_bytes(), hand.read_bytes(), os.stat(table).st_mtime_ns, os.stat(hand).st_mtime_ns)
            self.assertEqual(before, after)

    def test_the_committed_overrides_file_parses_and_the_tool_reads_it(self):
        path = HERE / "table_overrides.txt"
        rows = er.read_rows(path)  # EmitError for any bad line
        text = er.emit([("hand", rows)], "")
        self.assertIn("inline constexpr uint32_t kCompiledRowsFormat = 1;", text)

    def test_the_shipped_table_is_what_the_emitter_makes(self):
        # src/gpu/gbuffer_texture_rows.inc is the emitter's output for the rows it holds: when the committed file has rows, regenerating from its own comments is not possible, so only its shape is held
        text = (HERE.parents[1] / "src" / "gpu" / "gbuffer_texture_rows.inc").read_text(encoding="utf-8")
        self.assertIn("inline constexpr CompiledRow kCompiledHashRows[] = {", text)
        self.assertEqual(text.count("{0, {}},"), 3)
        import re
        for block in re.findall(r"CompiledRow \w+\[\] = \{(.*?)\n\};", text, re.S):
            keys = [int(k, 16) for k in re.findall(r"\{0x([0-9A-F]{16})ull,", block)]
            self.assertEqual(keys, sorted(set(keys)))


if __name__ == "__main__":
    unittest.main(verbosity=1)
