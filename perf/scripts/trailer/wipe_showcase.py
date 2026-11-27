"""Composite synchronized PNG plates into an SDR editing master and a preview."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


LABELS = {"vanilla": "VANILLA", "enhanced": "ENHANCED", "enhanced_rt": "ENHANCED RT"}
MASTER = "showcase-master-4k.mkv"
PREVIEW = "showcase-preview-1080p.mp4"


def frame_at(seconds, fps):
    frame = round(seconds * fps)
    if abs(frame - seconds * fps) > 1e-6:
        raise ValueError(f"{seconds} s is not on the {fps} FPS frame grid")
    return frame


def filter_graph(fps, starts, duration, master_size):
    start_frames = [frame_at(t, fps) for t in starts]
    wipe_frames = frame_at(duration, fps)
    switches = [frame + (wipe_frames + 1) // 2 for frame in start_frames]
    chains = ["[0:v]format=gbrp,setsar=1[v0]"]
    for index, start in enumerate(start_frames, 1):
        # xfade's second input starts at the wipe. Use that frame from the
        # incoming plate, so both inputs still show the same scene time.
        chains.append(f"[{index}:v]trim=start_frame={start},setpts=PTS-STARTPTS,format=gbrp,setsar=1[v{index}]")
    for index, offset in enumerate(starts, 1):
        first = "v0" if index == 1 else "wipe1"
        chains.append(f"[{first}][v{index}]xfade=transition=wiperight:duration={duration}:offset={offset}[wipe{index}]")
    width, height = master_size
    filters = [f"scale={width}:{height}:flags=lanczos", "setsar=1"]
    windows = (f"lt(n,{switches[0]})", f"gte(n,{switches[0]})*lt(n,{switches[1]})", f"gte(n,{switches[1]})")
    for label, window in zip(LABELS.values(), windows, strict=True):
        filters.append(f"drawtext=fontfile=typeface.ttf:text='{label}':fontcolor=white:fontsize=h*0.024:"
                       f"box=1:boxcolor=black@0.82:boxborderw={max(2, round(height * .006))}:"
                       f"x=w*0.035:y=h*0.05:enable='{window}'")
    chains.append("[wipe2]" + ",".join(filters) + "[video]")
    return ";".join(chains)


def encode(ffmpeg, cwd, count, args):
    result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-n",
                             "-progress", "pipe:1", "-nostats", *args],
                            cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f"ffmpeg failed ({result.returncode}): {result.stderr.strip()}")
    frames = re.findall(r"(?m)^frame=\s*(\d+)\s*$", result.stdout)
    if not frames or int(frames[-1]) != count:
        raise ValueError(f"expected {count} output frames, got {frames[-1] if frames else 'none'}")


def read_manifest(path):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest["version"] != 1 or manifest["presets"] != list(LABELS) or not manifest["shots"]:
        raise ValueError("expected a showcase manifest with three ordered presets and one or more shots")
    fps, starts, duration = manifest["fps"], manifest["wipe_starts"], manifest["wipe_duration"]
    if len(starts) != 2 or duration <= 0 or not 0 < starts[0] < starts[0] + duration <= starts[1]:
        raise ValueError("expected two ordered, non-overlapping wipes after the first frame")
    end = frame_at(starts[1] + duration, fps)
    root = path.parent.resolve()
    clips, names = [], set()
    for shot in manifest["shots"]:
        name, count = shot["id"], shot["frames"]
        if name in names or count <= end:
            raise ValueError(f"{name}: duplicate shot or second wipe reaches the cut")
        names.add(name)
        folders = [(root / shot["plates"][preset]).resolve() / "frames" for preset in LABELS]
        if len(set(folders)) != len(LABELS) or any(not folder.is_relative_to(root) for folder in folders):
            raise ValueError(f"{name}: plates must have separate folders inside the render")
        for folder in folders:
            expected = {f"f{frame:06d}.png" for frame in range(count)}
            if {image.name for image in folder.glob("f*.png")} != expected:
                raise ValueError(f"{folder}: expected {count} contiguous PNGs starting at frame 0")
        clips.append((name, count, folders))
    return fps, starts, duration, clips


def compose(manifest, out, ffmpeg, font, master_size=(3840, 2160), preview_size=(1920, 1080)):
    fps, starts, duration, clips = read_manifest(manifest)
    graph = filter_graph(fps, starts, duration, master_size)
    out.mkdir(parents=True, exist_ok=True)
    if any((out / name).exists() for name in (MASTER, PREVIEW)):
        raise ValueError(f"{out}: choose an empty output folder for the master and preview")
    with tempfile.TemporaryDirectory(prefix=".showcase-", dir=out) as scratch:
        stage = Path(scratch)
        shutil.copyfile(font, stage / "typeface.ttf")
        segments = []
        total = 0
        for index, (name, count, folders) in enumerate(clips):
            print(f"composite {name}: {count} frames", flush=True)
            inputs = []
            for folder in folders:
                inputs += ["-threads", "2", "-framerate", str(fps), "-start_number", "0",
                           "-i", str(folder / "f%06d.png")]
            segment = f"shot-{index:02d}.mkv"
            encode(ffmpeg, stage, count, [*inputs, "-filter_complex_threads", "4", "-filter_complex", graph,
                                          "-map", "[video]", "-frames:v", str(count), "-an", "-c:v", "ffv1",
                                          "-threads", "4", "-level", "3", "-slices", "16", "-pix_fmt", "bgr0",
                                          "-color_range", "pc", "-color_trc", "iec61966-2-1", "-color_primaries", "bt709",
                                          "-colorspace", "rgb", segment])
            segments.append(f"file '{segment}'\nduration {count / fps:.9f}\n")
            total += count
        (stage / "shots.ffconcat").write_text("ffconcat version 1.0\n" + "".join(segments), encoding="ascii")
        encode(ffmpeg, stage, total, ["-f", "concat", "-safe", "1", "-i", "shots.ffconcat",
                                     "-map", "0:v:0", "-c", "copy", MASTER])
        width, height = preview_size
        encode(ffmpeg, stage, total, ["-i", MASTER, "-filter_threads", "4", "-vf",
                                     f"scale={width}:{height}:flags=lanczos:out_color_matrix=bt709:out_range=tv,format=yuv420p",
                                     "-an", "-c:v", "libx264", "-threads", "4", "-preset", "medium", "-crf", "17",
                                     "-color_range", "tv", "-colorspace", "bt709", "-color_primaries", "bt709",
                                     "-color_trc", "iec61966-2-1", "-movflags", "+faststart", PREVIEW])
        for name in (MASTER, PREVIEW):
            (stage / name).rename(out / name)
    print(f"{total} frames at {fps} FPS\n{out / MASTER}\n{out / PREVIEW}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--font", type=Path, required=True)
    args = parser.parse_args()
    try:
        compose(args.manifest.resolve(), args.out.resolve(), args.ffmpeg, args.font.resolve())
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"showcase composite failed: {error}\n")


if __name__ == "__main__":
    main()
