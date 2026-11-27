"""Render the ordered showcase plates through one leased Director session."""

import argparse
from contextlib import suppress
import csv
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SIZE = (3840, 2160)
TIME_TOLERANCE = .000001
POSE_TOLERANCE = .10
HOUR_TOLERANCE = .00001


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def png_size(path):
    with path.open("rb") as stream:
        header = stream.read(24)
    require(len(header) == 24 and header[:16] == b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR", f"bad PNG: {path}")
    return struct.unpack(">II", header[16:])


def smoke_picture(client, ffmpeg):
    shot = client.call("shot", path="showcase-4k-smoke.png", timeout_s=30)
    path = Path(shot["path"])
    require((shot["width"], shot["height"]) == SIZE and png_size(path) == SIZE,
            f"4K gate failed: game captured {shot['width']}x{shot['height']}")
    luma = subprocess.check_output([ffmpeg, "-v", "error", "-i", str(path), "-vf", "scale=32:18:flags=area",
                                    "-frames:v", "1", "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"], timeout=15)
    values = sorted(luma)
    pixels = 32 * 18
    require(len(values) == pixels and values[int(.98 * pixels)] - values[int(.02 * pixels)] > 4,
            "4K smoke image is blank or near-flat")


def validate_plate(folder, count, fps):
    report = json.loads((folder / "take.json").read_text(encoding="utf-8"))
    picture, frames = report["picture"], report["frames"]
    require(report["state"] == "complete" and report["sink"]["kind"] == "png"
            and (picture["width"], picture["height"], picture["fps"]) == (*SIZE, fps)
            and frames["exact"] and frames["count"] == count and frames["lost"] == 0,
            f"{folder}: expected {count} exact 4K{fps} PNG frames")
    timeline = read_csv(folder / "frames.csv")
    state = read_csv(folder / "state.csv")
    images = sorted((folder / "frames").glob("f*.png"))
    require(len(timeline) == len(state) == len(images) == count, f"{folder}: missing frame evidence")
    for index, (frame, world, image) in enumerate(zip(timeline, state, images, strict=True)):
        expected_t = index / fps
        require(frame["frame"] == world["frame"] == str(index) and frame["update"] == world["update"]
                and abs(float(frame["scene_t"]) - expected_t) <= TIME_TOLERANCE
                and abs(float(world["scene_t"]) - expected_t) <= TIME_TOLERANCE
                and frame["lost"] == "0" and world["world_ready"] == "1" and world["paused"] == "0"
                and image.name == f"f{index:06d}.png" and png_size(image) == SIZE,
                f"{folder}: invalid frame {index}")
    return state


def compare_states(reference, candidate):
    deltas = {"time_s": 0., "hour": 0., "position_m_per_axis": 0.}
    for index, (first, second) in enumerate(zip(reference, candidate, strict=True)):
        dt = abs(float(first["scene_t"]) - float(second["scene_t"]))
        dh = abs(float(first["hour"]) - float(second["hour"]))
        axes = [abs(float(first[axis]) - float(second[axis])) for axis in ("x", "y", "z")]
        require(dt <= TIME_TOLERANCE and dh <= HOUR_TOLERANCE
                and all(dx <= POSE_TOLERANCE for dx in axes), f"plate world diverged at frame {index}")
        deltas["time_s"] = max(deltas["time_s"], dt)
        deltas["hour"] = max(deltas["hour"], dh)
        deltas["position_m_per_axis"] = max(deltas["position_m_per_axis"], *axes)
    return deltas


def verify_camera(client, shot):
    camera = client.call("camera.get")
    if "rig" in shot:
        require(camera["valid"] and camera["mode"] == "game" and camera["rig"] == shot["rig"],
                f"{shot['id']}: cockpit camera did not apply")
    else:
        require(camera["valid"] and camera["mode"] == "scene", f"{shot['id']}: scene camera did not apply")
    return camera


def drive(client, config, run, ffmpeg):
    client.call("events.set", only=["take.done"])
    ready_until = time.monotonic() + 120
    while not client.call("transport.get")["world"]:
        require(time.monotonic() < ready_until, "free roam never became ready")
        time.sleep(.25)
    original_preset = client.call("preset.get")
    original_rig = client.call("camera.get")["rig"]
    require(original_preset["rt_available"], "ENHANCED RT is unavailable on the selected adapter")
    results = []
    fps, presets = config["fps"], config["presets"]
    for shot in config["shots"]:
        name, duration = shot["id"], shot["seconds"]
        count = duration * fps
        # One shot can run to this size if PNG compression saves little.
        needed = (SIZE[0] * 3 + 1024) * SIZE[1] * count * len(presets)
        require(shutil.disk_usage(run).free >= needed, f"{name}: needs {needed / 2**30:.1f} GiB free for its three PNG plates")
        scene = HERE / "scenes" / f"{name}.scene"
        client.call("scene.load", path=str(scene), wait="settled", timeout_s=100)
        if "rig" in shot:
            # // CHECK: The gameplay cockpit rig must survive all three Director take restarts.
            client.call("camera.rig", rig=shot["rig"], timeout_s=15)
        client.call("scene.freeze", t=0)
        client.call("scene.settle", frames=120, timeout_s=45)
        first_camera = verify_camera(client, shot)
        if not results:
            smoke_picture(client, ffmpeg)
        print(f"render {name}: {count} frames per preset", flush=True)
        client.call("take.render", name=name, fps=fps, sink="png", presets=presets, **{"from": 0, "to": duration})
        done = client.wait_event("take.done", pred=lambda event: event.get("name") == name, timeout=600)
        require(done.get("state") == "complete" and done.get("frames") == count, f"{name}: take failed: {done}")
        outputs = done["outputs"]
        folders = [Path(output["folder"]).resolve() for output in outputs]
        require([output["preset"] for output in outputs] == presets and len(set(folders)) == len(presets),
                f"{name}: expected separate plates in preset order")
        require(all(folder.parent == (run / "takes").resolve() for folder in folders),
                f"{name}: unexpected take folders {folders}")
        reference = validate_plate(folders[0], count, fps)
        alignment = {preset: compare_states(reference, validate_plate(folder, count, fps))
                     for preset, folder in zip(presets[1:], folders[1:], strict=True)}
        if "rig" in shot:
            verify_camera(client, shot)
        client.call("scene.unload")
        if "rig" in shot:
            client.call("camera.rig", rig=original_rig, timeout_s=15)
        restored = client.call("preset.get")
        require(restored["name"] == original_preset["name"] and restored["label"] == original_preset["label"],
                f"{name}: unloading did not restore the initial preset and label")
        results.append(dict(id=name, frames=count, camera_at_start=first_camera, alignment=alignment,
                            plates={preset: folder.relative_to(run).as_posix()
                                    for preset, folder in zip(presets, folders, strict=True)}))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--run-tag", required=True)
    parser.add_argument("--round", type=int, choices=(1, 2), required=True)
    parser.add_argument("--ffmpeg", required=True)
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    sys.path.insert(0, str(workspace / "ports/scripts"))
    from director_client import DirectorClient, DirectorError
    from scene_tool import Launched, ToolError, wait_info

    row = json.load(sys.stdin)
    config = json.loads((HERE / "showcase.json").read_text(encoding="utf-8"))
    run = workspace / "ports/run" / args.run_tag
    log = workspace / "perf/logs" / f"{args.run_tag}.log"
    try:
        require(not run.exists(), f"run already exists: {run}")
        require(Path(row["channel"]) == run / "director.json", "unexpected Director channel")
        for shot in config["shots"]:
            require((HERE / "scenes" / f"{shot['id']}.scene").is_file(), f"missing scene {shot['id']}")
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("xb") as stream:
            proc = subprocess.Popen(row["argv"], cwd=workspace, stdout=stream, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        launch = Launched(proc, args.run_tag, run, Path(row["channel"]), row["argv"], log)
        try:
            wait_info(launch, 120)
            with DirectorClient.from_info(launch.info_path, name="trailer-showcase") as client:
                try:
                    shots = drive(client, config, run, args.ffmpeg)
                finally:
                    with suppress(DirectorError, OSError):
                        client.call("quit", timeout_s=10)
            require(proc.wait(timeout=45) == 0, "game or lockrun failed")
            summary = json.loads((run / "summary.json").read_text(encoding="utf-8"))
            require(summary.get("start_ok") and summary.get("exit_code") == 0, "invalid game summary")
            manifest = dict(version=1, fps=config["fps"], size=list(SIZE), presets=config["presets"],
                            wipe_starts=config["wipe_starts"], wipe_duration=config["wipe_duration"],
                            round=args.round, shots=shots)
            (run / "showcase.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        finally:
            if launch.alive():
                launch.kill_tree()
                proc.wait(timeout=10)
    except (OSError, ValueError, KeyError, ToolError, DirectorError, subprocess.SubprocessError) as error:
        print(f"showcase render failed: {error}. Launch log: {log}", file=sys.stderr)
        return 1
    print(run / "showcase.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
