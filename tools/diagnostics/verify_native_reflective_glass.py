"""Attribute selected glass material contribution separately from native-depth coverage."""

import argparse
from collections import defaultdict
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import struct

import numpy as np

from native_pixel_delta import decode_native_pixel_delta, read_native_pixels
from verify_native_glass import native_hdr_extent, require, sweep_poses
from verify_native_glass_raster import validate_reference_layout, validate_vertex
from verify_native_probe import recommended_extent_grade


PIXEL = "7427e7e3f1dc0ebfd5e7604d9450026354b027e940472aa10422c6ed8faa46e8"
LEGACY_PIXEL = "b9f4a2c34e11ece75a7bca31c9be4c12bf2a9913c55dc45af4b0c639886a4ce8"
PARTICLE_PIXEL = "aaf6160e56e1c3e86778a144f66760f0e24a21e25fc4498d120cb7180967601f"
PARTICLE_GPU_PROGRAMS = {
    "vertex": "0bfb183a8ba672473c75909a51dfa35d59a93f6bc883fdcf78125e025feea43c",
    "pixel": "22bf63ec8a83b4ca6de504dcb64e3eafd885999eb68f5028bb9320ceaebe1daa",
}
COVERAGE_PROGRAM = struct.pack("<11I", 0xffff0300, 0x05000051, 0xa00f0000,
                               0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000,
                               0x02000001, 0x800f0800, 0xa0e40000, 0xffff)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rows(path):
    with path.open(encoding="utf-8-sig") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def rejected(call, name):
    try:
        call()
    except ValueError:
        return name
    raise ValueError("Reflective material oracle accepted: " + name)

def projection_readback_policy(identity, frames):
    version = identity.get("no_projection_images_fixture_version", 0)
    fov = identity.get("asymmetric_fov_fixture_version", 0)
    require(type(version) is int and version in (0, 2) and
            type(fov) is int and fov in (0, 1),
            "Unrecognized material readback or FOV fixture")
    active = [frame for frame in frames if not frame.get("canceled")]
    require(active and all(
        type(frame.get("no_projection_images_fixture_version", 0)) is int and
        frame.get("no_projection_images_fixture_version", 0) == version and
        type(frame.get("asymmetric_fov_fixture_version", 0)) is int and
        frame.get("asymmetric_fov_fixture_version", 0) == fov for frame in active),
        "Material frame and manifest readback/FOV policies disagree")
    if version == 2:
        require(identity["recommended_resolution"] is True and
                identity["headset_extent_2688x2880"] is True and
                identity["draw_pixel_readback"] == "constants_only" and identity["draw_hdr"] is False and
                all(frame.get("captured") is False and frame["eye_width"] == 2688 and
                    frame["eye_height"] == 2880 for frame in active),
                "Bounded material fixture changed its explicit image-readback or native extent policy")
    require(not fov or version == 2, "Asymmetric FOV lacks its versioned material fixture")
    if fov:
        expected = [[-1., .85, .9, -.95], [-.85, 1., .9, -.95]]
        for frame in active:
            require(len(frame["views"]) == 2 and all(
                np.asarray(view["fov"]).shape == (4,) and
                np.allclose(view["fov"], expected[eye], atol=1e-6, rtol=0)
                for eye, view in enumerate(frame["views"])),
                "Actual eye FOV differs from its explicit synthetic asymmetric fixture")
    return dict(no_projection_images_fixture_version=version,
                asymmetric_fov_fixture_version=fov,
                asymmetric_fov_is_synthetic_stress_fixture=bool(fov),
                projection_image_stereo_acceptance_claimed=False)


