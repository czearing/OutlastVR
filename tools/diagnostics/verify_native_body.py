import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def segment_reference(before, standing, target, driver):
    reference_arm = (target.get("reference_upper_body", False) and driver in (10, 11, 34, 35)
                     and target["hands"][int(driver >= 34)]["tracked"])
    return standing if target["physical"] or reference_arm else before


def matrix(atom):
    x, y, z, w = atom[:4]
    result = np.eye(4)
    result[:3, :3] = [
        [1 - 2*y*y - 2*z*z, 2*x*y - 2*z*w, 2*x*z + 2*y*w],
        [2*x*y + 2*z*w, 1 - 2*x*x - 2*z*z, 2*y*z - 2*x*w],
        [2*x*z - 2*y*w, 2*y*z + 2*x*w, 1 - 2*x*x - 2*y*y],
    ]
    result[:3, 3] = atom[4:7]
    if len(atom) == 8:
        result[:3, :3] *= atom[7]
    return result


def match_gpu_pose(skeletons, cameras, poses, inverse_reference, samples, required_bones,
                   changed_required, fault="", contact=False):
    require(samples and len({draw["native_frame"] for draw in samples}) == 1,
            "Native GPU sample must contain exactly one nonempty diagnostic frame")
    source = next((camera for camera in cameras if camera["native_frame"] == samples[0]["native_frame"]), None)
    require(source is not None and source["camera_source_serial"] in poses,
            "Native GPU sample lacks its actual camera-source association")
    source_clock = poses[source["camera_source_serial"]]["clock"] / 10
    preceding = sorted((row for row in skeletons if row["clock_us"] <= source_clock),
                       key=lambda row: row["clock_us"])
    candidates = [row for row in preceding[-2:] if row["targets"] and
                  source_clock - row["clock_us"] <= 250000]
    stale_skin = None
    if fault == "--fault-stale-gpu" and not contact:
        require(len(preceding) >= 3, "Stale GPU control lacks an earlier actual native pose")
        stale_skin = (np.array([matrix(atom) for atom in preceding[-3]["atoms"]]) @ inverse_reference)[:, :3, :]
    matches = []
    for candidate in candidates:
        expected_skin = (np.array([matrix(atom) for atom in candidate["atoms"]]) @ inverse_reference)[:, :3, :]
        before_skin = (np.array([matrix(atom) for atom in candidate["before_atoms"]]) @ inverse_reference)[:, :3, :]
        coverage, changed_gpu_bones, matched_draws = set(), set(), []
        for draw in samples:
            for constant in draw["vertex_constants"]:
                if constant["name"] != "BoneMatrices":
                    continue
                gpu = np.array(constant["values"]).reshape(-1, 3, 4)
                if fault == "--fault-gpu" or (contact and fault == "--fault-contact-gpu"):
                    gpu.fill(0)
                elif stale_skin is not None:
                    gpu[:] = stale_skin[np.arange(len(gpu)) % len(stale_skin)]
                errors = np.max(np.abs(gpu[:, None] - expected_skin[None]), axis=(2, 3))
                matched = set(np.flatnonzero(errors.min(axis=0) < .001).tolist()) - {0, 1, 2}
                if len(matched) < 25:
                    continue
                matched_draws.append(draw["draw"])
                coverage |= matched
                for bone in matched:
                    if np.max(np.abs(expected_skin[bone] - before_skin[bone])) > .1:
                        changed_gpu_bones.add(bone)
        if len(matched_draws) >= 3 and required_bones <= coverage and changed_required <= changed_gpu_bones:
            matches.append((candidate, matched_draws, coverage, changed_gpu_bones))
    require(len(matches) == 1,
            "Actual contact GPU skinning did not contain constrained arms" if contact else
            "Actual GPU skinning did not contain the controller-driven arms and physically solved legs")
    pose, matched_draws, coverage, changed_gpu_bones = matches[0]
    return pose, matched_draws, coverage, changed_gpu_bones, preceding, source_clock


