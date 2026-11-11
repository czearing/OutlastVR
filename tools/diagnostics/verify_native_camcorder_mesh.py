"""Independent retail-mesh geometry oracle; passive RAM reads are non-atomic."""
import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path

import numpy as np

from verify_native_probe import require, rotation

CALIBRATION_SHA256 = "face43260b8bb7dd91ef73fb41937e5ab31d703d1525bd3fcfda5a70864f4dd7"


def read_rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines()]


def reference(records, bone):
    values = struct.unpack("<7f", struct.pack("<7I", *records[bone][4:11]))
    return rotation(values[:4]), np.array(values[4:])


def optics(row, records):
    world = np.array(row["world"]).reshape(4, 4)
    matrices = []
    for atom in row["atoms"]:
        local = np.eye(4)
        local[:3, :3] = rotation(atom[:4]).T * atom[7]
        local[3, :3] = atom[4:7]
        matrices.append(local @ world)
    root_rotation, root_position = reference(records, 0)
    screen_rotation, screen_position = reference(records, 1)
    screen_position = root_position + root_rotation @ screen_position
    screen_rotation = root_rotation @ screen_rotation

    def point(matrix, inverse_reference, position, asset_point):
        local = inverse_reference @ (np.array(asset_point) - position)
        return (np.append(local, 1) @ matrix)[:3]

    lens = point(matrices[0], root_rotation.T, root_position,
                 [9.208990097, .509961605, 2.846994519])
    screen = point(matrices[1], screen_rotation.T, screen_position,
                   [3.93251705, -9.53543067, 2.85399204])

    def vector(matrix, inverse_reference, value):
        return (np.append(inverse_reference @ value, 0) @ matrix)[:3]

    forward = vector(matrices[0], root_rotation.T, [1, 0, 0])
    up = vector(matrices[0], root_rotation.T, [0, 0, 1])
    axes = np.column_stack([row["tracking_right"], row["tracking_up"], row["tracking_backward"]])
    screen_up = vector(matrices[1], screen_rotation.T, [0, 0, 1])
    screen_forward = vector(matrices[1], screen_rotation.T, [1, 0, 0])
    right = vector(matrices[1], screen_rotation.T, [0, 1, 0])
    units = row["units_per_meter"]
    return dict(lens=lens, forward=forward / np.linalg.norm(forward),
                up=up / np.linalg.norm(up),
                screen=(screen - row["tracking_origin"]) @ axes / units,
                screen_forward=screen_forward @ axes / np.linalg.norm(screen_forward),
                screen_up=screen_up @ axes / np.linalg.norm(screen_up),
                half_extents=np.array([4.24970460*np.linalg.norm(right)/units,
                                       2.47671664*np.linalg.norm(screen_up)/units]))


def load(directory):
    observations = read_rows(directory / "native-retail-camera-observation.jsonl")
    require(observations[0]["kind"] == "identity" and observations[0]["read_only"],
            "Missing independent read-only native camera observation")
    require(observations[-1]["kind"] == "complete", "Independent native camera observation did not finish")
    meshes = read_rows(directory / "native-camera-mesh.jsonl")
    identities = {}
    samples = [row for row in observations if row["kind"] == "sample"]
    for row in samples:
        camera = row["camera"]
        require(camera["asset_name"] == "handycam" and camera["template_name"] == "CameraMeshComponent"
                and camera["class_name"] == "SkeletalMeshComponent" and camera["reference_count"] == 2,
                "Independent observer found a substitute native camera")
        identities[(camera["component"], camera["asset"])] = camera["reference_records"]
    require(len(samples) >= 150 and len(meshes) >= 500, "Insufficient actual retail camera observations")
    return meshes, identities, len(samples)