def material_body_camera_policy(identity, frames):
    version = identity.get("material_body_camera_fixture_version", 0)
    require(type(version) is int and version in (0, 1, 2, 3), "Unknown combined body/camera material fixture")
    stride = identity.get("material_readback_stride", 1)
    require(type(stride) is int and stride == (20 if version >= 2 else 1),
            "Invalid combined material readback cadence")
    active = [frame for frame in frames if not frame.get("canceled")]
    require(active and all(type(frame.get("material_body_camera_fixture_version", 0)) is int and
                           frame.get("material_body_camera_fixture_version", 0) == version and
                           type(frame.get("material_readback_stride", 1)) is int and
                           frame.get("material_readback_stride", 1) == stride
                           for frame in active), "Combined material frame/manifest declarations disagree")
    if not version:
        require(identity.get("body") is False and identity.get("physical_camcorder", False) is False,
                "Undeclared combined body/camera material state")
        return dict(fixture_version=0, combined_floor_pose_policy_validated=False)
    mount_version = identity.get("camera_mount_version")
    require(type(mount_version) is int and mount_version in (4, 5), "Unknown combined native camera mount")
    if mount_version == 5:
        from verify_native_camcorder_mesh import CALIBRATION_SHA256
        require(str(identity.get("camera_grip_calibration_sha256", "")).lower() == CALIBRATION_SHA256,
                "Combined canonical camera lacks its independent native calibration identity")
    require(all(identity.get(key) is True for key in
                ("body", "floor", "physical_camcorder", "native_camera_hud", "headset_extent_2688x2880")) and
            identity.get("reference_eye_ownership_version") == 1 and
            identity.get("physical_camcorder_fixture_version") == (4 if version >= 3 else 3) and
            identity.get("no_projection_images_fixture_version") == 2,
            "Incomplete combined measured-floor body/camera material fixture")
    selected = [frame for frame in active if 320 <= frame.get("gameplay_frame", 0) <= 420]
    require(len(selected) == 101 and {frame["gameplay_frame"] for frame in selected} == set(range(320, 421)),
            "Combined fixture lost its actual moving-head interval")
    for frame in selected:
        require(frame.get("floor_reference_ready") is True and frame.get("floor_reference_acquisitions") == 1 and
                abs(frame.get("standing_reference_m", 0) - 1.7) < 1e-5 and
                type(frame.get("camera_mount_version")) is int and frame["camera_mount_version"] == mount_version and
                frame.get("physical_camcorder_fixture_version") == (4 if version >= 3 else 3),
                "Combined material frame lacks its actual measured floor or physical camera")
        phase = frame["gameplay_frame"] - 300
        center = [.45 * math.sin(phase * .07), 1.7 + .25 * math.sin(phase * .11),
                  .45 * math.sin(phase * .05)]
        require(len(frame["views"]) == 2, "Combined fixture omitted an eye")
        for eye, view in enumerate(frame["views"]):
            x, y, z, w = view["orientation"]
            d = .032 if eye else -.032
            offset = [(1 - 2 * (y*y + z*z)) * d, 2 * (x*y + w*z) * d, 2 * (x*z - w*y) * d]
            position = view["position"]
            require(len(position) == 3 and all(math.isfinite(a) and abs(a-b-c) < 3e-6
                                               for a, b, c in zip(position, center, offset)),
                    "Combined sweep dropped measured floor height or changed submitted roomscale motion")
    return dict(fixture_version=version, combined_floor_pose_policy_validated=True,
                actual_floor_height_m=1.7, submitted_roomscale_frames=len(selected),
                material_readback_stride=stride)


def combined_camera_observations(capture, frames):
    # Camera's counter advances in Present; skeletal publication precedes the next rendered frame.
    selected = {frame["native_frame"] - 1 for frame in frames
                if not frame.get("canceled") and 320 <= frame.get("gameplay_frame", 0) <= 420}
    observed = rows(capture / "native-camera-mesh.jsonl")
    versions = {frame.get("camera_mount_version") for frame in frames if not frame.get("canceled")}
    require(len(versions) == 1 and versions <= {4, 5}, "Combined camera mount declarations disagree")
    mount_version = versions.pop()
    policy = ("immutable_anatomical_grip_minus_Z_plus_Y" if mount_version == 5 else
              "immutable_authored_wrist_mount")
    if mount_version == 5:
        from verify_native_camcorder_mesh import CALIBRATION_SHA256
        path = capture / "native-camera-grip-calibration.json"
        require(path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == CALIBRATION_SHA256,
                "Combined camera lost its independently retained native calibration bytes")
    held = [item for item in observed if item["frame"] in selected and item["source"] == "body_post_ik" and
            item["hand"] in (0, 1) and item["controller_tracked"] is True]
    counts = [sum(item["hand"] == hand for item in held) for hand in (0, 1)]
    require(min(counts) >= 10, "Combined material interval lacks actual left/right held native camera observations")
    witness_hands = {}
    if any(frame.get("material_body_camera_fixture_version") == 3 for frame in frames):
        for world, hand in ((320, 1), (340, 1), (360, -1), (380, -1), (400, 0), (420, 0)):
            native = next(frame["native_frame"] - 1 for frame in frames
                          if not frame.get("canceled") and frame["gameplay_frame"] == world)
            published = [item for item in observed if item["frame"] == native and item["source"] == "body_post_ik"]
            require(published and all(item["hand"] == hand and item["native_body_serial"] > 0 for item in published),
                    "Material witness lacks its actual declared right-held, chest or left-held native camera")
            witness_hands[str(world)] = hand
    for item in held:
        require(item["stock_mount_version"] == mount_version and
                item["optical_tracking"] == policy and item["native_body_serial"] > 0 and
                item["asset_name"] == "handycam" and
                np.asarray(item["world"]).shape == np.asarray(item["expected_world"]).shape == (16,) and
                np.isfinite(item["world"]).all() and np.isfinite(item["expected_world"]).all() and
                np.max(np.abs(np.asarray(item["world"]) - item["expected_world"])) <= .001,
                "Combined material interval has an invalid native camera/body mount")
    return dict(actual_left_held_camera_observations=counts[0],
                actual_right_held_camera_observations=counts[1],
                distinct_native_body_serials=len({item["native_body_serial"] for item in held}),
                actual_native_camera_states_validated=True,
                actual_material_witness_camera_hands=witness_hands)