def grade(directory, fault):
    skeletons = load_rows(directory / "native-skeleton.jsonl")
    cameras = load_rows(directory / "native-cameras.jsonl")
    poses = {row["serial"]: row for row in load_rows(directory / "camera-poses.jsonl")}
    input_events = load_rows(directory / "native-input-events.jsonl")
    inputs = [row for row in input_events if row["kind"] == "motion_pad"]
    draws = [row for row in load_rows(directory / "draw-probe" / "draws.jsonl") if "draw" in row]
    require(draws, "Native GPU diagnostic contains no eligible captured draw")
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require(manifest["body"] and manifest["scenario"] == "Continue", "Not an explicit native body scenario")
    require((directory / "capture-complete.txt").is_file(), "Native capture did not finish")
    startup_proof = None
    if manifest.get("floor_startup"):
        startup_frames = [row for row in load_rows(directory / "frames.jsonl") if not row.get("canceled")]
        if manifest.get("draw_contact"):
            require(manifest.get("contact_fixture_version") == 3 and
                    all(row.get("contact_fixture_version") == 3 for row in startup_frames),
                    "Paired native GPU diagnostic lacks its explicit fixture identity")
        pending = [row for row in startup_frames if not row["floor_reference_ready"]]
        measured = [row for row in startup_frames if row["floor_reference_ready"]]
        if fault == "--fault-floor-startup":
            measured[0]["standing_reference_m"] = 0
        require(len(pending) >= 7 and any(not row["world_panel"] for row in pending) and len(measured) >= 800,
                "Native startup fixture did not retain pending floor calibration into actual gameplay")
        require(all(row["floor_reference_acquisitions"] == 0 for row in pending) and
                all(row["floor_reference_acquisitions"] == 1 and abs(row["standing_reference_m"] - 1.7) < .0001
                    for row in measured) and
                max(row["frame"] for row in pending) < min(row["frame"] for row in measured),
                "Native startup latched invalid height, recalibrated during crouch or lost its measured baseline")
        startup_proof = {"pending_frames": len(pending), "measured_frames": len(measured),
                         "measured_standing_m": 1.7, "floor_acquisitions": 1}
    focus_proof = None
    if manifest.get("desktop_unfocused"):
        focus_rows = [row for row in input_events if row["kind"] == "joystick_focus"]
        overrides = [row for row in focus_rows if not row["actual_focus"] and row["focused"] and row["result"] == 1]
        blocked = [row for row in focus_rows if not row["actual_focus"] and not row["focused"] and not row["result"]]
        require(len(overrides) >= 800 and len(blocked) >= 30,
                "Explicit desktop-unfocused run did not prove XR-owned polling and unfocused native rejection")
        require(all(row["actual_focus"] or row["focused"] or not row["result"] for row in focus_rows),
                "Native joystick focus override remained active after XR focus loss")
        focus_proof = {"actual_desktop_unfocused_overrides": len(overrides),
                       "xr_unfocused_gate_rejections": len(blocked)}
    contact_queries = (load_rows(directory / "native-hand-contacts.jsonl")
                       if (directory / "native-hand-contacts.jsonl").is_file() else [])
    contact_groups = {}
    for query in contact_queries:
        require(query["thread"] == query["input_thread"] and query["thread"] != 0,
                "Native hand collision query ran outside the observed simulation/input thread")
        require(np.array_equal(query["extent"], [8, 8, 8]),
                "Native hand sweep volume changed without an explicit policy change")
        contact_groups.setdefault((query["clock_us"], query["hand"]), []).append(query)
    if fault == "--fault-contact-provenance":
        contact_groups.clear()
    contacts = [[], []]
    for (clock, hand), queries in contact_groups.items():
        if all(query["clear"] for query in queries):
            continue
        final = queries[-1]
        point = np.array(final["end"], dtype=float)
        if not final["clear"]:
            start = np.array(final["start"])
            delta = point - start
            length = np.linalg.norm(delta)
            require(length > 0 and 0 <= final["time"] <= 1,
                    "Native blocked sweep has invalid geometry")
            point = start + delta * max(0, final["time"] - .1 / length)
        contacts[hand].append((clock, point))
    constrained_hands = 0
    render_size = None
    if manifest.get("recommended_resolution"):
        from verify_native_probe import recommended_extent_grade
        frames = [row for row in load_rows(directory / "frames.jsonl") if not row.get("canceled")]
        if fault == "--fault-render-size":
            frames[0]["source_width"] += 1
        if fault == "--fault-direct-atlas":
            for frame in frames:
                frame["direct_xr_submissions"] = 0
        render_size = recommended_extent_grade(frames, manifest.get("diagnostic_eye_size", 1024),
                                               manifest.get("headset_extent_2688x2880", False))
        require(render_size["native_gameplay_projections"] >= 900,
                "Insufficient actual native body gameplay projections")
    active = [row for row in skeletons if row["targets"]]
    require(len(active) >= 800, "Insufficient actual native body evaluations")
    if fault == "--fault-native-pose":
        active[0]["atoms"] = active[0]["before_atoms"]
    if fault == "--fault-skinning":
        for segment in [(31, 32), (29, 30), (55, 56), (53, 54), (57, 58), (65, 66), (59, 60), (71, 72)]:
            for helper in segment:
                active[0]["atoms"][helper] = active[0]["before_atoms"][helper]
    if fault == "--fault-camera":
        for camera in cameras:
            if camera["camera_source_serial"] in poses and poses[camera["camera_source_serial"]]["pawn_crouched"]:
                camera["tracking_origin"][2] += 100
    max_hand_error = max_limb_error = max_foot_error = 0.0
    alignment_evaluations = 0
    max_alignment_error_m = 0.0
    physical = []
    skinning_segments = [(10, 31, 32), (11, 29, 30), (34, 55, 56), (35, 53, 54),
                         (61, 57, 58), (62, 65, 66), (67, 59, 60), (68, 71, 72)]
    standing_atoms = np.array(active[0]["before_atoms"])
    reference_row = next(row for row in skeletons if row["reference_records"])
    reference_records = np.array([
        [struct.unpack("<f", struct.pack("<I", word))[0] for word in record]
        for record in reference_row["reference_records"]])
    palm_geometry = []
    palm_versions = {row["targets"].get("palm_axes_version", 1) for row in active}
    require(len(palm_versions) == 1 and palm_versions <= {1, 2},
            "Mixed or unsupported anatomical palm policy")
    palm_version = palm_versions.pop()
    for fingers in [(16, 19, 22, 25), (40, 43, 46, 49)]:
        knuckles = reference_records[list(fingers), 8:11]
        center = knuckles.mean(axis=0) * .5
        z = knuckles[-1] - knuckles[0]
        z /= np.linalg.norm(z)
        x = np.cross(center, z)
        if palm_version == 2:
            x = -x
        x /= np.linalg.norm(x)
        palm_geometry.append((center, x, z))
    grip_evaluations = 0
    camera_local = None
    require(all(row["targets"].get("camera_hand", -1) in (-1, 0, 1) and
                (row["targets"].get("camera_hand", -1) == -1 or
                 row["targets"]["hands"][row["targets"]["camera_hand"]]["tracked"])
                for row in active),
            "Invalid or untracked native camera wrist ownership")
    if any(row["targets"].get("camera_hand", -1) >= 0 for row in active) and palm_version == 1:
        require(manifest.get("camera_mount_version") == 2,
                "Camera-specific wrist orientation lacks explicit authored-mount provenance")
        mounts = load_rows(directory / "native-camera-mesh.jsonl")
        mount = next(sample for sample in mounts if sample.get("stock_mount_version") == 2)
        camera_local = np.array(mount["stock_original_local"]).reshape(4, 4).T
    max_grip_axis_error = 0.0
    max_skinning_error = 0.0
    expected_bindings = {
        "Hero-Root": 0, "Hero-Pelvis": 3, "Hero-Spine": 4, "Hero-Spine1": 5, "Hero-Spine2": 6,
        "Hero-Head": 8, "Hero-L-Clavicle": 9, "Hero-L-UpperArm": 10, "Hero-L-Forearm": 11,
        "Hero-L-Hand": 12, "Hero-R-Clavicle": 33, "Hero-R-UpperArm": 34, "Hero-R-Forearm": 35,
        "Hero-R-Hand": 36, "Hero-L-Thigh": 61, "Hero-L-Calf": 62, "Hero-L-Foot": 63,
        "Hero-R-Thigh": 67, "Hero-R-Calf": 68, "Hero-R-Foot": 69,
    }
    if manifest["body_schedule_version"] >= 2:
        for names, indices in [
            (("Hero-LUpArmTwist", "Hero-LUpArmTwist1"), (31, 32)),
            (("Hero-L-ForeTwist", "Hero-L-ForeTwist1"), (29, 30)),
            (("Hero-RUpArmTwist", "Hero-RUpArmTwist1"), (55, 56)),
            (("Hero-R-ForeTwist", "Hero-R-ForeTwist1"), (53, 54)),
            (("Hero-LThighTwist", "Hero-LThighTwist1"), (57, 58)),
            (("Hero-LCalfTwist", "Hero-LCalfTwist1"), (65, 66)),
            (("Hero-RThighTwist", "Hero-RThighTwist1"), (59, 60)),
            (("Hero-RCalfTwist", "Hero-RCalfTwist1"), (71, 72)),
        ]:
            expected_bindings.update(zip(names, indices))
    standing_feet = np.array(active[0]["before_atoms"])[[63, 69], 4:7]
    crouch_foot_reference = None
    for row in active:
        before, after = np.array(row["before_atoms"]), np.array(row["atoms"])
        require(before.shape == after.shape == (73, 8), "Native hero topology changed")
        require(np.isfinite(after).all(), "Nonfinite native body pose")
        require(np.array_equal(before[:3], after[:3]), "Native body modified root/camera ancestors")
        require(np.max(np.abs(np.linalg.norm(after[:, :4], axis=1) - 1)) < .001, "Native body quaternion invalid")
        require(np.array_equal(before[:, 7], after[:, 7]), "Native body changed original bone scales")
        target = row["targets"]
        for chain in [(10, 11, 12), (34, 35, 36), (61, 62, 63), (67, 68, 69)]:
            source = segment_reference(before, standing_atoms, target, chain[0])
            for a, b in zip(chain, chain[1:]):
                error = abs(np.linalg.norm(source[a, 4:7] - source[b, 4:7]) -
                            np.linalg.norm(after[a, 4:7] - after[b, 4:7]))
                max_limb_error = max(max_limb_error, float(error))
                require(error < .02, "Actual native IK stretched a limb")
        require(row["ik_hands"] == sum(hand["tracked"] for hand in target["hands"]),
                "Native fallback count disagrees with the original tracked-hand flags")
        grip_anchored = target.get("grip_anchored", False)
        if manifest.get("grip_anchor_version"):
            require(grip_anchored, "Native body silently reverted to wrist-as-grip alignment")
        grip_evaluations += grip_anchored
        for driver, first_helper, second_helper in skinning_segments:
            skinning_source = segment_reference(before, standing_atoms, target, driver)
            delta = matrix(after[driver]) @ np.linalg.inv(matrix(skinning_source[driver]))
            for helper in [first_helper, second_helper]:
                error = np.max(np.abs(matrix(after[helper]) - delta @ matrix(skinning_source[helper])))
                max_skinning_error = max(max_skinning_error, float(error))
                require(error < .03, "Actual native parallel skinning chain did not follow its solved IK segment")
        if not target["physical"]:
            crouch_foot_reference = None
        elif row["pawn_crouched"] and not row["special"] and crouch_foot_reference is None:
            crouch_foot_reference = before[[63, 69], 4:7].copy()
        for side, foot in enumerate([63, 69]):
            expected_foot = before[foot, 4:7]
            if target["physical"]:
                expected_foot = standing_feet[side]
                if row["pawn_crouched"] and not row["special"] and crouch_foot_reference is not None:
                    expected_foot = expected_foot + before[foot, 4:7] - crouch_foot_reference[side]
            error = np.linalg.norm(expected_foot - after[foot, 4:7])
            max_foot_error = max(max_foot_error, float(error))
            require(error < .03, "Actual native IK moved a planted foot")
        basis = np.array([target["right"], target["up"], target["backward"]]).T
        inverse_world = np.linalg.inv(np.array(row["world"]).reshape(4, 4).T)
        for side, (hand, chain) in enumerate(zip(target["hands"], [(10, 11, 12), (34, 35, 36)])):
            if not hand["tracked"]:
                continue
            upper, middle, end = chain
            goal_world = np.array(target["origin"]) + basis @ np.array(hand["position"]) * target["units_per_meter"]
            goal = (inverse_world @ np.r_[goal_world, 1])[:3]
            root = after[upper, 4:7]
            anchor = after[end, 4:7]
            if grip_anchored:
                center, palm_x, palm_z = palm_geometry[side]
                require(np.linalg.norm(center - hand["palm_center_local"]) < .001 and
                        np.linalg.norm(palm_x - hand["palm_x_local"]) < .001 and
                        np.linalg.norm(palm_z - hand["palm_z_local"]) < .001,
                        "Native grip calibration disagrees with immutable metacarpal geometry")
                wrist = matrix(after[end])
                anchor = (wrist @ np.r_[center, 1])[:3]
                if fault == "--fault-grip-offset" and row is active[0]:
                    anchor = after[end, 4:7]
                grip_rotation = matrix([*hand["orientation"], 0, 0, 0])[:3, :3]
                for local, axis in [(palm_x, [1, 0, 0]), (palm_z, [0, 0, 1])]:
                    actual = wrist[:3, :3] @ local
                    expected = inverse_world[:3, :3] @ basis @ grip_rotation @ axis
                    axis_error = float(np.linalg.norm(actual / np.linalg.norm(actual) -
                                                      expected / np.linalg.norm(expected)))
                    max_grip_axis_error = max(max_grip_axis_error, axis_error)
                    require(axis_error < .001, "Rendered native palm axes do not match the XR grip")
            error = np.linalg.norm(anchor - goal)
            max_hand_error = max(max_hand_error, float(error))
            require(error < .03, "Actual native hand missed the original unclamped controller target")
            raw_world = (np.array(target["origin"]) + basis @ np.array(hand.get("raw_position", hand["position"])) *
                         target["units_per_meter"])
            raw_goal = (inverse_world @ np.r_[raw_world, 1])[:3]
            raw_error = np.linalg.norm(anchor - raw_goal)
            require(abs(raw_error - hand["controller_error_units"]) < .03,
                    "Native hand telemetry concealed raw controller discrepancy")
            require(hand.get("world_contact", False) or raw_error < .03,
                    "Unobstructed native hand missed its original physical controller target")
            if manifest.get("hand_alignment_version"):
                offset = target["hand_wristward_m"]
                require(np.isfinite(offset) and abs(offset) <= .060001,
                        "Invalid controller-local hand fit offset")
                require(abs(offset - float(manifest["hand_wristward_cm"]) * .01) < 1e-7 and
                        ((palm_version == 1 and target.get("camera_hand", -1) == side) or
                         np.array_equal(hand["controller_orientation"], hand["orientation"])),
                        "Controller-local fit changed orientation or ignored the requested offset")
                if target.get("camera_hand", -1) == side and palm_version == 1:
                    relative = np.linalg.inv(matrix(before[36])) @ matrix(before[52]) @ camera_local
                    _, x, z = palm_geometry[1]
                    palm_basis = np.column_stack((x, -np.cross(z, x), z))
                    forward = palm_basis.T @ relative[:3, 0]
                    up = palm_basis.T @ relative[:3, 2]
                    forward /= np.linalg.norm(forward)
                    up -= forward * np.dot(up, forward)
                    up /= np.linalg.norm(up)
                    optical = np.column_stack((np.cross(forward, up), up, -forward))
                    expected_orientation = matrix([*hand["controller_orientation"], 0, 0, 0])[:3, :3] @ optical.T
                    actual_orientation = matrix([*hand["orientation"], 0, 0, 0])[:3, :3]
                    require(np.max(np.abs(expected_orientation - actual_orientation)) < .0001,
                            "Held wrist does not preserve stock camera grip while aiming along the physical controller")
                controller = np.array(hand["controller_position"])
                expected_fit = controller + matrix([*hand["controller_orientation"], 0, 0, 0])[:3, :3] @ [0, offset, 0]
                if fault == "--fault-hand-fit" and row is active[0]:
                    expected_fit = controller - (expected_fit - controller)
                fit_error = float(np.linalg.norm(expected_fit - hand["raw_position"]))
                max_alignment_error_m = max(max_alignment_error_m, fit_error)
                alignment_evaluations += 1
                require(fit_error < 1e-5,
                        "Native hand correction was not applied in controller-local space")
                controller_world = np.array(target["origin"]) + basis @ controller * target["units_per_meter"]
                controller_goal = (inverse_world @ np.r_[controller_world, 1])[:3]
                require(abs(np.linalg.norm(anchor - controller_goal) -
                            hand["uncalibrated_controller_error_units"]) < .03,
                        "Native hand fit concealed unmodified controller pose discrepancy")
            if hand.get("world_contact", False):
                require(any(0 <= row["clock_us"] - clock <= 100000 and
                            np.linalg.norm(goal_world - point) < .04 for clock, point in contacts[side]),
                        "Constrained hand target lacks a matching actual native world sweep")
                constrained_hands += 1
            require(hand.get("contact_target_error_units", hand["controller_error_units"]) < .03 and
                    hand["shoulder_adjustment_units"] <= target["units_per_meter"] * .35 + .001,
                    "Native shoulder adaptation exceeded its explicit physical limit")
        if target["physical"]:
            physical.append(row)
        require(len(row["bindings"]) == len(expected_bindings) and
                {binding["name"]: binding["index"] for binding in row["bindings"]} == expected_bindings,
                "Actual native driver/skinning name bindings are missing, repeated or incorrect")
        require(max(binding["matrix_error"] for binding in row["bindings"]) == 0,
                "Actual game GetBoneMatrix disagreed with the applied native pose")
    standing_head = standing_atoms[8][6]
    require(len(physical) > 300 and sum(row["pawn_crouched"] for row in physical) > 200,
            "Physical squats never reached actual native pawn crouch")
    require(max(abs(row["atoms"][8][6] - standing_head -
                    (row["targets"]["head_position"][1] + row["targets"].get("standing_height_delta", 0)) * 100)
                for row in physical) < .03, "Physical native body head was lowered twice")
    height_reference = active[0]["targets"]["origin"][2] - active[0]["root"][2]
    crouch_views = [camera for camera in cameras if camera["camera_source_serial"] in poses
                    and poses[camera["camera_source_serial"]]["pawn_crouched"]]
    camera_error = max(abs(camera["tracking_origin"][2] - poses[camera["camera_source_serial"]]["root"][2] -
                           height_reference) for camera in crouch_views)
    require(len(crouch_views) > 200 and camera_error < .03, "Native physical camera compensation failed")
    require(sum(row["pose_restored"] for row in skeletons) >= len(active) - 1,
            "Native cached pose was not restored before subsequent evaluations")
    # Native animation can evaluate the same XR tracking window more than once.
    fallback_evaluations = sum(row["ik_hands"] == 1 for row in skeletons)
    if fault == "--fault-tracking-fallback":
        fallback_evaluations = 29
    require(fallback_evaluations >= 30, "Lost-hand native fallback did not reach 30 actual evaluations")
    unfocused = [row for row in inputs if not row["focused"]]
    require((manifest.get("desktop_unfocused") or len(unfocused) >= 30) and
            all(not row["buttons"] and not row["left_x"] and not row["left_y"]
                                       and not row["right_x"] for row in unfocused),
            "Actual unfocused native controls were not neutral")
    require(sum(bool(row["buttons"] & 8192) for row in inputs) == 6, "Physical crouch/stand toggles repeated or missing")
    locomotion = None
    if manifest["body_schedule_version"] >= 3:
        if fault == "--fault-native-travel":
            for row in active:
                row["root"][:2] = active[0]["root"][:2]
        distances = {}
        for crouched, name in [(False, "standing"), (True, "physically_crouched")]:
            positions = np.array([row["root"][:2] for row in active
                                  if row["pawn_crouched"] == crouched and
                                  (not crouched or row["targets"]["physical"])])
            distance = float(np.linalg.norm(np.diff(positions, axis=0), axis=1).sum())
            require(distance > 10, f"Native controller locomotion did not move the actual {name} pawn")
            distances[name+"_travel_units"] = distance
        movement_polls = sum(row["focused"] and row["left_y"] >= 22000 for row in inputs)
        lateral_polls = sum(row["focused"] and row["left_x"] >= 22000 for row in inputs)
        if manifest.get("contacts"):
            require(movement_polls >= 25 and lateral_polls >= 270,
                    "Explicit contact path missed its standing forward walk or extended lateral wall approach")
        else:
            require(movement_polls >= 50, "Native standing/crouched controller movement was not polled")
        locomotion = dict(**distances, actual_native_movement_polls=movement_polls)
        if manifest.get("contacts"):
            locomotion["actual_native_lateral_polls"] = lateral_polls
    first = next(row for row in skeletons if row["reference_records"])
    if manifest.get("floor"):
        require(abs(height_reference - 170) < .03,
                "Actual native floor-based height did not match the explicit1.7m STAGE fixture")
    records = np.array([[struct.unpack("<f", struct.pack("<I", word))[0] for word in record]
                        for record in first["reference_records"]])
    reference = []
    for index, record in enumerate(records):
        local = matrix(record[4:11])
        reference.append(reference[first["parents"][index]] @ local if index else local)
    inverse_reference = np.linalg.inv(reference)
    required_bones = {3, 12, 36, 63, 69} | {helper for segment in skinning_segments for helper in segment[1:]}
    pose, matched_draws, coverage, changed_gpu_bones, preceding, source_clock = match_gpu_pose(
        skeletons, cameras, poses, inverse_reference, draws, required_bones, required_bones, fault)
    contact_sample = None
    contact_pose = pose
    if manifest.get("draw_contact"):
        require(pose["targets"]["physical"] and pose["pawn_crouched"] and not pose["special"] and
                pose["targets"]["head_position"][1] < -.25 and
                all(hand["tracked"] for hand in pose["targets"]["hands"]),
                "Body GPU sample did not retain settled physical crouch and both tracked hands")
        contact_draws = [row for row in load_rows(directory / "contact-draw-probe" / "draws.jsonl") if "draw" in row]
        if fault == "--fault-contact-frame":
            contact_draws = draws
        require(contact_draws and contact_draws[0]["native_frame"] != draws[0]["native_frame"],
                "Body/crouch and world-contact diagnostics require separate actual GPU frames")
        arms = {12, 36} | {helper for segment in skinning_segments
                          if segment[0] in {10, 11, 34, 35} for helper in segment[1:]}
        contact_pose, contact_matches, contact_coverage, contact_changed, contact_preceding, contact_clock = match_gpu_pose(
            skeletons, cameras, poses, inverse_reference, contact_draws, required_bones, arms, fault, contact=True)
        require(contact_pose["targets"]["physical"] and all(hand["tracked"] for hand in contact_pose["targets"]["hands"]),
                "Contact-selected GPU pose did not retain physical body IK and both tracked hands")
        contact_sample = {
            "native_frame": contact_draws[0]["native_frame"], "native_pose_serial": contact_pose["serial"],
            "world_constrained_hands": sum(hand["world_contact"] for hand in contact_pose["targets"]["hands"]),
            "matched_draws": contact_matches, "matched_bones": sorted(contact_coverage),
            "changed_arm_bones": sorted(arms), "changed_bones": sorted(contact_changed),
            "pose_lag_evaluations": contact_preceding[-1]["serial"] - contact_pose["serial"],
            "camera_source_age_us": contact_clock - contact_pose["clock_us"],
            "scope": "Separate actual contact frame: complete native body palette coverage and changed arms. Full changed leg/helper GPU evidence remains mandatory in the independent body/crouch frame.",
        }
    if manifest.get("contacts"):
        require(any(not query["clear"] for query in contact_queries) and constrained_hands > 0,
                "Explicit world-contact scenario never blocked a native hand sweep and constrained the rendered rig")
        require(any(hand.get("world_contact", False) for hand in contact_pose["targets"]["hands"]),
                "Actual GPU pose did not contain a native world-constrained hand")
    return {
        "status": "pass",
        "mod_sha256": manifest["mod_sha256"],
        "probe_sha256": manifest["probe_sha256"],
        "native_evaluations": len(skeletons),
        "ik_evaluations": len(active),
        "native_one_hand_fallback_evaluations": fallback_evaluations,
        "physical_body_evaluations": len(physical),
        "max_native_hand_error_units": max_hand_error,
        "native_grip_anchored_evaluations": grip_evaluations,
        "native_hand_alignment": ({
            "wristward_cm": float(manifest["hand_wristward_cm"]),
            "controller_local_axis": "+Y; actual immutable native wristward direction checked on both hands",
            "verified_tracked_hand_evaluations": alignment_evaluations,
            "max_controller_local_fit_error_m": max_alignment_error_m,
            "unmodified_controller_position_and_orientation_retained": True,
            "physical_headset_fit_accepted": False,
        } if manifest.get("hand_alignment_version") else None),
        "max_native_grip_axis_error": max_grip_axis_error,
        "native_hand_collision_queries": len(contact_queries),
        "native_blocked_hand_sweeps": sum(not query["clear"] for query in contact_queries),
        "native_constrained_hand_evaluations": constrained_hands,
        "collision_policy": ("Explicit8cm swept grip/palm-center volume; raw controller discrepancy retained separately."
                             if grip_evaluations else
                             "Explicit8cm swept wrist volume; raw controller discrepancy retained separately.") +
                            " Not complete articulated finger/hand rigid-body physics.",
        "max_limb_length_error_units": max_limb_error,
        "max_planted_foot_error_units": max_foot_error,
        "max_parallel_skinning_matrix_error": max_skinning_error,
        "max_physical_camera_anchor_error_units": camera_error,
        "native_input_polls": len(inputs),
        "native_unfocused_polls": len(unfocused),
        "native_physical_crouch_edges": 6,
        "native_joystick_focus": focus_proof,
        "native_floor_startup": startup_proof,
        "gpu_source_native_pose_serial": pose["serial"],
        "gpu_source_native_frame": draws[0]["native_frame"],
        "gpu_contact_sample": contact_sample,
        "gpu_world_constrained_hands": sum(hand.get("world_contact", False) for hand in pose["targets"]["hands"]),
        "gpu_pose_lag_evaluations": preceding[-1]["serial"] - pose["serial"],
        "gpu_pose_camera_source_age_us": source_clock - pose["clock_us"],
        "gpu_pose_source_association": "Unique complete palette match in the latest two preceding native evaluations, no older than250ms; never an unrestricted historical search.",
        "gpu_matched_draws": matched_draws,
        "gpu_matched_bones": sorted(coverage),
        "gpu_changed_bones": sorted(changed_gpu_bones),
        "native_recommended_render_size": render_size,
        "native_controller_locomotion": locomotion,
        "physical_headset_tested": False,
        "limitations": "Controlled native body scenario, not acceptance of all scenes, physical comfort, grip ergonomics or headset FPS.",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--fault", choices=["--fault-native-pose", "--fault-skinning", "--fault-gpu", "--fault-camera",
                                          "--fault-render-size", "--fault-direct-atlas", "--fault-native-travel",
                                          "--fault-stale-gpu", "--fault-contact-provenance",
                                          "--fault-floor-startup", "--fault-grip-offset", "--fault-hand-fit",
                                          "--fault-tracking-fallback", "--fault-contact-gpu", "--fault-contact-frame"], default="")
    args = parser.parse_args()
    result = grade(args.capture, args.fault)
    if args.selftest:
        require(not args.fault, "Positive selftest cannot also inject a fault")
        manifest = json.loads((args.capture / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
        require(manifest.get("hand_alignment_version") and manifest.get("floor_startup") and
                manifest.get("contacts") and manifest.get("recommended_resolution"),
                "Complete body selftest requires the explicit fit/floor/contact/recommended-resolution capture")
        controls = ["--fault-native-pose", "--fault-skinning", "--fault-gpu", "--fault-camera",
                    "--fault-render-size", "--fault-direct-atlas", "--fault-native-travel",
                    "--fault-stale-gpu", "--fault-contact-provenance", "--fault-floor-startup",
                    "--fault-grip-offset", "--fault-hand-fit", "--fault-tracking-fallback"]
        if manifest.get("draw_contact"):
            controls += ["--fault-contact-gpu", "--fault-contact-frame"]
        rejected = []
        for fault in controls:
            try:
                grade(args.capture, fault)
            except ValueError:
                rejected.append(fault)
            else:
                raise ValueError("Native body oracle accepted fault: " + fault)
        result["rejected_controls"] = rejected
    names = ["native-skeleton.jsonl", "native-cameras.jsonl", "camera-poses.jsonl",
             "native-input-events.jsonl", "draw-probe\\draws.jsonl", "native-run-manifest.json", "frames.jsonl"]
    if result["gpu_contact_sample"] is not None:
        names.append("contact-draw-probe\\draws.jsonl")
    if (args.capture / "native-hand-contacts.jsonl").is_file():
        names.append("native-hand-contacts.jsonl")
    result["input_sha256"] = {
        name: hashlib.sha256((args.capture / name).read_bytes()).hexdigest()
        for name in names
    }
    result["verifier_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (args.capture / "native-body-proof.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
