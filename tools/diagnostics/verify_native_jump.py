import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path

import numpy as np

from verify_native_body import load_rows, match_gpu_pose, matrix, require


def load(directory):
    return {
        name: load_rows(directory / (name + ".jsonl"))
        for name in ("native-cameras", "camera-poses", "camera-updates", "native-skeleton",
                     "native-input-events", "body-tracking-events")
    } | {"frames": [row for row in load_rows(directory / "frames.jsonl") if not row.get("canceled")],
         "draws": [row for row in load_rows(directory / "draw-probe" / "draws.jsonl") if "draw" in row]}


def grade(data, fault=""):
    poses = {row["serial"]: row for row in data["camera-poses"]}
    updates = {row["camera_pose_serial"]: row for row in data["camera-updates"]}
    frames = {row["frame"]: row["gameplay_frame"] for row in data["frames"]}
    require(all(row["jump_probe"] and row["jump_schedule_version"] == 2 for row in data["frames"]),
            "Actual native frame trace lacks the explicit revised jump fixture")
    inputs = {row["xr_time"]: row for row in data["native-input-events"] if row["kind"] == "motion_pad"}
    input_frames = {row["frame"] * 11111111: row["gameplay_frame"] for row in data["frames"]}
    rising, startup_confirmations = [], []
    pressed = False
    for row in sorted(inputs.values(), key=lambda row: row["xr_time"]):
        current = bool(row["buttons"] & 0x1000)
        if current and not pressed:
            require(row["xr_time"] in input_frames,
                    "Native jump rising edge has no actual projected XR frame")
            if input_frames[row["xr_time"]] > 0:
                rising.append(row)
            else:
                startup_confirmations.append(row)
        pressed = current
    require(len(rising) == 4 and all(row["focused"] and row["right_grip_tracked"] for row in rising),
            "Four actual native jump-button rising edges were not delivered")
    require([input_frames[row["xr_time"]] for row in rising] == [180, 360, 540, 800],
            "Native gameplay jump rising edges do not match their actual scheduled XR frames")
    rendered = []
    for camera in data["native-cameras"]:
        world = frames.get(camera["native_frame"], 0)
        if world < 30:
            continue
        serial = camera["camera_source_serial"]
        require(serial in poses and serial in updates and
                serial <= min(camera["camera_pose_serial"], camera["camera_completed_serial"]) and
                camera["camera_completed_serial"] - serial < 64 and
                np.max(np.abs(np.array(camera["origin"]) - updates[serial]["final_pose"][:3])) < .03,
                "Rendered jump camera lost its exact completed native source pose")
        source = poses[serial]
        eyes = np.array(camera.get("rendered_eyes", []), dtype=float)
        require(eyes.shape == (2, 32) and np.isfinite(eyes).all(),
                "Jump proof lacks the exact finite rendered stereo eye constants")
        head = np.mean(eyes[:, 16:19] - eyes[:, 20:23], axis=0)
        rendered.append((world, source, head, camera))
    require(len(rendered) >= 800, "Insufficient actual rendered jump/landing observations")
    roots = np.array([row[1]["root"] for row in rendered])
    require(np.ptp(roots[:, 0]) >= 250, "Jump fixture did not exercise actual native body travel")
    windows = ((180, 320), (360, 490), (540, 650), (800, 890))
    episodes = []
    for start, end in windows:
        before = [row for row in rendered if start - 15 <= row[0] < start]
        active = [row for row in rendered if start <= row[0] < end]
        require(len(before) >= 10 and len(active) >= 60, "Native jump episode is missing baseline or landing samples")
        require(all((row[1]["basic_state"] in (0, 1) and row[1]["special_state"] == 0) or
                    (row[1]["basic_state"] == 2 and row[1]["special_state"] == 3)
                    for row in active), "Jump fixture entered an unrelated contextual animation")
        baseline = float(np.median([row[1]["root"][2] for row in before]))
        peak = max(active, key=lambda row: row[1]["root"][2])
        height = peak[1]["root"][2] - baseline
        specials = sum(row[1]["basic_state"] == 2 and row[1]["special_state"] == 3 for row in active)
        if start in (360, 800):
            require(height >= 30, "Delivered running jump did not produce real native vertical root travel")
            elevated = [row for row in active if row[1]["root"][2] > baseline + 15]
            errors = []
            for _, source, head, camera in elevated:
                if fault == "frozen-head":
                    head = head.copy()
                    head[2] = baseline + 170
                elif fault == "wrong-height":
                    head = head - [0, 0, 30]
                errors.append(abs(head[2] - source["root"][2] - 170))
                require(camera["camera_stabilized"], "Running jump dropped the anchored headset camera")
            require(len(errors) >= 8 and max(errors) < 4 and np.percentile(errors, 90) < 2,
                    "Actually rendered headset did not follow the airborne native root at measured standing height")
        else:
            require(specials >= 15, "In-place jump did not enter the actual native basic2/special3 animation")
            errors = []
        landing = active[-10:]
        require(all(row[1]["basic_state"] in (0, 1) and not row[1]["special_state"] for row in landing) and
                abs(np.median([row[1]["root"][2] for row in landing]) - baseline) < 5,
                "Native jump did not return to ordinary grounded locomotion")
        episodes.append({"gameplay_frame": start, "native_root_rise": float(height),
                         "native_special3_samples": specials,
                         "maximum_airborne_eye_height_error": max(errors) if errors else None})
    skeletons = data["native-skeleton"]
    active = [row for row in skeletons if row["targets"]]
    require(len(active) >= 800, "Jump proof lacks actual native body evaluations")
    reference_row = next(row for row in skeletons if row["reference_records"])
    records = np.array([[struct.unpack("<f", struct.pack("<I", word))[0] for word in record]
                        for record in reference_row["reference_records"]])
    reference = []
    for index, record in enumerate(records):
        local = matrix(record[4:11])
        reference.append(reference[reference_row["parents"][index]] @ local if index else local)
    palms = [records[list(fingers), 8:11].mean(axis=0) * .5
             for fingers in ((16, 19, 22, 25), (40, 43, 46, 49))]
    hand_errors = []
    restored = lost = 0
    for row in active:
        target = row["targets"]
        world = np.array(row["world"]).reshape(4, 4).T
        basis = np.array([target["right"], target["up"], target["backward"]]).T
        for side, hand in enumerate(target["hands"]):
            bone = (12, 36)[side]
            if not hand["tracked"]:
                expected_fallback = world @ matrix(row["before_atoms"][bone])
                expected_fallback[:3, 3] += np.array(target["up"]) * (
                    target["standing_height_delta"] * target["units_per_meter"])
                actual_fallback = world @ matrix(row["atoms"][bone])
                if fault == "stale-untracked":
                    actual_fallback[:3, 3] += [30, 0, 0]
                require(np.max(np.abs(actual_fallback - expected_fallback)) < .003,
                        "Untracked native hand retained a stale controlled wrist")
                lost += 1
                continue
            require(target["grip_anchored"], "Native jump hand abandoned metacarpal grip calibration")
            wrist = matrix(row["atoms"][bone])
            if fault == "stale-hand":
                wrist[:3, 3] += [30, 0, 0]
            actual = (world @ wrist @ np.r_[palms[side], 1])[:3]
            expected = np.array(target["origin"]) + basis @ np.array(hand["position"]) * target["units_per_meter"]
            hand_errors.append(float(np.linalg.norm(actual - expected)))
            restored += side == 0 and row["clock_us"] > active[-100]["clock_us"]
    require(hand_errors and max(hand_errors) < .03 and lost >= 20 and restored >= 50,
            "Native jump/travel hand continuity or tracking-loss recovery failed")
    draws = data["draws"]
    gpu_fault = {"wrong-gpu": "--fault-gpu", "stale-gpu": "--fault-stale-gpu"}.get(fault, "")
    gpu, matches, coverage, changed, _, clock = match_gpu_pose(
        skeletons, data["native-cameras"], poses, np.linalg.inv(reference), draws,
        {3, 12, 36, 63, 69}, {12, 36}, gpu_fault)
    require(gpu["basic"] == 2 and gpu["special"] == 3,
            "Captured GPU skinning did not occur during an actual native in-place jump")
    return {"episodes": episodes, "startup_only_confirmations": len(startup_confirmations),
            "native_gameplay_jump_rising_frames": [input_frames[row["xr_time"]] for row in rising],
            "native_render_observations": len(rendered),
            "native_body_evaluations": len(active), "native_x_travel": float(np.ptp(roots[:, 0])),
            "maximum_reconstructed_palm_error": max(hand_errors), "untracked_native_wrist_fallbacks": lost,
            "gpu_frame": draws[0]["native_frame"], "gpu_native_pose_serial": gpu["serial"],
            "gpu_pose_source_age_us": clock - gpu["clock_us"], "gpu_matched_draws": matches,
            "gpu_matched_bones": sorted(coverage), "gpu_changed_bones": sorted(changed)}


