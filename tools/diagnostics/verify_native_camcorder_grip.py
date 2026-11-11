"""Verify the stock right grip, optionally binding it to actual normal/NV GPU palettes."""
import argparse
import bisect
import copy
import hashlib
import json
from pathlib import Path
import struct

import numpy as np

from verify_native_body import load_rows, match_gpu_pose, matrix
from verify_native_camcorder_mesh import grade as mesh_grade, read_rows
from verify_native_probe import require, rotation


def atom(value):
    result = np.eye(4)
    result[:3, :3] = rotation(value[:4]).T * value[7]
    result[3, :3] = value[4:7]
    return result


def grade(directory, module, bodies, meshes, events, fault=None):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require(manifest["mod_sha256"] == hashlib.sha256(module.read_bytes()).hexdigest().upper() and
            manifest["camera_mount_version"] == 2 and fault != "module",
            "Stock grip does not match its exact native module and authored attachment")
    times = [r["clock"] for r in events]
    counts, errors, translations = {}, [], []
    unconfirmed, transitioning = 0, 0
    for row in meshes:
        if row["source"] != "body_post_ik" or row["hand"] != 1:
            continue
        body = bodies[row["native_body_serial"]]
        index = bisect.bisect_right(times, body["clock_us"]) - 1
        if index < 0 or body["clock_us"] - times[index] >= 100000:
            unconfirmed += 1
            continue
        event = events[index]
        if event["native_state"] != 1:
            transitioning += 1
            continue
        require(event["held"] and event["hand"] == 1 and row["bone"] == 52 and
                body["parents"][52] == 36 and body["targets"]["hands"][1]["tracked"],
                "Stock grip lost its native right wrist, aux socket or tracked held state")
        require(any(b["name"] == "Hero-R-Hand" and b["index"] == 36 and b["matrix_error"] == 0
                    for b in body["bindings"]), "Native stock wrist observation is missing")
        mode = "night_vision" if event["native_mode"] == 1 else "normal"
        require(event["night_vision"] == (mode == "night_vision"),
                "Native grip state does not match the real camera NV mode")
        counts[mode] = counts.get(mode, 0) + 1
        world = np.array(body["world"]).reshape(4, 4)
        original = np.array(row["stock_original_local"]).reshape(4, 4)
        source_camera = original @ atom(body["before_atoms"][52]) @ world
        delivered_camera = np.array(row["world"]).reshape(4, 4)
        for bone in range(37, 52):
            parent = bone
            seen = set()
            while parent != 36 and 0 <= parent < len(body["parents"]) and parent not in seen:
                seen.add(parent)
                parent = body["parents"][parent]
            require(parent == 36, "Stock finger is not an actual right-wrist descendant")
            before = atom(body["before_atoms"][bone]) @ world @ np.linalg.inv(source_camera)
            after = atom(body["atoms"][bone]) @ world @ np.linalg.inv(delivered_camera)
            errors.append(float(np.max(np.abs(before[:3, :3] - after[:3, :3]))))
            translations.append(float(np.linalg.norm(before[3, :3] - after[3, :3])))
    require(counts.get("normal", 0) >= 60 and counts.get("night_vision", 0) >= 20,
            "Missing stable raised normal/NV native right-hand grip observations")
    require(max(errors) < 2e-5 and max(translations) < .002,
            "Tracked right fingers changed their authored native relationship to the actual camera")
    return {"stock_right_animated_grip_acceptance": True, "left_stock_grip_acceptance": False,
            "gpu_finger_skinning_acceptance": False, "physical_headset_acceptance": False,
            "native_raised_samples": counts, "finger_transform_comparisons": len(errors),
            "excluded_unconfirmed_state_samples": unconfirmed, "excluded_transition_samples": transitioning,
            "max_relative_rotation_scale_error": max(errors),
            "max_relative_position_error_units": max(translations)}