def viewport_witness(actual, raster, half, world):
    require(actual == raster, "Actual material differs from the original same-raster full-image control")
    native = np.frombuffer(raster, dtype="<u2").reshape(-1, 4)
    reference = np.frombuffer(half, dtype="<u2").reshape(-1, 4)
    require(native.shape == reference.shape, "Native viewport witness extent changed")
    different = native != reference
    return dict(world_frame=world, original_same_raster_equals_actual=True,
                native_to_native_different_rgb_pixels=int(different[:, :3].any(axis=1).sum()),
                native_to_native_different_depth_alpha_pixels=int(different[:, 3].sum()),
                role="different-viewport diagnostic; not a same-raster correctness oracle")


def frame_binding(records, frames, fixture=2, material_stride=1):
    require(type(material_stride) is int and material_stride in (1, 20),
            "Unknown independently declared material cadence")
    worlds = set(range(320, 421))
    selected = [frame for frame in frames if not frame.get("canceled") and
                frame.get("gameplay_frame") in worlds]
    require(len(selected) == 101, "Missing/duplicated actual moving-head projections")
    native = {frame["native_frame"]: frame for frame in selected}
    require(len(native) == 101 and set(native) == set(range(min(native), min(native) + 101)),
            "Actual native frames are not consecutive")
    poses = sweep_poses(selected, set(native), fixture)
    sampled_worlds = set(range(320, 421, material_stride))
    require({item["gameplay_frame"] for item in records} == sampled_worlds,
            "Reflective program is absent from one or more declared sweep frames")
    for index, item in enumerate(records):
        require(item["sample"] == index and item["frame"] in poses and
                item["gameplay_frame"] == poses[item["frame"]]["gameplay_frame"],
                "Reflective evidence has a stale/reordered native frame association")
    require(len({item["actual_sha256"] for item in records}) >= len(sampled_worlds),
            "Reflective diagnostic reused frozen scene images")


def metadata(item, pixel=PIXEL, extent=(7680, 2160)):
    require((item["width"], item["height"]) == extent and
            item["decoded_xor_bytes"] == extent[0] * extent[1] * 8,
            "Reflective diagnostic changed its explicitly declared native HDR resolution")
    require(item["pixel_sha256"] == pixel and
            item["material_contribution_fixture_version"] == (3 if pixel == PARTICLE_PIXEL else 1) and
            item["original_retail_pixel_shader"] and item["native_depth_and_stencil_read_only"] and
            item["diagnostic_reference_eye_draws"] == item["coverage_eye_draws"] == 2 and
            item["production_geometry_submissions"] == 1 and
            item["coverage_native_cull_blend_depth_scissor_retained"] and
            item["coverage_target_cleared_without_clearing_depth"] and
            item["coverage_color"] == [0.25, 0.5, 0.75, 1],
            "Reflective diagnostic changed program, native state or production single-pass scope")
    if pixel == PARTICLE_PIXEL:
        require(item["native_particle_camera_reference"], "Particle reference omitted native camera parameters")
        require(item["reference_eye_seams_isolated"], "Particle reference permits cross-eye geometry")
        require(type(item["particle_camera_view_dependent"]) is bool, "Missing native particle axis-lock policy")


