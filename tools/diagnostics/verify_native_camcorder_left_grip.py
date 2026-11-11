"""Independently verify anatomically mirrored retail camera fingers on the native left hand."""
import argparse
import bisect
import hashlib
import json
import struct
from pathlib import Path

import numpy as np

from verify_native_body import matrix, require
from verify_native_camcorder_grip import grade_gpu_palette
from verify_native_camcorder_mesh import read_rows
from verify_native_probe import rotation


def grade(directory, module, bodies, meshes, events, fault=None, expected_mode=None):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require(manifest.get("mirrored_camera_grip_version") == 1 and
            manifest["mod_sha256"] == hashlib.sha256(module.read_bytes()).hexdigest().upper() and
            (directory / "capture-complete.txt").is_file(),
            "Left camera grip lacks its completed exact-module native fixture")
    reference_row = next(row for row in bodies.values() if row["reference_records"])
    reference = [struct.unpack("<20f", struct.pack("<20I", *words))
                 for words in reference_row["reference_records"]]
    mirror = np.diag([1.0, 1.0, -1.0])
    if fault == "wrong-reflection":
        mirror = np.diag([1.0, -1.0, 1.0])
    for left in range(13, 28):
        right = left + 24
        parent = 12 if (left - 13) % 3 == 0 else left - 1
        require(reference_row["parents"][left] == parent and
                reference_row["parents"][right] == parent + 24 and
                np.linalg.norm(np.array(reference[left][8:11]) -
                               mirror @ np.array(reference[right][8:11])) < .0001 and
                np.max(np.abs(rotation(reference[left][4:8]) -
                              mirror @ rotation(reference[right][4:8]) @ mirror)) < .00002,
                "Actual reference fingers do not establish the claimed anatomical correspondence")
    times = [event["clock"] for event in events]
    counts, errors, positions, curl_changes = {}, [], [], []
    untracked_samples = 0
    for mesh in meshes:
        if mesh["source"] != "body_post_ik" or mesh["hand"] != 0:
            continue
        body = bodies[mesh["native_body_serial"]]
        index = bisect.bisect_right(times, body["clock_us"]) - 1
        if index < 0 or not 0 <= body["clock_us"] - times[index] < 100000:
            continue
        event = events[index]
        if event["native_state"] != 1:
            continue
        if not body["targets"]["hands"][0]["tracked"]:
            require(not mesh["controller_tracked"] and
                    not body["targets"].get("mirrored_camera_grip"),
                    "Lost tracking still applied a controlled left finger pose")
            untracked_samples += 1
            continue
        require(event["held"] and event["hand"] == 0 and mesh["bone"] == 12 and
                event["native_mode"] in (0, 1) and
                event["night_vision"] == (event["native_mode"] == 1) and
                body["targets"]["camera_hand"] == 0 and
                body["targets"].get("mirrored_camera_grip") and
                body["targets"]["hands"][0]["tracked"] and fault != "untracked",
                "Anatomical grip was not delivered on the actual tracked held left camera")
        mode = "night_vision" if event["native_mode"] == 1 else "normal"
        counts[mode] = counts.get(mode, 0) + 1
        before, after = body["before_atoms"], body["atoms"]
        for left in range(13, 28):
            right = left + 24
            lp, rp = body["parents"][left], body["parents"][right]
            right_local = rotation(before[rp][:4]).T @ rotation(before[right][:4])
            delta = right_local @ rotation(reference[right][4:8]).T
            expected = mirror @ delta @ mirror @ rotation(reference[left][4:8])
            old_left = rotation(before[lp][:4]).T @ rotation(before[left][:4])
            actual = rotation(after[lp][:4]).T @ rotation(after[left][:4])
            if fault == "unchanged-left":
                actual = old_left
            elif fault == "copied-right":
                actual = right_local
            errors.append(float(np.max(np.abs(actual - expected))))
            curl_changes.append(float(np.max(np.abs(old_left - expected))))
            original_local = np.linalg.inv(matrix(before[lp])) @ matrix(before[left])
            delivered_local = np.linalg.inv(matrix(after[lp])) @ matrix(after[left])
            if fault == "stretched-finger":
                delivered_local[0, 3] += 1
            positions.append(float(np.linalg.norm(original_local[:3, 3] - delivered_local[:3, 3])))
            require(abs(after[left][7] / after[lp][7] - before[left][7] / before[lp][7]) < .00002,
                    "Camera reflection changed native left finger scale")
    modes = (expected_mode,) if expected_mode else ("normal", "night_vision")
    require(min(counts.get(mode, 0) for mode in modes) >= 20 and
            curl_changes and max(curl_changes) > .1,
            "Left grip lacks meaningful changed native finger animation in both camera modes")
    require(max(errors) < .00002 and max(positions) < .0005,
            "Native left camera fingers do not match the anatomical stock-animation mirror")
    return dict(native_left_anatomical_camera_grip_acceptance=True,
                required_native_modes=list(modes),
                native_left_raised_samples=counts, left_joint_comparisons=len(errors),
                maximum_local_rotation_error=max(errors),
                maximum_native_local_translation_error=max(positions),
                maximum_required_change_from_original_left_pose=max(curl_changes),
                explicitly_untracked_native_fallback_samples=untracked_samples,
                physical_headset_acceptance=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--gpu-palette", action="store_true")
    parser.add_argument("--gpu-mode", choices=("normal", "night_vision"))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.gpu_mode and not args.gpu_palette:
        parser.error("--gpu-mode requires --gpu-palette")
    try:
        bodies = {row["serial"]: row for row in read_rows(args.evidence / "native-skeleton.jsonl")}
        meshes = read_rows(args.evidence / "native-camera-mesh.jsonl")
        events = read_rows(args.evidence / "physical-camcorder-events.jsonl")
        result = grade(args.evidence, args.module, bodies, meshes, events, expected_mode=args.gpu_mode)
        if args.gpu_palette:
            result.update(grade_gpu_palette(args.evidence, args.module, bodies, meshes, events,
                                           expected_mode=args.gpu_mode, expected_hand=0))
        if args.selftest:
            faults = ("wrong-reflection", "unchanged-left", "copied-right", "stretched-finger", "untracked")
            for fault in faults:
                try:
                    grade(args.evidence, args.module, bodies, meshes, events, fault,
                          expected_mode=args.gpu_mode)
                except ValueError:
                    continue
                raise ValueError(f"Left anatomical grip accepted corruption: {fault}")
            result["rejected_left_grip_controls"] = list(faults)
        print(json.dumps(result, indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f"FAIL: {error}\n")
