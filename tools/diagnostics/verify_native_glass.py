"""Compare real single-pass glass pixels with original-shader, native-depth GPU references."""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from native_pixel_delta import RAW, read_native_pixels


PIXEL_SHADER = "b9f4a2c34e11ece75a7bca31c9be4c12bf2a9913c55dc45af4b0c639886a4ce8"
ABSOLUTE_HDR_TOLERANCE = .001


def require(condition, message):
    if not condition:
        raise ValueError(message)


def native_hdr_extent(manifest):
    portrait = manifest.get("headset_extent_2688x2880", False)
    require(type(portrait) is bool, "Native HDR headset extent must be explicitly boolean")
    if not manifest.get("recommended_resolution"):
        require(not portrait, "Headset HDR extent requires the recommended-resolution fixture")
        return 7680, 2160
    declared = manifest.get("diagnostic_eye_size", 1024)
    if portrait:
        require(declared is None, "Headset HDR extent conflicts with a declared square eye")
        return 5376, 2880
    require(type(declared) is int and declared in (1024, 2448),
            "Unsupported recommended native HDR extent")
    return declared * 2, declared


def compare(before, actual, reference):
    require(before.shape == actual.shape == reference.shape and before.ndim == 3 and
            before.shape[2] == 4 and before.shape[1] % 2 == 0, "Invalid full-atlas HDR extent")
    values = [value[:, :, :3].astype(np.float32) for value in (before, actual, reference)]
    require(all(np.isfinite(value).all() for value in values), "Nonfinite native glass RGB")
    first, delivered, expected = values
    error = np.abs(delivered - expected)
    require(float(error.max()) <= ABSOLUTE_HDR_TOLERANCE,
            f"Native glass differs from original-shader reference: max HDR error {error.max()}")
    half = before.shape[1] // 2
    actual_changed = np.any(np.abs(delivered - first) > 1e-5, axis=2)
    reference_changed = np.any(np.abs(expected - first) > 1e-5, axis=2)
    return {
        "rgb_channel_comparisons": int(error.size),
        "maximum_hdr_error": float(error.max()),
        "actual_changed_pixels": [int(actual_changed[:, eye * half:(eye + 1) * half].sum())
                                  for eye in range(2)],
        "reference_changed_pixels": [int(reference_changed[:, eye * half:(eye + 1) * half].sum())
                                     for eye in range(2)],
        "different_half_float_rgb_channels": int(np.count_nonzero(delivered != expected)),
    }


def select_sweep_frames(frames, fixture):
    require(fixture in (1, 2), "Unknown moving-head fixture")
    markers = set(range(600, 701, 20)) if fixture == 1 else set(range(320, 421, 20))
    key = "native_frame" if fixture == 1 else "gameplay_frame"
    frames = list(frames)
    projected = [frame for frame in frames if not frame.get("canceled")]
    require(projected and all(frame["frame"] < projected[0]["frame"]
                              for frame in frames if frame.get("canceled")),
            "Window fixture canceled a projected gameplay frame")
    require(all(key in frame and "native_frame" in frame for frame in projected),
            "Projected window frame lacks its actual sample markers")
    selected = [frame for frame in projected if frame[key] in markers]
    require(len(selected) == 6 and {frame[key] for frame in selected} == markers,
            "Missing or duplicate native moving-head sample markers")
    return selected, {frame["native_frame"] for frame in selected}