def particle_camera(item, folder):
    prefix = f"temporal_{item['sample']}"

    def floats(name, key, count, identity):
        require(name == prefix + key, "Unrelated particle camera witness")
        data = (folder / name).read_bytes()
        require(len(data) == count * 4 and sha(data) == identity, "Changed particle camera witness")
        result = np.frombuffer(data, dtype="<f4").astype(np.float64)
        require(np.isfinite(result).all(), "Nonfinite particle camera witness")
        return result

    context = floats(item["particle_camera_context"], ".particle-camera-context.f32", 80,
                     item["particle_camera_context_sha256"])
    original = floats(item["particle_camera_originals"], ".particle-camera-originals.f32", 12,
                      item["particle_camera_originals_sha256"]).reshape(3, 4)
    native = context[:9].reshape(3, 3).T
    require(np.allclose(native.T @ native, np.eye(3), atol=2e-5, rtol=0) and context[15] > 0,
            "Invalid native camera basis or world scale")
    require(len(item["particle_camera_reference_files"]) ==
            len(item["particle_camera_reference_sha256"]) == 2, "Missing eye camera witnesses")
    changed, references = [], []
    for eye in range(2):
        actual = floats(item["particle_camera_reference_files"][eye], f".particle-camera-eye-{eye}.f32",
                        12, item["particle_camera_reference_sha256"][eye]).reshape(3, 4)
        view = context[16 + eye * 32:48 + eye * 32]
        projection = view[:16].reshape(4, 4)
        forward = projection[3, :3] / np.linalg.norm(projection[3, :3])
        plane = projection[:2, :3] - np.outer(projection[:2, :3] @ forward, forward)
        plane /= np.linalg.norm(plane, axis=1)[:, None]
        target = np.column_stack((plane.T, -forward))
        require(np.allclose(target.T @ target, np.eye(3), atol=2e-5, rtol=0),
                "Invalid physical eye projection basis")
        expected = original.copy()
        expected[0, :3] += view[16:19] - context[9:12] - context[12:15]
        if item["particle_camera_view_dependent"]:
            expected[1:, :3] = (target @ native.T @ original[1:, :3].T).T
        require(np.allclose(actual, expected, atol=.004, rtol=0),
                "Native particle camera reference is not the per-eye transform of its authored parameters")
        require(np.array_equal(actual[:, 3], original[:, 3]), "Particle camera reference changed native W")
        changed.append(float(np.max(np.abs(actual[1:, :3] - original[1:, :3]))))
        references.append(actual)
    width = item["particle_view_texture_width"]
    require(width in (8, 16), "Unexpected particle GPU table extent")
    uploaded = floats(item["particle_uploaded_views"], ".particle-uploaded-views.f32", width * 8,
                      item["particle_uploaded_views_sha256"]).reshape(2, width, 4)
    uniforms = item["particle_uploaded_uniforms"]
    corrected = bool(uniforms)
    require(not corrected or {(u["register"], u["kind"]) for u in uniforms} ==
            {(17, 5), (18, 11), (19, 11)}, "Unexpected particle camera tracking layout")
    require(len(uniforms) == (3 if corrected else 0), "Duplicate particle GPU camera bindings")
    for eye in range(2):
        view = context[16 + eye * 32:48 + eye * 32]
        require(np.array_equal(uploaded[eye, :4], view[:16].reshape(4, 4).T) and
                np.array_equal(uploaded[eye, 4], view[16:20]), "Actual GPU eye projection/camera differs")
        for uniform in uniforms:
            require(uniform["vertex"] and uniform["rows"] == 1 and 8 <= uniform["row"] < width,
                    "Invalid particle GPU camera binding")
            vector = {17: 0, 18: 1, 19: 2}[uniform["register"]]
            require(np.array_equal(uploaded[eye, uniform["row"]], references[eye][vector]),
                    "Actual GPU camera vector differs from native reference")
    for stage in ("vertex", "pixel"):
        identity = item["actual_" + stage + "_sha256"]
        require(re.fullmatch("[a-f0-9]{64}", identity) and
                sha((folder / ("temporal-program-" + identity + ".bin")).read_bytes()) == identity,
                "Changed actual GPU particle program")
        require(not corrected or identity == PARTICLE_GPU_PROGRAMS[stage],
                "Actual particle program differs from independently reconstructed native patch")
    return changed, corrected


def material_masks(contribution, coverage, width, height):
    require(len(contribution) == len(coverage) == width * height * 8,
            "Invalid material/coverage pixel extent")
    delta = np.frombuffer(contribution, dtype="<u8").reshape(height, width)
    image = np.frombuffer(coverage, dtype="<u8").reshape(height, width)
    color = image & np.uint64(0x0000ffffffffffff)
    covered = color != 0
    require(not np.any((image & np.uint64(0x7c00000000000000)) == np.uint64(0x7c00000000000000)),
            "Nonfinite coverage alpha")
    require(np.all((color == np.uint64(0x00003a0038003400)) | ~covered),
            "Coverage pixels are not the independently defined flat shader color")
    changed_rgb = (delta & np.uint64(0x0000ffffffffffff)) != 0
    changed_rgba = delta != 0
    return covered, changed_rgb, changed_rgba