def stock_grips(directory, anatomical=False):
    source = next(row for row in read_rows(directory / "native-skeleton.jsonl")
                  if row["reference_records"])
    records = np.array([struct.unpack("<20f", struct.pack("<20I", *record))
                        for record in source["reference_records"]])
    result = []
    for fingers in ((16, 19, 22, 25), (40, 43, 46, 49)):
        knuckles = records[list(fingers), 8:11]
        center = knuckles.mean(axis=0) * .5
        z = knuckles[-1] - knuckles[0]
        z /= np.linalg.norm(z)
        x = np.cross(center, z)
        if anatomical:
            x = -x
        x /= np.linalg.norm(x)
        result.append((center, np.column_stack((x, np.cross(z, x), z))))
    return result


def stock_mount(row, grips, socket_reference=None):
    original = np.array(row["stock_original_local"]).reshape(4, 4)
    right_socket = np.array(row["right_socket_world"]).reshape(4, 4)
    if socket_reference is not None:
        right_socket = np.eye(4)
        right_socket[:3, :3] = rotation(socket_reference[4:8]).T
        right_socket[3, :3] = socket_reference[8:11]
        right_socket = right_socket @ np.array(row["right_wrist_world"]).reshape(4, 4)
    stock = original @ right_socket
    if row["hand"] == 1:
        require(row["bone"] == (36 if socket_reference is not None else 52),
                "Right camera left its admitted native mount")
        return stock
    require(row["hand"] == 0 and row["bone"] == 12, "Invalid mirrored stock camera mount")
    relative = stock @ np.linalg.inv(np.array(row["right_wrist_world"]).reshape(4, 4))
    left_center, left_basis = grips[0]
    right_center, right_basis = grips[1]
    transfer = right_basis @ left_basis.T
    relative[:3, :3] = relative[:3, :3] @ transfer
    relative[3, :3] = left_center + (relative[3, :3] - right_center) @ transfer
    return relative @ np.array(row["left_wrist_world"]).reshape(4, 4)

def calibration_contact(calibration):
    def matrix(atom):
        result = np.eye(4)
        result[:3, :3] = rotation(atom[:4]).T * atom[7]
        result[3, :3] = atom[4:7]
        return result

    bits = calibration["right_knuckle_position_bits"]
    knuckles = np.array([struct.unpack("<3f", struct.pack("<3I", *row)) for row in bits])
    palm = knuckles.mean(axis=0) * .5
    relative = (np.array(calibration["stock_original_local"]).reshape(4, 4) @
                matrix(calibration["before_socket"]) @ np.linalg.inv(matrix(calibration["before_wrist"])))
    return (np.append(palm, 1) @ np.linalg.inv(relative))[:3]


def fixed_grip_mount(row, grips, contact):
    hand = row["hand"]
    require(hand in (0, 1) and row["bone"] == (36 if hand else 12),
            "Canonical camera left its immutable holding wrist")
    center, basis = grips[hand]
    forward, up = -basis[:, 2], -basis[:, 1]
    relative = np.eye(4)
    scale = np.linalg.norm(np.array(row["stock_original_local"]).reshape(4, 4)[:3, :3], axis=1)
    relative[:3, :3] = np.array([forward, np.cross(up, forward), up]) * scale[:, None]
    contact = np.array(contact) * [1, 1 if hand else -1, 1]
    require(np.linalg.norm(contact - row["model_grip_point"]) < .00002,
            "Camera contact is not derived from the independent native raised pose")
    relative[3, :3] = center - contact @ relative[:3, :3]
    wrist = np.array(row["right_wrist_world"] if hand else row["left_wrist_world"]).reshape(4, 4)
    return relative @ wrist