def grade_gpu_palette(directory, module, bodies, meshes, events, fault=None, expected_mode=None,
                      expected_hand=1):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require(manifest["body"] and manifest["physical_camcorder"] and
            manifest["camera_mount_version"] == 2 and
            manifest["mod_sha256"] == hashlib.sha256(module.read_bytes()).hexdigest().upper(),
            "Camera GPU witness lacks its exact module and actual physical stock-camera fixture")
    skeletons = list(bodies.values())
    cameras = load_rows(directory / "native-cameras.jsonl")
    poses = {row["serial"]: row for row in load_rows(directory / "camera-poses.jsonl")}
    draws = [row for row in load_rows(directory / "draw-probe" / "draws.jsonl") if "draw" in row]
    require(draws and all(row["native_frame"] == manifest["draw_frame"] for row in draws),
            "Camera GPU draw frame does not match its native capture manifest")
    first = next(row for row in skeletons if row["reference_records"])
    reference = []
    for index, words in enumerate(first["reference_records"]):
        record = struct.unpack("<" + "f" * len(words), struct.pack("<" + "I" * len(words), *words))
        local = matrix(record[4:11])
        reference.append(reference[first["parents"][index]] @ local if index else local)
    inverse_reference = np.linalg.inv(reference)
    fingers = set(range(13, 28)) | set(range(37, 52))
    required = fingers | {3, 12, 36, 63, 69}
    wrist = 36 if expected_hand == 1 else 12

    if fault == "missing-right-finger":
        baseline, *_ = match_gpu_pose(skeletons, cameras, poses, inverse_reference, draws, required, {wrist})
        finger = 40 if expected_hand == 1 else 16
        expected = (matrix(baseline["atoms"][finger]) @ inverse_reference[finger])[:3, :]
        altered = 0
        for draw in draws:
            for constant in draw["vertex_constants"]:
                if constant["name"] == "BoneMatrices":
                    palette = np.array(constant["values"]).reshape(-1, 3, 4)
                    selected = np.max(np.abs(palette - expected), axis=(1, 2)) < .001
                    altered += int(selected.sum())
                    palette[selected, 0, 3] += 4
                    constant["values"] = palette.reshape(-1).tolist()
        require(altered > 0, "Missing-finger GPU control lacks an actual native finger matrix")
    elif fault == "missing-gpu":
        for draw in draws:
            draw["vertex_constants"] = [c for c in draw["vertex_constants"] if c["name"] != "BoneMatrices"]
    elif fault == "wrong-camera-source":
        for camera in cameras:
            if camera["native_frame"] == manifest["draw_frame"]:
                camera["camera_source_serial"] = max(poses) + 1
    elif fault == "wrong-gpu-frame":
        draws[0]["native_frame"] += 1
    pose, matched, coverage, changed, preceding, source_clock = match_gpu_pose(
        skeletons, cameras, poses, inverse_reference, draws, required, {wrist},
        "--fault-stale-gpu" if fault == "stale-gpu" else "")
    times = [row["clock"] for row in events]
    index = bisect.bisect_right(times, pose["clock_us"]) - 1
    require(index >= 0 and 0 <= pose["clock_us"] - times[index] < 100000,
            "Actual camera GPU pose has no fresh native camera-state observation")
    event = events[index]
    require(expected_hand != 0 or pose["targets"].get("mirrored_camera_grip"),
            "Actual left-held GPU witness lacks the anatomical stock-finger retarget")
    require(fault != "native-state" and event["held"] and event["hand"] == expected_hand and
            event["native_state"] == 1 and event["native_mode"] in (0, 1) and
            event["night_vision"] == (event["native_mode"] == 1) and
            pose["targets"]["hands"][expected_hand]["tracked"],
            "Actual GPU palette was not sampled with the tracked stock camera stably raised")
    mode = "night_vision" if event["native_mode"] == 1 else "normal"
    require(expected_mode is None or mode == expected_mode,
            f"Actual camera GPU mode is {mode}, not the required {expected_mode}")
    camera_meshes = [row for row in meshes if row["source"] == "body_post_ik" and row["hand"] == expected_hand and
                     row["native_body_serial"] == pose["serial"]]
    require(fault != "missing-camera-mesh" and len(camera_meshes) == 1 and
            camera_meshes[0]["bone"] == (52 if expected_hand == 1 else 12) and
            camera_meshes[0]["stock_mount_version"] == 2 and
            camera_meshes[0]["controller_tracked"],
            "Actual GPU pose lacks its same-evaluation authored native camera attachment")
    shaders = set()
    for draw in draws:
        if draw["draw"] not in matched:
            continue
        digest = draw["vertex"]
        program = bytearray((directory / "draw-probe" / (digest + ".bin")).read_bytes())
        if fault == "wrong-shader":
            program[0] ^= 1
        require(hashlib.sha256(program).hexdigest() == digest and program[:4] == b"\x00\x03\xfe\xff",
                "Actual camera GPU vertex program does not match its retained native bytecode")
        shaders.add(digest)
    return {
        "stock_right_grip_gpu_palette_acceptance": expected_hand == 1,
        "mirrored_left_grip_gpu_palette_acceptance": expected_hand == 0,
        "gpu_palette_hand": expected_hand,
        "gpu_palette_scope": "Actual draw constants and same-evaluation stock camera attachment; "
                             "not vertex-weight, rasterized-finger or physical acceptance",
        "gpu_palette_mode": mode,
        "gpu_palette_native_frame": draws[0]["native_frame"],
        "gpu_palette_native_body_serial": pose["serial"],
        "gpu_palette_pose_lag_evaluations": preceding[-1]["serial"] - pose["serial"],
        "gpu_palette_camera_source_age_us": source_clock - pose["clock_us"],
        "gpu_palette_native_state_age_us": pose["clock_us"] - times[index],
        "gpu_palette_matched_draws": matched,
        "gpu_palette_vertex_programs": sorted(shaders),
        "gpu_palette_right_fingers": sorted(coverage & set(range(37, 52))),
        "gpu_palette_left_fingers": sorted(coverage & set(range(13, 28))),
        "gpu_palette_changed_wrist_bones": sorted(changed & {12, 36}),
    }


