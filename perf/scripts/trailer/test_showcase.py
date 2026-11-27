"""Exercise both wipes and capture evidence with synthetic plates. No game is involved."""

import csv
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import render_showcase as render
import wipe_showcase as wipe


def write_png(path, size, color):
    width, height = size

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    rows = (b"\0" + bytes(color) * width) * height
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


COLORS = ((230, 0), (0, 230), (230, 230))


def write_fixture(root):
    shots = []
    for number, count in enumerate((27, 30)):
        name = f"fixture-{number}"
        folders = {}
        for preset, color in zip(wipe.LABELS, COLORS, strict=True):
            relative = Path(name) / preset
            folder = root / relative / "frames"
            folder.mkdir(parents=True)
            for frame in range(count):
                write_png(folder / f"f{frame:06d}.png", (80, 46), (*color, number * 96 + frame * 3))
            folders[preset] = str(relative)
        shots.append(dict(id=name, frames=count, plates=folders))
    manifest = root / "showcase.json"
    manifest.write_text(json.dumps(dict(version=1, fps=30, presets=list(wipe.LABELS),
                                        wipe_starts=[.1, .5], wipe_duration=.2, shots=shots)), encoding="utf-8")
    return manifest


class ShowcaseTest(unittest.TestCase):
    def test_wipes_labels_and_cut(self):
        ffmpeg = shutil.which("ffmpeg")
        font = Path(os.environ["WINDIR"]) / "Fonts/segoeuib.ttf"
        with tempfile.TemporaryDirectory(prefix="showcase-test-") as directory:
            root = Path(directory)
            manifest = write_fixture(root)
            out = root / "edit"
            width, height = 640, 360
            wipe.compose(manifest, out, ffmpeg, font, (width, height), (320, 180))
            samples = (0, 5, 6, 11, 17, 18, 24, 26, 27, 32, 51, 56)
            select = "select='" + "+".join(f"eq(n,{frame})" for frame in samples) + "'"
            data = subprocess.check_output([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(out / wipe.MASTER),
                                            "-vf", select, "-fps_mode", "passthrough",
                                            "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"])
            self.assertEqual(len(data), width * height * 3 * len(samples))

            def pixel(frame, x, y):
                start = ((samples.index(frame) * height + y) * width + x) * 3
                return tuple(data[start:start + 3])

            for frame, left, right in [(0, 0, 0), (5, 1, 0), (6, 1, 0), (11, 1, 1), (17, 2, 1),
                                       (18, 2, 1), (24, 2, 2), (26, 2, 2), (27, 0, 0),
                                       (32, 1, 0), (51, 2, 2), (56, 2, 2)]:
                blue = frame * 3 if frame < 27 else 96 + (frame - 27) * 3
                self.assertEqual(pixel(frame, width // 4, 250), (*COLORS[left], blue), frame)
                self.assertEqual(pixel(frame, width * 3 // 4, 250), (*COLORS[right], blue), frame)

            # The box width tracks the label switch at each wipe midpoint.
            def box_width(frame):
                return sum(pixel(frame, x, 16) != pixel(frame, x, 150) for x in range(16, 110))

            self.assertEqual(box_width(0), box_width(5))
            self.assertEqual(box_width(6), box_width(17))
            self.assertGreater(box_width(6), box_width(5))
            self.assertGreater(box_width(18), box_width(17))
            self.assertEqual(box_width(27), box_width(0))
            self.assertTrue((out / wipe.PREVIEW).is_file())

            (root / "fixture-0/enhanced/frames/f000005.png").unlink()
            with self.assertRaisesRegex(ValueError, "contiguous PNGs"):
                wipe.read_manifest(manifest)

    def test_plate_evidence(self):
        with tempfile.TemporaryDirectory(prefix="showcase-test-") as directory:
            root = Path(directory)
            (root / "frames").mkdir()
            # A solid frame is sufficient to test this validator's dimensions and metadata.
            write_png(root / "frames/f000000.png", render.SIZE, (1, 2, 3))
            report = dict(state="complete", sink={"kind": "png"},
                          picture=dict(width=3840, height=2160, fps=30), frames=dict(exact=True, count=1, lost=0))
            (root / "take.json").write_text(json.dumps(report), encoding="utf-8")
            (root / "frames.csv").write_text("frame,scene_t,update,lost\n0,0,543,0\n", encoding="ascii")
            fields = "frame,scene_t,update,world_ready,paused,hour,x,y,z".split(",")
            state = dict(zip(fields, "0,0,543,1,0,23,10,10,10".split(","), strict=True))
            with (root / "state.csv").open("w", newline="", encoding="ascii") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                writer.writerow(state)
            first = render.validate_plate(root, 1, 30)
            self.assertAlmostEqual(render.compare_states(first, [dict(state, x="10.09")])["position_m_per_axis"], .09)
            for change in (dict(x="10.11"), dict(scene_t=".000002"), dict(hour="23.00002")):
                with self.assertRaisesRegex(ValueError, "diverged at frame 0"):
                    render.compare_states(first, [state | change])
            (root / "frames.csv").write_text("frame,scene_t,update,lost\n0,0,543,1\n", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "invalid frame 0"):
                render.validate_plate(root, 1, 30)


if __name__ == "__main__":
    unittest.main()