def grade(directory, meshes=None, identities=None):
    observed_meshes, observed_identities, observations = load(directory)
    meshes = observed_meshes if meshes is None else meshes
    identities = observed_identities if identities is None else identities
    versions = {row.get("stock_mount_version", 1) for row in meshes}
    require(len(versions) == 1 and versions <= {1, 2, 4, 5}, "Mixed or unsupported camera mount evidence")
    version = versions.pop()
    grip_facing = False
    if version == 2:
        manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
        require(manifest.get("camera_mount_version") == 2, "Authored mount lacks an explicit versioned native run")
        grip_facing = manifest.get("physical_camcorder_fixture_version") == 3
    grips = stock_grips(directory, anatomical=version in (4, 5)) if version in (2, 4, 5) else None
    contact = None
    if version == 5:
        manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
        calibration_path = directory / "native-camera-grip-calibration.json"
        require(manifest.get("camera_mount_version") == 5 and
                str(manifest.get("camera_grip_calibration_sha256", "")).lower() == CALIBRATION_SHA256,
                "Canonical camera lacks its explicitly sourced native calibration")
        require(hashlib.sha256(calibration_path.read_bytes()).hexdigest() == CALIBRATION_SHA256,
                "Independent native held-pose calibration bytes changed")
        calibration = json.loads(calibration_path.read_bytes())
        source = next(row for row in read_rows(directory / "native-skeleton.jsonl") if row["reference_records"])
        require([source["reference_records"][bone][8:11] for bone in (40, 43, 46, 49)] ==
                calibration["right_knuckle_position_bits"],
                "Native camera calibration belongs to a different anatomical rig")
        contact = calibration_contact(calibration)
    socket_reference = None
    if version == 4:
        manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
        require(manifest.get("camera_mount_version") == 4,
                "Fixed authored mount lacks its explicit native run identity")
        source = next(row for row in read_rows(directory / "native-skeleton.jsonl") if row["reference_records"])
        socket_reference = np.array(struct.unpack("<20f", struct.pack("<20I", *source["reference_records"][52])))
    bodies = {row["serial"]: row for row in read_rows(directory / "native-skeleton.jsonl")} if version in (2,4,5) else {}
    untracked_fallbacks = 0
    positions, axes, palms, matrices, mounts, aiming = [], [], [], [], [], []
    counts = {}
    for row in meshes:
        require(row["asset_name"] == "handycam", "Module published a substitute native model")
        key = (row["component"], row["asset"])
        require(key in identities, "Published camera was not independently observed in actual game memory")
        require(not (row["flags"] & (4 | 0x100000)), "Tracked retail camera is hidden from its owner")
        matrix = np.array(row["world"]).reshape(4, 4)
        matrices.append(float(np.max(np.abs(matrix - np.array(row["expected_world"]).reshape(4, 4)))))
        if row["hand"] >= 0:
            if version in (2, 4, 5):
                expected = fixed_grip_mount(row, grips, contact) if version == 5 else stock_mount(row, grips, socket_reference)
                mounts.append(float(np.max(np.abs(matrix - expected))))
                if version in (4, 5):
                    policy = ("immutable_anatomical_grip_minus_Z_plus_Y" if version == 5 else
                              "immutable_authored_wrist_mount")
                    require(row.get("optical_tracking") == policy,
                            "Fixed camera mount lacks its explicit native policy")
                    actual = (np.append(row["model_grip_point"], 1) @ matrix)[:3]
                    palms.append(float(np.linalg.norm(actual - row["palm_position"])))
            else:
                actual = (np.append(row["model_grip_point"], 1) @ matrix)[:3]
                palms.append(float(np.linalg.norm(actual - row["palm_position"])))
        recovered = optics(row, identities[key])
        if version in (2,4,5) and row["hand"] >= 0 and row["source"] == "body_post_ik":
            body = bodies.get(row.get("native_body_serial"))
            require(body is not None and body["targets"] is not None,
                    "Camera optical aim lacks its exact applied native body evaluation")
            hand = body["targets"]["hands"][row["hand"]]
            require(row.get("controller_tracked") is hand["tracked"],
                    "Camera optical evidence contradicts actual native controller tracking")
            bone = 36 if row["hand"] else 12
            atom = body["atoms"][bone]
            wrist = np.eye(4)
            wrist[:3, :3] = rotation(atom[:4]).T * atom[7]
            wrist[3, :3] = atom[4:7]
            wrist = wrist @ np.array(body["world"]).reshape(4, 4)
            observed = np.array(row["right_wrist_world"] if row["hand"] else row["left_wrist_world"]).reshape(4, 4)
            require(np.max(np.abs(wrist - observed)) < .002,
                    "Camera tracking provenance references a different solved native wrist")
            if hand["tracked"]:
                require(body["targets"]["camera_hand"] == row["hand"] and
                        np.max(np.abs(np.array(row["controller_orientation"]) -
                                      hand["controller_orientation"])) < 1e-6,
                        "Camera optical aim does not use the exact tracked controller applied to its wrist")
                basis = np.column_stack((row["tracking_right"], row["tracking_up"], row["tracking_backward"]))
                controller = rotation(row["controller_orientation"])
                forward_axis, up_axis = ([0, 1, 0], [0, 0, 1]) if grip_facing else ([0, 0, -1], [0, 1, 0])
                if version in (2, 5):
                    aiming.extend((float(np.linalg.norm(recovered["forward"] - basis @ controller @ forward_axis)),
                                   float(np.linalg.norm(recovered["up"] - basis @ controller @ up_axis))))
                if version in (4, 5):
                    require(body["targets"].get("palm_axes_version") == 2 and
                            np.array_equal(hand["orientation"], hand["controller_orientation"]),
                            "Fixed camera grip rotated the native hand away from its anatomical controller pose")
            else:
                require(row["controller_orientation"] is None and body["targets"]["camera_hand"] == -1,
                        "Lost tracking was presented as a valid optical controller pose")
                untracked_fallbacks += 1
        positions.extend([float(np.linalg.norm(recovered["lens"] - row["lens"])),
                          float(np.linalg.norm(recovered["screen"] - row["screen_position"]))
                          * row["units_per_meter"]])
        axes.extend([float(np.linalg.norm(rotation(row["screen_orientation"]) @ [0, 0, -1]
                                          - recovered["screen_forward"])),
                     float(np.linalg.norm(rotation(row["screen_orientation"]) @ [0, 1, 0]
                                          - recovered["screen_up"]))])
        require(np.max(np.abs(recovered["half_extents"] - row["screen_half_extents"][:2])) < 1e-6,
                "LCD dimensions differ from the actual retail mesh")
        counts[(row["source"], row["hand"])] = counts.get((row["source"], row["hand"]), 0) + 1
    require(all(counts.get(("body_post_ik", hand), 0) >= 70 for hand in (-1, 0, 1)),
            "Missing solved retail camera poses in either hand or on chest")
    require(max(matrices) < .002, "Real camera render transform lagged its native attachment")
    if version == 2:
        require(max(mounts) < .002, "Real camera no longer matches its authored native animation mount")
        require(max(aiming) < .003, "Real camera looks behind the physical controller or loses controller roll")
    elif version in (4, 5):
        require(max(mounts) < .005, "Camera deviates from its immutable wrist mount beyond native rotator quantization")
        require(max(palms) < .002, "Fixed camera mount lost its immutable native palm contact")
        if version == 5:
            require(aiming and max(aiming) < .003,
                    "Canonical camera points behind the controller or loses its physical grip roll")
    else:
        require(max(palms) < .002, "Real camera grip is not on the solved native palm")
    require(max(positions) < .002 and max(axes) < .0001,
            "Native LCD/lens pose does not follow actual skinned retail geometry")
    return dict(mesh_observations=len(meshes), independent_observations=observations,
                stock_mount_version=version, maximum_matrix_error=max(matrices),
                maximum_palm_error_native_units=max(palms) if palms else None,
                maximum_stock_mount_error=max(mounts) if mounts else None,
                maximum_controller_optical_error=max(aiming) if aiming else None,
                controller_optical_basis=("immutable anatomical OpenXR grip -Z/+Y" if version == 5 else
                                          "immutable native socket relative to anatomical wrist" if version == 4 else
                                          "OpenXR grip +Y/+Z" if grip_facing else "legacy controller -Z/+Y"),
                verified_untracked_native_fallbacks=untracked_fallbacks,
                maximum_geometry_error_native_units=max(positions), maximum_LCD_axes_error=max(axes),
                solved_poses_by_mount={str(hand): counts[("body_post_ik", hand)] for hand in (-1, 0, 1)})


