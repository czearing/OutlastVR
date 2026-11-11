"""Independently replay completed native poses and verify rendered locomotion correction."""
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path

import numpy as np

from verify_native_camera_gait import analyze_records as analyze_gait
from verify_native_camera_gait import unique_records, vector
from verify_native_probe import require


EPSILON = float(np.finfo(np.float32).eps)


def rotate(value, yaw, inverse=False):
    angle = (int(yaw) & 65535) * math.tau / 65536
    if inverse:
        angle = -angle
    c, s = math.cos(angle), math.sin(angle)
    return np.array([c * value[0] - s * value[1],
                     s * value[0] + c * value[1], value[2]])


def rounding_bound(*values, operations=8):
    return operations * EPSILON * np.maximum(1, np.max(np.abs(values), axis=0))


def native_origin_bound(a, b):
    return (16 * EPSILON / (1 - 16 * EPSILON)) * max(1, np.sum(np.abs(a)), np.sum(np.abs(b)))


def analyze_records(manifest, frames, cameras, poses, updates, events):
    lean = manifest.get("lean", False)
    version = manifest.get("gait_schedule_version", 1)
    require(type(version) is int and version in (1, 2, 3), "Unsupported native gait schedule version")
    release = {1: 180, 2: 120, 3: 80}[version]
    schedule = [(60, 87, True), (release, 87, False), (300, 162, True), (420, 162, False)]
    if lean:
        schedule += [(500, 81, True), (560, 81, False), (650, 69, True), (710, 69, False)]
    require(events == [dict(gameplay_frame=f, key=k, down=d) for f, k, d in schedule],
            "Native locomotion input schedule differs")
    gait = analyze_gait(dict(manifest, lean=False), frames, cameras, poses,
                        [event for event in events if event["key"] in (87, 162)])
    pose_by_serial = unique_records(poses, "serial")
    completed = [update for update in updates if update["camera_pose_serial"]]
    update_by_serial = unique_records(completed, "camera_pose_serial")
    order = list(update_by_serial)
    require(order == sorted(order), "Native camera completions are not in causal order")
    positions = {serial: i for i, serial in enumerate(order)}
    expected = {}
    anchor, candidate = None, None
    for update in completed:
        serial = update["camera_pose_serial"]
        require(serial in pose_by_serial, "Completed native camera has no base sample")
        sample = pose_by_serial[serial]
        require(update["camera"] == sample["camera"] and sample["camera"] > 0 and sample["hero"] > 0,
                "Completed native camera identity differs")
        root = vector(sample["root"], 3).astype(np.float32).astype(float)
        bone = vector(sample["base_pose"], 6).astype(np.float32).astype(float)[:3]
        final = vector(update["final_pose"], 6).astype(np.float32).astype(float)[:3]
        intent, weight = sample["lean_intent"], sample["lean_weight"]
        require(np.isfinite([intent, weight]).all(), "Nonfinite native lean state")
        observed = sample["lean_weight_observed"]
        require(type(observed) is bool, "Invalid native lean observation flag")
        crouched = bool(sample["stance_region_0x2f80"][2] & 1)
        key = (sample["camera"], sample["hero"], crouched)
        yaw = sample["root_rotator"][1]
        normal = ((sample["basic_state"], sample["special_state"], sample["camera_mode"]) == (0, 0, 0)
                  and not (sample["camera_flags"] & 4) and intent == 0)
        supported = normal and observed and abs(weight) <= 8 * EPSILON
        local = rotate(bone - root, yaw, inverse=True)
        correction = np.zeros(3)
        if supported:
            if anchor is None or anchor[0] != key:
                anchor = (key, candidate[1] if candidate is not None and candidate[0] == key else local)
            candidate = None
            correction = root + rotate(anchor[1], yaw) - bone
        else:
            anchor = None
            if normal and not crouched and not observed:
                if candidate is None or candidate[0] != key:
                    candidate = (key, local)
            else:
                candidate = None
        expected[serial] = dict(root=root, bone=bone, final=final, correction=correction,
                                supported=supported, intent=intent, weight=weight, crouched=crouched,
                                normal=normal, lean_observed=observed)

    cameras = unique_records([camera for camera in cameras if not camera["world_panel"]], "native_frame")
    world = [frame for frame in frames if not frame.get("canceled") and not frame["world_panel"]]
    rows = []
    for frame in world:
        require(bool(frame["lean_probe"]) == lean, "Native lean capture scope differs")
        camera = cameras[frame["native_frame"]]
        source, watermark = camera["camera_source_serial"], camera["camera_completed_serial"]
        require(source in expected and watermark in positions and camera["camera_pose_serial"] in pose_by_serial,
                "Missing completed native source or history watermark")
        require(source <= watermark <= camera["camera_pose_serial"],
                "Future native pose was used before its camera completed")
        history = order[max(0, positions[watermark] - 63):positions[watermark] + 1]
        origin = vector(camera["origin"], 3).astype(np.float32).astype(float)
        matches = []
        for serial in reversed(history):
            difference = origin - expected[serial]["final"]
            if np.all(np.abs(difference) <= native_origin_bound(origin, expected[serial]["final"])):
                matches.append((float(difference @ difference), -serial, serial))
        require(matches and min(matches)[2] == source,
                "Renderer source was not its closest causally completed native camera")
        state = expected[source]
        require(type(camera["camera_stabilized"]) is bool and camera["camera_stabilized"] == state["supported"],
                "Native correction bypass/eligibility differs")
        tracked = vector(camera["tracking_origin"], 3)
        target = origin + state["correction"]
        require(np.all(np.abs(tracked - target) <= rounding_bound(origin, target, state["root"])),
                "Tracked origin violates independent native root/bone correction")
        rows.append(dict(**state, tracked=tracked, origin=origin))

    phases = {"standing": (30, 55), "walking": (70, 175),
              "crouched": (350, 410), "standing_recovered": (750, 850)}
    measurements = {}
    for name, (lo, hi) in phases.items():
        group = rows[lo - 1:hi]
        tail_prefix = 0
        if lean and name == "standing_recovered":
            while (group and not group[0]["supported"] and group[0]["normal"] and
                   group[0]["lean_observed"] and group[0]["intent"] == 0 and
                   8 * EPSILON < abs(group[0]["weight"]) < .1):
                group = group[1:]
                tail_prefix += 1
        require(len(group) >= 20, f"Insufficient settled native {name} coverage")
        require(all(row["supported"] for row in group), f"Native stabilization did not cover steady {name}")
        native = np.array([row["origin"][2] - row["root"][2] for row in group])
        tracked = np.array([row["tracked"][2] - row["root"][2] -
                            (row["origin"][2] - row["bone"][2]) for row in group])
        require(abs(float(np.median(native) - np.median(tracked))) < 1,
                f"Native {name} anchor calibrated to a transient or wrong stance height")
        tolerance = max(float(rounding_bound(row["origin"], row["root"])[2]) for row in group)
        stances = [list(stance) for _, stance in itertools.groupby(group, key=lambda row: row["crouched"])]
        require(max(map(len, stances)) >= 20, f"No sustained native stance during {name}")
        stance_ranges = []
        for stance in stances:
            heights = [row["tracked"][2] - row["root"][2] -
                       (row["origin"][2] - row["bone"][2]) for row in stance]
            residual = float(np.ptp(heights))
            require(residual <= 2 * tolerance, f"Animated positional bob remains during {name}")
            stance_ranges.append(dict(crouched=stance[0]["crouched"], views=len(stance),
                                      tracked_bone_height_range=residual))
        measurements[name] = dict(native_height_median=float(np.median(native)),
                                  tracked_bone_height_median=float(np.median(tracked)),
                                  native_height_range=float(np.ptp(native)),
                                  tracked_bone_height_range=float(np.ptp(tracked)),
                                  native_stance_transitions=len(stances) - 1,
                                  native_release_tail_prefix_views=tail_prefix,
                                  steady_stances=stance_ranges)
    require(measurements["walking"]["native_height_range"] >= .5,
            "Native capture did not exercise measurable walking translation bob")
    require(sum(row["supported"] for row in rows) >= (600 if lean else 800),
            "Insufficient actual supported native locomotion coverage")
    lean_rows = [row for row in rows if abs(row["weight"]) >= .1]
    tails = [row for row in rows if row["intent"] == 0 and 8 * EPSILON < abs(row["weight"]) < .1]
    if lean:
        require(len(lean_rows) >= 20 and len(tails) >= 3,
                "Native lean or its real release tail was not exercised")
        neutral_y = np.median([row["bone"][1] - row["root"][1] for row in rows[749:850]])
        require(max(abs(row["bone"][1] - row["root"][1] - neutral_y) for row in lean_rows) >= 20,
                "Posted lean input did not move the actual native camera")
    return dict(status="pass", binary_identity=manifest, native_gait=gait,
                gameplay_projections=len(rows), stabilized_views=sum(row["supported"] for row in rows),
                completed_pose_association_verified=True, positional_bob_removed=True,
                native_root_terrain_stance_preserved=True, phases=measurements,
                observed_left_lean=any(row["weight"] <= -.1 for row in lean_rows),
                observed_right_lean=any(row["weight"] >= .1 for row in lean_rows),
                preserved_lean_release_tail_views=len(tails),
                comfortable_play_certified=False, stereo_images_accepted=False,
                limitations="Verifies actual native gait, tracked translation and completed-pose history; "
                            "does not certify physical-headset comfort, stereo images or headset cadence.")


def analyze(evidence):
    require((evidence / "capture-complete.txt").is_file(), "Native locomotion capture did not complete")
    manifest = json.loads((evidence / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    for filename, field in (("OutlastVRSinglePass.dll", "mod_sha256"),
                            ("OutlastVRHeadlessProbe.dll", "probe_sha256"),
                            ("OutlastVRLauncher.exe", "launcher_sha256")):
        require(hashlib.sha256((evidence / filename).read_bytes()).hexdigest().upper() ==
                manifest[field].upper(), f"Retained {filename} differs from capture")

    def records(filename):
        return [json.loads(line) for line in (evidence / filename).read_text(encoding="utf-8-sig").splitlines()]

    return analyze_records(manifest, records("frames.jsonl"), records("native-cameras.jsonl"),
                           records("camera-poses.jsonl"), records("camera-updates.jsonl"),
                           records("gait-input-events.jsonl"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    report = analyze(args.evidence)
    (args.evidence / "native-locomotion-proof.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: native completed-pose correction, root/terrain travel, crouch and locomotion bob")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError) as error:
        raise SystemExit(f"FAIL: {error}") from error