def sweep_poses(frames, expected_frames, fixture=1):
    require(fixture in (1, 2, 3), "Unknown native sweep fixture")
    poses = {}
    for frame in frames:
        native = frame["native_frame"]
        if native not in expected_frames:
            continue
        require(native not in poses and frame["window_sweep_fixture_version"] == fixture,
                "Missing or duplicate versioned native sweep frame")
        world = frame["gameplay_frame"]
        require(300 <= world < 450, "Glass sweep missed its actual side-facing gameplay phase")
        phase = world - 300
        yaw = (0 if fixture == 3 else math.pi / 2) + math.radians(8) * math.sin(phase * .05)
        pitch = math.radians(-15) + math.radians(3) * math.sin(phase * .07)
        sy, cy = math.sin(yaw / 2), math.cos(yaw / 2)
        sp, cp = math.sin(pitch / 2), math.cos(pitch / 2)
        expected = [cy * sp, sy * cp, -sy * sp, cy * cp]
        require(len(frame["views"]) == 2, "Sweep did not submit both real XR views")
        for view in frame["views"]:
            actual = view["orientation"]
            require(len(actual) == 4 and all(math.isfinite(x) for x in actual) and
                    max(abs(a - b) for a, b in zip(actual, expected)) < 3e-6,
                    "Actual submitted XR orientation differs from the moving-head fixture")
        poses[native] = {"gameplay_frame": world, "yaw_degrees": math.degrees(yaw),
                         "pitch_degrees": math.degrees(pitch)}
    require(set(poses) == expected_frames, "Native XR evidence omits a required sweep sample")
    require(max(x["yaw_degrees"] for x in poses.values()) -
            min(x["yaw_degrees"] for x in poses.values()) >= 8,
            "Window fixture did not exercise substantial actual head rotation")
    return poses