def grade_fixture_timing(directory, fault=None):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    version = manifest["physical_camcorder_fixture_version"]
    require(manifest["physical_camcorder"] and version in (2, 3),
            "Camera timing proof requires an explicit paced physical-camera fixture")
    frames = []
    with (directory / "frames.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            row = json.loads(line)
            if not row.get("canceled") and row["gameplay_frame"] > 0:
                frames.append(row)
    if fault == "missing-frame":
        del frames[len(frames) // 2]
    elif fault == "wrong-version":
        frames[0]["physical_camcorder_fixture_version"] = 0
    elif fault == "too-fast":
        index = len(frames) // 2
        frames[index]["camcorder_frame_clock_ns"] = frames[index - 1]["camcorder_frame_clock_ns"] + 11111110
    require([row["gameplay_frame"] for row in frames] == list(range(1, 900)) and
            all(row["physical_camcorder_fixture_version"] == version and
                row["camcorder_frame_clock_ns"] > 0 for row in frames),
            "Camera timing proof lacks the complete versioned native gameplay sequence")
    gaps = [b["camcorder_frame_clock_ns"] - a["camcorder_frame_clock_ns"]
            for a, b in zip(frames, frames[1:])]
    require(min(gaps) >= 11111111, "Physical camera fixture ran faster than its actual90Hz animation/input clock")
    return {"physical_camcorder_fixture_version": version, "camcorder_paced_gameplay_steps": len(frames),
            "camcorder_minimum_actual_period_ns": min(gaps)}


def grade_controller_axes(bodies, meshes, fault=None):
    samples = {0: 0, 1: 0}
    errors = []
    for row in meshes:
        if row["source"] != "body_post_ik" or row["hand"] not in (0, 1) or not row["controller_tracked"]:
            continue
        hand = row["hand"]
        body = bodies[row["native_body_serial"]]
        target = body["targets"]["hands"][hand]
        require(body["targets"]["camera_hand"] == hand and target["tracked"] and
                row["controller_orientation"] == target["controller_orientation"],
                "Held-camera mesh is not associated with its actual same-evaluation controller")
        q = rotation(target["controller_orientation"])
        basis = np.array([row["tracking_right"], row["tracking_up"], row["tracking_backward"]]).T
        expected_forward = basis @ q[:, 1]
        expected_up = basis @ q[:, 2]
        if fault == "aim-not-grip":
            expected_forward = -(basis @ q[:, 2])
        elif fault == "backward-grip":
            expected_forward = -expected_forward
        elif fault == "up-axis":
            expected_up = basis @ q[:, 1]
        world = np.array(row["world"]).reshape(4, 4)
        forward, up = world[0, :3], world[2, :3]
        errors.extend([float(np.linalg.norm(forward / np.linalg.norm(forward) - expected_forward)),
                       float(np.linalg.norm(up / np.linalg.norm(up) - expected_up))])
        samples[hand] += 1
    require(min(samples.values()) >= 100, "Missing actual held-camera controller-axis observations in either hand")
    require(max(errors) < .001, "Native camera does not follow the OpenXR grip fist axis and grip-relative up")
    return {"native_controller_grip_axes_acceptance": True, "controller_grip_axis_samples": samples,
            "max_controller_grip_axis_error": max(errors)}


def grade_gc_continuity(directory, module, fault=None):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require((directory / "capture-complete.txt").is_file() and manifest["physical_camcorder"] and
            manifest["mod_sha256"] == hashlib.sha256(module.read_bytes()).hexdigest().upper(),
            "Held-GC proof does not match its completed exact-module physical-camera capture")
    lifecycle = read_rows(directory / "native-camcorder-lifecycle.jsonl")
    events = read_rows(directory / "physical-camcorder-events.jsonl")
    collections = [row for row in lifecycle if row.get("phase") == "before_gc" and
                   row.get("held") and row.get("requested") and row.get("rhi_texture")]
    if fault == "missing-gc":
        collections = []
    require(collections, "No actual requested native GC collected a live held-camera capture")
    proofs = []
    for collected in collections:
        generation = collected["generation"]
        require(any(row.get("phase") == "after_gc" and row.get("generation") == generation + 1 and
                    row["rhi_texture"] == 0 for row in lifecycle),
                "Held-GC proof did not clear native resource publication before rebuilding")
        before = [row for row in events if row["generation"] == generation]
        after = [row for row in events if row["generation"] == generation + 1]
        if fault == "new-generation":
            after = []
        require(before and after, "Held-GC proof lacks actual camera events on both native generations")
        previous, resumed = before[-1], after[0]
        if fault == "released-grip":
            resumed["held"] = False
        elif fault == "changed-hand":
            resumed["hand"] = 1 - previous["hand"]
        elif fault == "repeated-toggle":
            resumed["toggle"] = True
        elif fault == "missing-rebuild":
            for row in after:
                row["rhi_texture"] = 0
        require(previous["held"] and previous["hand"] in (0, 1) and
                previous["rhi_texture"] == collected["rhi_texture"] and
                resumed["held"] and resumed["hand"] == previous["hand"] and not resumed["toggle"] and
                previous["native_state"] in (1, 2) and resumed["native_state"] in (1, 2) and
                resumed["native_mode"] == previous["native_mode"] and
                0 < resumed["clock"] - previous["clock"] <= 250000,
                "Native GC discarded or changed the continuously held camera grip or repeated its raise")
        rebuilt = [row for row in after if row["held"] and row["hand"] == previous["hand"] and
                   row["native_mesh_ready"] and row["rhi_texture"] and
                   0 <= row["clock"] - resumed["clock"] <= 250000]
        require(rebuilt, "Held camera did not rebuild a completed native lens within the fresh-pose interval")
        proofs.append({"generation_before": generation, "generation_after": generation + 1,
                       "holding_hand": previous["hand"],
                       "native_state_before": previous["native_state"],
                       "native_state_after": resumed["native_state"],
                       "native_gc_gap_us": resumed["clock"] - previous["clock"],
                       "native_lens_rebuild_us": rebuilt[0]["clock"] - resumed["clock"]})
    return {"held_camera_gc_continuity_acceptance": True, "held_gc_samples": proofs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--gpu-palette", action="store_true",
                        help="Require an actual raised-camera GPU palette and same-evaluation attachment")
    parser.add_argument("--gc-continuity", action="store_true",
                        help="Require the same held grip across actual native GC and fresh resource rebuilding")
    parser.add_argument("--gpu-mode", choices=("normal", "night_vision"),
                        help="Require the actual sampled GPU frame to use this native camera mode")
    parser.add_argument("--gpu-hand", type=int, choices=(0, 1), default=1,
                        help="Require the actual GPU-held hand: 0 left, 1 right (default)")
    parser.add_argument("--controller-facing", action="store_true",
                        help="Require actual native mesh forward/up to follow OpenXR grip +Y/+Z in either hand")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.gpu_mode and not args.gpu_palette:
        parser.error("--gpu-mode requires --gpu-palette")
    if args.gpu_hand != 1 and not args.gpu_palette:
        parser.error("--gpu-hand requires --gpu-palette")
    manifest = json.loads((args.evidence / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    timing = (grade_fixture_timing(args.evidence)
              if manifest.get("physical_camcorder_fixture_version") in (2, 3) else {})
    mesh_grade(args.evidence)
    bodies = {r["serial"]: r for r in read_rows(args.evidence / "native-skeleton.jsonl")}
    meshes = read_rows(args.evidence / "native-camera-mesh.jsonl")
    events = read_rows(args.evidence / "physical-camcorder-events.jsonl")
    result = grade(args.evidence, args.module, bodies, meshes, events)
    result.update(timing)
    if args.controller_facing or manifest.get("physical_camcorder_fixture_version") == 3:
        result.update(grade_controller_axes(bodies, meshes))
    if args.gpu_palette:
        result.update(grade_gpu_palette(args.evidence, args.module, bodies, meshes, events,
                                       expected_mode=args.gpu_mode, expected_hand=args.gpu_hand))
    if args.gc_continuity:
        result.update(grade_gc_continuity(args.evidence, args.module))
    if args.selftest:
        faults = ("module", "socket", "finger-position", "finger-rotation", "source-camera",
                  "missing-nv", "native-state", "tracking")
        for fault in faults:
            b, m, e = copy.deepcopy(bodies), copy.deepcopy(meshes), copy.deepcopy(events)
            times = [r["clock"] for r in e]
            chosen = next(r for r in m if r["source"] == "body_post_ik" and r["hand"] == 1 and
                          0 <= b[r["native_body_serial"]]["clock_us"] -
                          times[bisect.bisect_right(times, b[r["native_body_serial"]]["clock_us"]) - 1] < 100000 and
                          e[bisect.bisect_right(times, b[r["native_body_serial"]]["clock_us"]) - 1]["native_state"] == 1)
            body = b[chosen["native_body_serial"]]
            if fault == "socket":
                chosen["bone"] = 36
            elif fault == "finger-position":
                body["atoms"][40][4] += 1
            elif fault == "finger-rotation":
                body["atoms"][43][:4] = [0, 0, 0, 1]
            elif fault == "source-camera":
                chosen["stock_original_local"][12] += 1
            elif fault == "missing-nv":
                for r in e:
                    r["native_mode"] = 0
                    r["night_vision"] = False
            elif fault == "native-state":
                for r in e:
                    r["native_state"] = 0
            elif fault == "tracking":
                body["targets"]["hands"][1]["tracked"] = False
            try:
                grade(args.evidence, args.module, b, m, e, fault)
            except ValueError:
                continue
            raise ValueError(f"Native stock-grip oracle accepted corruption: {fault}")
        result["rejected_controls"] = len(faults)
        if args.gpu_palette:
            gpu_faults = ("missing-right-finger", "missing-gpu", "stale-gpu", "wrong-camera-source",
                          "wrong-gpu-frame", "native-state", "missing-camera-mesh", "wrong-shader")
            for fault in gpu_faults:
                try:
                    grade_gpu_palette(args.evidence, args.module, bodies, meshes, events, fault,
                                      expected_hand=args.gpu_hand)
                except ValueError:
                    continue
                raise ValueError(f"Native camera GPU-palette oracle accepted corruption: {fault}")
            result["gpu_palette_controls_rejected"] = len(gpu_faults)
            if args.gpu_mode:
                wrong_mode = "normal" if args.gpu_mode == "night_vision" else "night_vision"
                try:
                    grade_gpu_palette(args.evidence, args.module, bodies, meshes, events,
                                      expected_mode=wrong_mode, expected_hand=args.gpu_hand)
                except ValueError:
                    result["gpu_mode_mismatch_rejected"] = True
                else:
                    raise ValueError("Native camera GPU oracle accepted the wrong required mode")
        if timing:
            timing_faults = ("missing-frame", "wrong-version", "too-fast")
            for fault in timing_faults:
                try:
                    grade_fixture_timing(args.evidence, fault)
                except ValueError:
                    continue
                raise ValueError(f"Native camera timing oracle accepted corruption: {fault}")
            result["timing_controls_rejected"] = len(timing_faults)
        if args.controller_facing or manifest.get("physical_camcorder_fixture_version") == 3:
            axis_faults = ("aim-not-grip", "backward-grip", "up-axis")
            for fault in axis_faults:
                try:
                    grade_controller_axes(bodies, meshes, fault)
                except ValueError:
                    continue
                raise ValueError(f"Native controller-axis oracle accepted corruption: {fault}")
            result["controller_axis_controls_rejected"] = len(axis_faults)
        if args.gc_continuity:
            gc_faults = ("missing-gc", "new-generation", "released-grip",
                         "changed-hand", "repeated-toggle", "missing-rebuild")
            for fault in gc_faults:
                try:
                    grade_gc_continuity(args.evidence, args.module, fault)
                except ValueError:
                    continue
                raise ValueError(f"Native held-GC oracle accepted corruption: {fault}")
            result["gc_controls_rejected"] = len(gc_faults)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