def validate_record(item, folder, pixel=PIXEL, extent=(7680, 2160)):
    validate_reference_layout(item)
    metadata(item, pixel, extent)
    camera = particle_camera(item, folder) if pixel == PARTICLE_PIXEL else None
    prefix = f"temporal_{item['sample']}"
    programs = {}
    for stage in ("vertex", "pixel", "raster_vertex", "coverage_program"):
        identity = item[stage + "_sha256"]
        require(re.fullmatch("[a-f0-9]{64}", identity), "Invalid retained native program identity")
        data = (folder / ("temporal-program-" + identity + ".bin")).read_bytes()
        require(sha(data) == identity, "Retained GPU program changed")
        programs[stage] = data
    validate_vertex(programs["vertex"], programs["raster_vertex"], item["raster_constant"])
    require(programs["coverage_program"] == COVERAGE_PROGRAM,
            "Geometry coverage shader changes more than its declared flat color")
    matrix_name = prefix + ".local-to-world.f32"
    require(item["local_to_world_file"] == matrix_name, "Unrelated native world-transform source")
    matrix = (folder / matrix_name).read_bytes()
    require(len(matrix) == 64 and sha(matrix) == item["local_to_world_sha256"] and
            np.isfinite(np.frombuffer(matrix, dtype="<f4")).all(), "Changed native world transform")
    size = item["decoded_xor_bytes"]
    zero = bytes(size)
    zero_payload = b"OLVRPD01" + struct.pack("<QI", size, size // 8)
    decoded = {}
    for key, suffix, hash_key in (
        ("xor_image", ".xor.delta", "xor_payload_sha256"),
        ("contribution_xor", ".contribution.xor.delta", "contribution_payload_sha256"),
        ("coverage_image", ".coverage.rgba16f.delta", "coverage_payload_sha256"),
    ):
        require(item[key] == prefix + suffix, "Unrelated material/coverage payload")
        path = folder / item[key]
        require(path.stat().st_size <= 16 + size * 3 // 2, "Unbounded pixel payload")
        payload = path.read_bytes()
        require(sha(payload) == item[hash_key], "Changed material/coverage payload")
        decoded[key] = zero if payload == zero_payload else decode_native_pixel_delta(payload, zero, size)
    parity_bits = 0 if decoded["xor_image"] is zero else int(
        np.count_nonzero(np.frombuffer(decoded["xor_image"], dtype=np.uint8)))
    equal = parity_bits == 0
    require(item["byte_exact"] == equal and
            (item["actual_sha256"] == item["raster_sha256"]) == equal,
            "Declared shader parity disagrees with independently decoded XOR")
    per_eye = []
    if decoded["contribution_xor"] is zero and decoded["coverage_image"] is zero:
        per_eye = [dict(coverage_pixels=0, actual_changed_rgb_pixels=0,
                        actual_changed_rgba_pixels=0, changed_rgb_outside_coverage=0) for _ in range(2)]
    else:
        coverage, rgb, rgba = material_masks(decoded["contribution_xor"], decoded["coverage_image"],
                                             item["width"], item["height"])
        outside = rgb & ~coverage
        for eye in range(2):
            region = np.s_[:, eye * item["width"] // 2:(eye + 1) * item["width"] // 2]
            per_eye.append(dict(coverage_pixels=int(coverage[region].sum()),
                                actual_changed_rgb_pixels=int(rgb[region].sum()),
                                actual_changed_rgba_pixels=int(rgba[region].sum()),
                                changed_rgb_outside_coverage=int(outside[region].sum())))
    result = dict(sample=item["sample"], world_frame=item["gameplay_frame"],
                local_to_world=list(struct.unpack("<16f", matrix)),
                local_to_world_sha256=sha(matrix), original_shader_equal=equal,
                different_original_shader_bytes=parity_bits, eyes=per_eye)
    if camera is not None:
        result["native_camera_direction_change_each_eye"] = camera[0]
        result["actual_gpu_camera_correction_present"] = camera[1]
    return result, decoded


def grade(capture, module, probe, selftest, pixel=PIXEL):
    identity = json.loads((capture / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    combined = identity.get("material_body_camera_fixture_version", 0)
    emitter_facing = identity.get("particle_emitter_facing_fixture_version", 0)
    require(emitter_facing in (0, 1) and (not emitter_facing or pixel == PARTICLE_PIXEL),
            "Emitter-facing sweep is restricted to its versioned particle fixture")
    fixture = 3 if emitter_facing else 2
    require((capture / "capture-complete.txt").is_file() and
            identity["glass_reference_fixture_version"] == (8 if pixel == PARTICLE_PIXEL else 6) and
            identity["temporal_glass_fixture_version"] == (4 if pixel == PARTICLE_PIXEL else 2) and
            identity["glass_material_contribution_fixture_version"] == (3 if pixel == PARTICLE_PIXEL else 1) and
            identity.get("glass_material_sha256", PIXEL) == pixel and
            identity["window_position_fixture_version"] == 1 and
            identity["window_sweep_fixture_version"] == fixture and identity["head_turn_pitch_degrees"] == -15 and
            identity["body"] is bool(combined) and identity["draw_frame"] == 600 and
            identity["mod_sha256"] == sha(module.read_bytes()).upper() and
            identity["probe_sha256"] == sha(probe.read_bytes()).upper(),
            "Not a completed exact-module reflective contribution fixture")
    folder = capture / "native-glass-reference"
    extent = native_hdr_extent(identity)
    records = rows(folder / "temporal-references.jsonl")
    ownership_version = identity.get("reference_eye_ownership_version", 0)
    require(type(ownership_version) is int and ownership_version in (0, 1),
            "Invalid manifest original-reference ownership version")
    for item in records:
        validate_reference_layout(item, ownership_version)
    frames = rows(capture / "frames.jsonl")
    combined_policy = material_body_camera_policy(identity, frames)
    if combined:
        combined_policy.update(combined_camera_observations(capture, frames))
    material_stride = combined_policy.get("material_readback_stride", 1)
    require(all(type(item.get("material_readback_stride", 1)) is int and
                item.get("material_readback_stride", 1) == material_stride for item in records),
            "Original-material reference cadence differs from submitted native fixture")
    frame_binding(records, frames, fixture, material_stride)
    readback = projection_readback_policy(identity, frames)
    if readback["no_projection_images_fixture_version"] == 2:
        readback["native_submission_grade"] = recommended_extent_grade(
            [frame for frame in frames if not frame.get("canceled")],
            identity.get("diagnostic_eye_size", 1024), identity["headset_extent_2688x2880"])
        require(readback["native_submission_grade"]["native_gameplay_projections"] >= 900,
                "Bounded material fixture lacks its native gameplay submissions")
    full = rows(folder / "references.jsonl")
    require(len(full) >= 6 and all(item["sample"] == index and item["prefix"] == f"sample_{index}"
                                  for index, item in enumerate(full)), "Missing/reordered full-image witnesses")
    results, linked, native_witnesses, viewport_diagnostics = [], 0, [], []
    for item in records:
        result, decoded = validate_record(item, folder, pixel, extent)
        results.append(result)
        if (item["gameplay_frame"] - 320) % 20 == 0:
            require(linked < len(full), "Continuous contribution lacks its full-image witness")
            witness = full[linked]
            require(type(witness.get("material_readback_stride", 1)) is int and
                    witness.get("material_readback_stride", 1) == material_stride,
                    "Full original-material witness changed its independently declared cadence")
            validate_reference_layout(witness, ownership_version)
            require(witness["frame"] == item["frame"] and witness["after_sha256"] == item["actual_sha256"] and
                    witness["vertex_sha256"] == item["vertex_sha256"] and
                    witness["before_sha256"] == item["before_sha256"] and
                    witness["raster_control_sha256"] == item["raster_sha256"] and
                    witness["pixel_sha256"] == pixel and witness["diagnostic_only_control_draws"] == 6,
                    "Full-image provenance disagrees with continuous material contribution")
            images = {}
            for role in (("before", "after", "raster_control", "reference") if pixel == PARTICLE_PIXEL else
                         ("before", "after", "raster_control")):
                data = read_native_pixels(folder, witness["prefix"], role, witness["pixel_storage"],
                                          item["decoded_xor_bytes"])
                require(sha(data) == witness[role + "_sha256"], "Changed full-resolution native witness")
                images[role] = data
            if pixel == PARTICLE_PIXEL:
                native_witnesses.append(images["reference"] == images["after"])
                viewport_diagnostics.append(viewport_witness(
                    images["after"], images["raster_control"], images["reference"], item["gameplay_frame"]))
            actual_xor = np.bitwise_xor(np.frombuffer(images["before"], dtype=np.uint8),
                                       np.frombuffer(images["after"], dtype=np.uint8))
            require(actual_xor.tobytes() == decoded["contribution_xor"],
                    "Contribution XOR is not the actual full-image before-to-after difference")
            raster_xor = np.bitwise_xor(np.frombuffer(images["raster_control"], dtype=np.uint8),
                                       np.frombuffer(images["after"], dtype=np.uint8))
            require(raster_xor.tobytes() == decoded["xor_image"],
                    "Parity XOR is not the actual full-image raster-to-after difference")
            linked += 1
    require(linked == len(full), "Unassociated full-image material witness")
    groups = defaultdict(list)
    for item in results:
        groups[item["local_to_world_sha256"]].append(item)
    summary = []
    for matrix, samples in groups.items():
        summary.append(dict(world_transform_sha256=matrix, local_to_world=samples[0]["local_to_world"],
                            world_frames=sorted({item["world_frame"] for item in samples}),
                            eyes=[dict(
                                minimum_coverage_pixels=min(item["eyes"][eye]["coverage_pixels"] for item in samples),
                                maximum_coverage_pixels=max(item["eyes"][eye]["coverage_pixels"] for item in samples),
                                minimum_changed_rgb_pixels=min(item["eyes"][eye]["actual_changed_rgb_pixels"] for item in samples),
                                maximum_changed_rgb_pixels=max(item["eyes"][eye]["actual_changed_rgb_pixels"] for item in samples),
                                covered_but_no_rgb_change_samples=sum(
                                    item["eyes"][eye]["coverage_pixels"] > 0 and
                                    item["eyes"][eye]["actual_changed_rgb_pixels"] == 0 for item in samples))
                                  for eye in range(2)]))
    controls = []
    if selftest:
        if combined:
            bad = copy.deepcopy(identity)
            bad["floor"] = False
            controls.append(rejected(lambda: material_body_camera_policy(bad, frames),
                                     "missing-combined-measured-floor"))
            bad_frames = copy.deepcopy(frames)
            next(frame for frame in bad_frames if not frame.get("canceled") and
                 frame.get("gameplay_frame") == 350)["views"][0]["position"][1] -= 1.7
            controls.append(rejected(lambda: material_body_camera_policy(identity, bad_frames),
                                     "lost-combined-submitted-floor-height"))
        if ownership_version:
            for key, value in (
                ("reference_eye_ownership_version", 0),
                ("reference_eye_scissors", [[0, 0, extent[0], extent[1]]] * 2),
                ("reference_raster_mappings", [[1, 0, 0, 0]] * 2),
            ):
                bad = copy.deepcopy(records[0])
                bad[key] = value
                controls.append(rejected(lambda: validate_reference_layout(bad, ownership_version),
                                         "altered-original-reference-" + key))
        if readback["no_projection_images_fixture_version"] == 2:
            bad = copy.deepcopy(identity)
            bad["draw_pixel_readback"] = "images"
            controls.append(rejected(lambda: projection_readback_policy(bad, frames),
                                     "false-material-readback-policy"))
            bad_frames = copy.deepcopy(frames)
            next(frame for frame in bad_frames if not frame.get("canceled"))["captured"] = True
            controls.append(rejected(lambda: projection_readback_policy(identity, bad_frames),
                                     "undeclared-projection-image-readback"))
            bad_frames = copy.deepcopy([frame for frame in frames if not frame.get("canceled")])
            bad_frames[-1]["direct_xr_submissions"] = 0
            controls.append(rejected(lambda: recommended_extent_grade(bad_frames, None, True),
                                     "missing-native-direct-atlas-submission"))
            bad_frames = copy.deepcopy([frame for frame in frames if not frame.get("canceled")])
            bad_frames[0]["source_width"] += 1
            controls.append(rejected(lambda: recommended_extent_grade(bad_frames, None, True),
                                     "wrong-native-material-source-extent"))
        if readback["asymmetric_fov_fixture_version"]:
            bad_frames = copy.deepcopy(frames)
            next(frame for frame in bad_frames if not frame.get("canceled"))["views"][0]["fov"][0] = -.8
            controls.append(rejected(lambda: projection_readback_policy(identity, bad_frames),
                                     "incorrect-actual-asymmetric-fov"))
        if pixel == PARTICLE_PIXEL:
            bad = copy.deepcopy(records[0])
            bad["reference_eye_seams_isolated"] = False
            controls.append(rejected(lambda: metadata(bad, pixel, extent), "cross-eye-reference-spill"))
            bad = copy.deepcopy(records[0])
            bad["particle_uploaded_views_sha256"] = "0" * 64
            controls.append(rejected(lambda: particle_camera(bad, folder), "changed-actual-gpu-camera-upload"))
            if records[0]["particle_uploaded_uniforms"]:
                bad = copy.deepcopy(records[0])
                bad["particle_uploaded_uniforms"][0]["row"] = bad["particle_uploaded_uniforms"][1]["row"]
                controls.append(rejected(lambda: particle_camera(bad, folder), "wrong-actual-gpu-camera-row"))
                bad = copy.deepcopy(records[0])
                bad["particle_camera_view_dependent"] = not bad["particle_camera_view_dependent"]
                controls.append(rejected(lambda: particle_camera(bad, folder), "incorrect-native-axis-lock-policy"))
                bad = copy.deepcopy(records[0])
                bad["actual_vertex_sha256"] = bad["vertex_sha256"]
                controls.append(rejected(lambda: particle_camera(bad, folder), "original-shader-instead-of-stereo-program"))
        for key, value in (("production_geometry_submissions", 2), ("pixel_sha256", "0" * 64),
                           ("coverage_native_cull_blend_depth_scissor_retained", False),
                           ("coverage_target_cleared_without_clearing_depth", False)):
            bad = copy.deepcopy(records[0])
            bad[key] = value
            controls.append(rejected(lambda: metadata(bad, pixel, extent), key))
        controls.append(rejected(lambda: frame_binding(
            [item for item in records if item["gameplay_frame"] != records[len(records)//2]["gameplay_frame"]],
            frames, fixture, material_stride), "missing-native-frame"))
        bad = copy.deepcopy(records)
        for item in bad:
            item["actual_sha256"] = bad[0]["actual_sha256"]
        controls.append(rejected(lambda: frame_binding(bad, frames, fixture, material_stride), "frozen-scene-images"))
        controls.append(rejected(lambda: material_masks(bytes(16), bytes(8), 2, 1), "truncated-pixels"))
        bad_color = struct.pack("<8e", 1, 0, 0, 1, 0, 0, 0, 0)
        controls.append(rejected(lambda: material_masks(bytes(16), bad_color, 2, 1), "false-coverage-color"))
        coverage, rgb, rgba = material_masks(struct.pack("<8H", 0, 0, 0, 1, 0, 0, 0, 0),
                                             struct.pack("<8e", .25, .5, .75, 1, 0, 0, 0, 0), 2, 1)
        require(coverage.sum() == 1 and rgb.sum() == 0 and rgba.sum() == 1,
                "Alpha-only writes were incorrectly accepted as visible RGB material")
        controls.append("alpha-only-is-not-visible-rgb")
    equal = all(item["original_shader_equal"] for item in results)
    outside = sum(eye["changed_rgb_outside_coverage"] for item in results for eye in item["eyes"])
    visible = [any(item["eyes"][eye]["actual_changed_rgb_pixels"] for item in results) for eye in range(2)]
    if pixel == PARTICLE_PIXEL:
        require(all(max(item["native_camera_direction_change_each_eye"]) > .05 for item in results),
                "Particle fixture never exercises a meaningful native-to-head camera rotation")
    return dict(evidence_integrity_passed=True, full_resolution=list(extent), sweep_fixture_version=fixture,
                material_readback_policy=readback,
                material_body_camera_policy=combined_policy,
                reference_eye_ownership_version=ownership_version,
                original_reference_eye_bounds_independently_validated=bool(ownership_version),
                particle_emitter_facing_fixture_version=emitter_facing, original_same_raster_shader_equal=equal,
                original_half_viewport_witnesses_equal=all(native_witnesses) if native_witnesses else None,
                original_same_raster_full_witnesses_equal=all(
                    item["original_same_raster_equals_actual"] for item in viewport_diagnostics)
                    if viewport_diagnostics else None,
                native_to_native_viewport_diagnostics=viewport_diagnostics,
                actual_gpu_camera_correction_present=all(
                    item.get("actual_gpu_camera_correction_present", False) for item in results),
                actual_rgb_contribution_observed_in_each_eye=visible,
                actual_changed_rgb_outside_control_coverage=outside,
                frames=101, material_sampled_frames=len({item["world_frame"] for item in results}),
                material_readback_stride=material_stride, draws=len(results), full_image_witnesses=linked,
                geometry_groups=summary, per_draw=results, rejected_controls=controls,
                release_ready=False, specific_headset_disappearance_reproduced=False,
                limitations=["This is material attribution, not a glass-disappearance fix.",
                             "Flat coverage retains native depth/culling but intentionally replaces material shading.",
                             "Matrices identify captured draw groups, not a proven missing retail primitive.",
                             "No physical ergonomics or live-headset performance verdict."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--material", choices=("legacy", "reflective", "particle"), default="reflective")
    parser.add_argument("--require-corrected", action="store_true")
    args = parser.parse_args()
    result = grade(args.capture, args.module, args.probe, args.selftest,
                   dict(legacy=LEGACY_PIXEL, reflective=PIXEL, particle=PARTICLE_PIXEL)[args.material])
    print(json.dumps(result, indent=2))
    if args.require_corrected:
        require(result["original_same_raster_shader_equal"] and
                (args.material != "particle" or (result["actual_gpu_camera_correction_present"] and
                 result["original_same_raster_full_witnesses_equal"])) and
                all(result["actual_rgb_contribution_observed_in_each_eye"]) and
                result["actual_changed_rgb_outside_control_coverage"] == 0,
                "Corrected renderer does not match its independently retained native shader reference")


if __name__ == "__main__":
    main()