def optical_sample(directory, sample, views):
    meshes, identities, _ = load(directory)
    view = min(views, key=lambda row: abs(row["frame"] - sample["frame"]))
    require(abs(view["frame"] - sample["frame"]) <= 1, "Missing same-frame actual native lens view")
    candidates = [optics(row, identities[(row["component"], row["asset"])]) for row in meshes
                  if row["source"] == "body_post_ik" and row["hand"] >= 0
                  and abs(row["frame"] - view["frame"]) <= 4]
    require(candidates, "No independently identified real-mesh optical pose for the actual capture")
    errors = [(float(np.linalg.norm(row["lens"] - view["origin"])),
               max(float(np.linalg.norm(row["forward"] + view["backward"])),
                   float(np.linalg.norm(row["up"] - view["up"])))) for row in candidates]
    position, axes = min(errors, key=lambda error: error[0] + error[1] * 100)
    require(position < .02 and axes < .003,
            "Actual engine lens view does not match independently skinned retail lens origin/axes")
    return dict(optical_method="independent_retail_mesh_and_actual_engine_capture_view",
                origin_error_native_units=position, axes_error=axes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    report = grade(args.directory)
    rejected = []
    if args.selftest:
        meshes, identities, _ = load(args.directory)
        faults = ["stale-transform", "wrong-grip", "wrong-lens", "wrong-LCD", "wrong-asset"]
        if report["stock_mount_version"] in (2,4,5):
            faults.extend(("backwards-lens", "unproved-tracking", "wrong-body-evaluation"))
        for fault in faults:
            altered = copy.deepcopy(meshes)
            for row in altered:
                if fault == "stale-transform":
                    row["world"][12] += 5
                elif fault == "wrong-grip":
                    if row.get("stock_mount_version", 1) == 2:
                        row["stock_original_local"][12] += 5
                    else:
                        row["model_grip_point"][0] += 5
                elif fault == "wrong-lens":
                    row["lens"][0] += 5
                elif fault == "wrong-LCD":
                    row["screen_position"][0] += .05
                elif fault == "backwards-lens":
                    if row["controller_orientation"] is not None:
                        x, y, z, w = row["controller_orientation"]
                        row["controller_orientation"] = [-z, w, x, -y]
                elif fault == "unproved-tracking":
                    row["controller_tracked"] = False
                    row["controller_orientation"] = None
                elif fault == "wrong-body-evaluation":
                    row["native_body_serial"] = -1
                else:
                    row["asset_name"] = "placeholder"
            try:
                grade(args.directory, altered, identities)
            except ValueError:
                rejected.append(fault)
            else:
                raise ValueError("Retail mesh oracle accepted " + fault)
    report["rejected_controls"] = rejected
    inputs = ["native-camera-mesh.jsonl", "native-retail-camera-observation.jsonl"]
    if report["stock_mount_version"] in (2,4,5):
        inputs.extend(("native-skeleton.jsonl", "native-run-manifest.json"))
    if report["stock_mount_version"] == 5:
        inputs.append("native-camera-grip-calibration.json")
    report["input_sha256"] = {name: hashlib.sha256((args.directory / name).read_bytes()).hexdigest()
                              for name in inputs}
    report["verifier_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (args.directory / "native-retail-camera-mesh-proof.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