def verify(root, module, probe, selftest=False):
    manifest = json.loads((root / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    version = manifest["glass_reference_fixture_version"]
    require(manifest["glass_reference"] and version in (2, 3, 4, 5) and
            manifest["head_turn"] and not manifest["body"] and not manifest["physical_camcorder"],
            "Not the explicit window-visible original-glass fixture")
    sweep = version in (3, 4, 5)
    require(bool(manifest.get("window_sweep", False)) == sweep,
            "Glass reference and moving-head fixture versions disagree")
    expected_frames = {manifest["draw_frame"]}
    poses, pose_controls = {}, 0
    if sweep:
        fixture = 2 if version == 5 else 1
        require(manifest["draw_frame"] == 600 and manifest["window_sweep_fixture_version"] == fixture and
                manifest["head_turn_pitch_degrees"] == -15, "Wrong moving-head window fixture")
        with (root / "frames.jsonl").open() as stream:
            frames, expected_frames = select_sweep_frames((json.loads(line) for line in stream), fixture)
        poses = sweep_poses(frames, expected_frames, fixture)
        if selftest:
            wrong_pose = copy.deepcopy(frames)
            wrong_pose[0]["views"][0]["orientation"] = [0, 0, 0, 1]
            wrong_version = copy.deepcopy(frames)
            wrong_version[0]["window_sweep_fixture_version"] = 0
            stationary = [dict(copy.deepcopy(frames[0]), native_frame=frame)
                          for frame in sorted(expected_frames)]
            for corrupted in (wrong_pose, wrong_version, frames[:-1], frames + frames[:1], stationary):
                try:
                    sweep_poses(corrupted, expected_frames, fixture)
                except ValueError:
                    pose_controls += 1
                    continue
                raise ValueError("Sweep oracle accepted changed, missing, duplicate or stationary XR poses")
    for key, path in (("mod_sha256", module), ("probe_sha256", probe)):
        require(hashlib.sha256(path.read_bytes()).hexdigest().upper() == manifest[key],
                f"Wrong actual native binary: {key}")
    require((root / "capture-complete.txt").is_file(), "Native capture did not finish")
    folder = root / "native-glass-reference"
    records = [json.loads(line) for line in (folder / "references.jsonl").read_text().splitlines()]
    require(records, "No actual native window glass matched")
    results, controls, native_controls, equivalent_controls = [], 0, 0, 0
    for index, record in enumerate(records):
        require(record["sample"] == index and record["frame"] in expected_frames and
                record["original_retail_shaders"] and record["native_depth_retained"] and
                record["native_depth_and_stencil_read_only"] and record["tracked_native_world_scissor"] and
                record["diagnostic_reference_draws"] == 2 and
                record["diagnostic_only_control_draws"] == (4 if version >= 4 else 2),
                "Original native shader/depth reference provenance missing")
        if version == 5:
            require(record["gameplay_frame"] == poses[record["frame"]]["gameplay_frame"],
                    "Glass pixels and submitted moving-head pose belong to different gameplay frames")
        require(record["pixel_sha256"] == PIXEL_SHADER, "Not the attributed retail window glass program")
        prefix = record["prefix"]
        require(prefix == f"sample_{index}", "Invalid glass reference file identity")
        for stage, signature in (("vertex", b"\x00\x03\xfe\xff"), ("pixel", b"\x00\x03\xff\xff")):
            code = (folder / f"{prefix}_{stage}.bin").read_bytes()
            require(code.startswith(signature) and
                    hashlib.sha256(code).hexdigest() == record[f"{stage}_sha256"],
                    "Original retail shader bytes changed")
        images = []
        width, height = record["width"], record["height"]
        require((width, height) == native_hdr_extent(manifest),
                "Actual glass target differs from its explicit native HDR extent")
        for name in ("before", "after", "reference", "legacy_scissor_control"):
            payload = read_native_pixels(folder, prefix, name, record.get("pixel_storage", RAW),
                                         width * height * 8)
            require(len(payload) == width * height * 8 and
                    hashlib.sha256(payload).hexdigest() == record[f"{name}_sha256"],
                    f"Changed or truncated full native glass pixels: {name}")
            images.append(np.frombuffer(payload, dtype="<f2").reshape(height, width, 4))
        before, after, expected, legacy = images
        result = compare(before, after, expected)
        results.append(dict(sample=index, frame=record["frame"], **result))
        if max(result["actual_changed_pixels"]) >= 128:
            try:
                compare(before, expected, legacy)
            except ValueError:
                try:
                    compare(before, after, legacy)
                except ValueError:
                    native_controls += 1
                else:
                    raise ValueError("Glass oracle accepted the actually rendered wrong-scissor GPU control")
            else:
                equivalent_controls += 1
        if selftest and max(result["actual_changed_pixels"]) >= 128:
            faults = [
                (after, before),
                (before, expected),
                (after, expected + np.float16(.025)),
                (after, np.full_like(expected, np.nan)),
            ]
            for changed_actual, changed_expected in faults:
                try:
                    compare(before, changed_actual, changed_expected)
                except ValueError:
                    controls += 1
                    continue
                raise ValueError("Glass oracle accepted an omitted, altered or nonfinite material")
    actual_coverage = [sum(x["actual_changed_pixels"][eye] for x in results) for eye in range(2)]
    reference_coverage = [sum(x["reference_changed_pixels"][eye] for x in results) for eye in range(2)]
    require(min(actual_coverage + reference_coverage) >= 128, "No substantial visible glass in both real eyes")
    require({x["frame"] for x in results} == expected_frames,
            "Original-shader reference omits a required native frame")
    if sweep:
        for frame in expected_frames:
            coverage = [sum(x["actual_changed_pixels"][eye] for x in results if x["frame"] == frame)
                        for eye in range(2)]
            require(min(coverage) >= 128, f"No substantial native window glass at sweep frame {frame}")
    require(native_controls, "Missing real original-shader wrong-scissor counterexample")
    if selftest:
        require(controls >= 4, "No visible native sample exercised the glass corruption controls")
    return {
        "module_sha256": manifest["mod_sha256"],
        "probe_sha256": manifest["probe_sha256"],
        "original_shader_glass_acceptance": True,
        "sampled_moving_head_shader_parity": sweep,
        "submitted_sweep_poses": poses,
        "moving_head_pose_controls": pose_controls,
        "temporal_window_flicker_acceptance": False,
        "physical_headset_acceptance": False,
        "absolute_hdr_tolerance": ABSOLUTE_HDR_TOLERANCE,
        "corruption_controls": controls,
        "actual_native_wrong_scissor_controls_rejected": native_controls,
        "equivalent_legacy_scissor_controls_not_counterexamples": equivalent_controls,
        "samples": results,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.root, args.module, args.probe, args.selftest), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f"FAIL: {error}\n")
