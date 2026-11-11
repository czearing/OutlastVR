"""Verify real native gait, preserved crouching and upright tracking, not comfort or positional-bob removal."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from verify_native_probe import require


def vector(value, size):
    result = np.asarray(value, dtype=float)
    require(result.shape == (size,) and np.isfinite(result).all(),
            "Invalid native camera vector")
    return result


def unique_records(records, key):
    result = {}
    for record in records:
        identity = record[key]
        require(type(identity) is int and identity > 0 and identity not in result,
                f"Invalid or duplicate {key}")
        result[identity] = record
    return result


def analyze_records(manifest, frames, cameras, poses, events):
    require(manifest["scenario"] == "Continue" and manifest["gait"] and not manifest["head_turn"]
            and not manifest.get("lean", False),
            "Capture does not select isolated native gait")
    version = manifest.get("gait_schedule_version", 1)
    require(type(version) is int and version in (1, 2, 3), "Unsupported native gait schedule version")
    expected_events = [
        dict(gameplay_frame=60, key=87, down=True),
        dict(gameplay_frame={1: 180, 2: 120, 3: 80}[version], key=87, down=False),
        dict(gameplay_frame=300, key=162, down=True),
        dict(gameplay_frame=420, key=162, down=False),
    ]
    require(events == expected_events, "Native gait input schedule differs")
    world = [frame for frame in frames if not frame.get("canceled") and not frame["world_panel"]]
    require(len(world) >= 900, "Fewer than 900 native gameplay projections")
    native_frames = unique_records(world, "native_frame")
    cameras = unique_records(
        [camera for camera in cameras if camera["native_frame"] in native_frames], "native_frame")
    poses = unique_records(poses, "serial")
    rows = []
    for frame in world:
        require(frame["gait_probe"] and not frame["head_turn_probe"],
                "A gameplay projection did not use isolated gait")
        require(frame.get("gait_schedule_version", 1) == version, "Native gait schedule metadata differs")
        require(frame["native_frame"] in cameras, "Missing linked renderer camera")
        camera = cameras[frame["native_frame"]]
        require(not camera["world_panel"], "Linked renderer camera belongs to a menu")
        serial = (camera["camera_source_serial"] if "camera_source_serial" in camera
                  else camera["camera_pose_serial"])
        require(serial in poses, "Missing linked native camera pose")
        pose = poses[serial]
        progress_serial = (camera["camera_completed_serial"] if "camera_completed_serial" in camera
                           else camera["camera_pose_serial"])
        require(progress_serial in poses and serial <= progress_serial <= camera["camera_pose_serial"],
                "Future native pose or missing camera update watermark")
        up = vector(camera["tracking_up"], 3)
        backward = vector(camera["tracking_backward"], 3)
        require(np.max(np.abs(up - [0, 0, 1])) < 1e-4 and abs(backward[2]) < 1e-4,
                "Animated native pitch or roll entered the tracked gravity basis")
        require(abs(np.linalg.norm(backward) - 1) < 1e-4,
                "Tracked native heading is not unit length")
        root = vector(pose["root"], 3)
        base = vector(pose["base_pose"], 6)
        origin = vector(camera["origin"], 3)
        rows.append(dict(serial=progress_serial, root=root, base=base, origin=origin,
                         state=(pose["basic_state"], pose["special_state"], pose["camera_mode"]),
                         native_height=base[2] - root[2], render_height=origin[2] - root[2]))

    phases = {"standing": (30, 55), "walking": (55, 190),
              "crouched": (350, 410), "standing_recovered": (600, 850)}
    selected = {name: rows[lo - 1:hi] for name, (lo, hi) in phases.items()}
    for name, group in selected.items():
        require(len({row["serial"] for row in group}) >= 10,
                f"Native camera updates stopped during {name}")
        require(all(row["state"] == (0, 0, 0) for row in group),
                f"Steady {name} entered an unsupported native camera state")
    walking = selected["walking"]
    displacement = walking[-1]["root"] - walking[0]["root"]
    require(np.linalg.norm(displacement[:2]) >= 100,
            "Queued walking did not produce at least one meter of native pawn travel")
    if version >= 2:
        require(np.linalg.norm(displacement[:2]) <= 500,
                "Controlled native gait exceeded the clear-corridor fixture travel")
    heights = {}
    for name in ("standing", "crouched", "standing_recovered"):
        group = selected[name]
        heights[name] = float(np.median([row["native_height"] for row in group]))
        require(max(abs(row["native_height"] - row["render_height"]) for row in group) < 1,
                f"Renderer camera did not preserve the native {name} height")
    require(heights["standing"] - heights["crouched"] >= 30,
            "Queued crouching did not change the actual native camera stance")
    require(abs(heights["standing"] - heights["standing_recovered"]) < 10,
            "Native standing height did not recover after crouch release")
    angle_range = np.ptp(np.array([row["base"][4:] for row in rows]), axis=0)
    require(max(angle_range) >= .2,
            "Native gait did not exercise animated camera pitch or roll")
    return dict(
        status="pass", binary_identity=manifest, gameplay_projections=len(world),
        native_pose_observations=len(poses),
        native_horizontal_pawn_displacement_units=float(np.linalg.norm(displacement[:2])),
        native_terrain_height_change_units=float(displacement[2]),
        native_stance_heights_units=heights,
        native_pitch_roll_ranges_degrees=angle_range.tolist(),
        upright_tracking_preserved=True, native_crouch_preserved=True,
        positional_bob_removed=None, comfortable_play_certified=False,
        limitations="Tests actual native walking, crouch/recovery, camera-update progression and world-Z "
                    "tracked gravity. Tracked translation is graded separately; "
                    "this does not certify removal of positional bob, stereo image quality, physical "
                    "headset comfort or frame pacing.")


def analyze(evidence):
    require((evidence / "capture-complete.txt").is_file(), "Native gait capture did not complete")
    manifest = json.loads((evidence / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    for filename, field in (("OutlastVRSinglePass.dll", "mod_sha256"),
                            ("OutlastVRHeadlessProbe.dll", "probe_sha256"),
                            ("OutlastVRLauncher.exe", "launcher_sha256")):
        digest = hashlib.sha256((evidence / filename).read_bytes()).hexdigest()
        require(digest.upper() == manifest[field].upper(), f"Retained {filename} differs from capture")

    def records(filename):
        with (evidence / filename).open(encoding="utf-8-sig") as stream:
            return [json.loads(line) for line in stream]

    return analyze_records(
        manifest, records("frames.jsonl"), records("native-cameras.jsonl"),
        records("camera-poses.jsonl"), records("gait-input-events.jsonl"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    report = analyze(args.evidence)
    (args.evidence / "native-camera-gait-proof.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: actual native walking, crouch/recovery and upright tracked gravity; "
          "positional bob and headset comfort remain unaccepted")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError) as error:
        raise SystemExit(f"FAIL: {error}") from error