def main():
    parser = argparse.ArgumentParser(description="Grade actual native jump/root-follow and hand/GPU continuity.")
    parser.add_argument("directory", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    manifest = json.loads((args.directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require((args.directory / "capture-complete.txt").is_file() and manifest["jump"] and manifest["floor"] and
            manifest["body"] and manifest["jump_schedule_version"] == 2 and
            manifest["mod_sha256"] == hashlib.sha256(args.module.read_bytes()).hexdigest().upper(),
            "Native jump proof is incomplete or does not match the exact captured module")
    data = load(args.directory)
    result = grade(data)
    controls = []
    if args.selftest:
        for fault in ("frozen-head", "wrong-height", "stale-hand", "stale-untracked", "wrong-gpu", "stale-gpu",
                      "missing-eyes", "wrong-source", "uncompleted-source"):
            altered = copy.deepcopy(data) if fault in ("missing-eyes", "wrong-source") else data
            if fault == "missing-eyes":
                for camera in altered["native-cameras"]:
                    camera.pop("rendered_eyes", None)
            if fault == "wrong-source":
                for camera in altered["native-cameras"]:
                    camera["camera_source_serial"] = 1
            if fault == "uncompleted-source":
                for camera in altered["native-cameras"]:
                    camera["camera_completed_serial"] = camera["camera_source_serial"] - 1
            try:
                grade(altered, fault)
            except ValueError:
                controls.append(fault)
            else:
                raise ValueError("Native jump oracle accepted negative control: " + fault)
    result["rejected_controls"] = controls
    paths = ["native-cameras.jsonl", "camera-poses.jsonl", "camera-updates.jsonl",
             "native-skeleton.jsonl", "native-input-events.jsonl",
             "body-tracking-events.jsonl", "frames.jsonl", "draw-probe\\draws.jsonl", "native-run-manifest.json"]
    result["input_sha256"] = {name: hashlib.sha256((args.directory / name).read_bytes()).hexdigest() for name in paths}
    result["verifier_sha256"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in (Path(__file__), Path(__file__).with_name("verify_native_body.py"))}
    (args.directory / "native-jump-proof.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
