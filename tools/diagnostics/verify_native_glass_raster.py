"""Attribute glass differences without changing the original half-viewport oracle."""

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from native_pixel_delta import RAW, read_native_pixels

from sm3 import instructions, register_index, register_type, resources
from verify_native_glass import ABSOLUTE_HDR_TOLERANCE, PIXEL_SHADER, compare, native_hdr_extent, require, select_sweep_frames, sweep_poses


def validate_reference_layout(record, expected=None):
    version = record.get("reference_eye_ownership_version", 0)
    require(type(version) is int and version in (0, 1) and
            (expected is None or version == expected),
            "Original reference eye ownership version changed")
    keys = ("reference_source_viewport", "reference_native_scissor_enabled",
            "reference_tracked_scissors", "reference_eye_scissors", "reference_raster_mappings")
    if not version:
        require(not any(key in record for key in keys), "Undeclared original-reference eye bounds")
        return False
    width, height = record["width"], record["height"]
    viewport = record["reference_source_viewport"]
    require(type(width) is int and type(height) is int and width > 0 and width % 2 == 0 and
            height > 0 and len(viewport) == 4 and all(type(x) is int for x in viewport),
            "Invalid reference viewport extent")
    x, y, w, h = viewport
    require(x >= 0 and x % 2 == 0 and y >= 0 and w > 0 and w % 2 == 0 and h > 0 and
            x + w <= width and y + h <= height,
            "Reference viewport is outside the native atlas")
    enabled = record["reference_native_scissor_enabled"]
    require(type(enabled) is bool, "Invalid native reference scissor state")
    tracked, scissors, mappings = [record[key] for key in keys[2:]]
    require(all(len(values) == 2 for values in (tracked, scissors, mappings)),
            "Reference omitted one eye")
    for eye in range(2):
        native, actual, mapping = tracked[eye], scissors[eye], mappings[eye]
        require(len(native) == len(actual) == len(mapping) == 4 and
                all(type(value) is int for value in native + actual) and
                native[0] <= native[2] and native[1] <= native[3],
                "Invalid original-reference eye rectangle")
        left, top, right, bottom = x // 2, y, (x + w) // 2, y + h
        if enabled:
            left = min(right, max(left, native[0]))
            top = min(bottom, max(top, native[1]))
            right = max(left, min(right, native[2]))
            bottom = max(top, min(bottom, native[3]))
        offset = eye * (width // 2)
        require(actual == [left + offset, top, right + offset, bottom],
                "Original reference spills into another eye or changes native clipping")
        scale = w / (2 * width)
        expected_mapping = [scale, x / width + scale - 1 + eye, 0, 0]
        require(all(type(a) in (int, float) and math.isfinite(a) and abs(a - b) <= 1e-7
                    for a, b in zip(mapping, expected_mapping)),
                "Original reference changed its native viewport mapping")
    return True


def validate_vertex(original, control, constant):
    require(control[:4] == original[:4] == b"\x00\x03\xfe\xff",
            "Raster control changed the original vertex shader version")
    layout = resources(original)
    positions = [d["register"] for d in layout["declarations"]
                 if d["register_type"] == 6 and d["usage"] == d["usage_index"] == 0]
    require(len(positions) == 1 and layout["free_temps"] and layout["free_constants"],
            "Original glass lacks independent raster-control registers")
    position, temporary = positions[0], layout["free_temps"][0]
    require(constant == layout["free_constants"][0], "Raster control overwrites a native constant")
    expected, writes = [], 0
    for opcode, words in instructions(original):
        words = list(words)
        if opcode not in (65534, 31, 47, 48, 81) and len(words) > 1 and \
                register_type(words[1]) == 6 and register_index(words[1]) == position:
            words[1] = (words[1] & ~(0x70001800 | 2047)) | temporary
            writes += 1
        expected.append((opcode, tuple(words)))
    require(writes > 0, "Original glass never writes its declared position")
    temp, uniform = 0x80000000 | temporary, 0xA0000000 | constant
    expected.extend([
        (5, (0x03000005, temp | 0x10000, temp, uniform)),
        (4, (0x04000004, temp | 0x10000, temp | 0xFF0000, uniform | 0x550000, temp)),
        (1, (0x02000001, 0xE00F0000 | position, temp | 0xE40000)),
    ])
    require(list(instructions(control)) == expected,
            "Raster control changes native instructions beyond the final position mapping")


def difference(left, right):
    require(left.shape == right.shape and left.ndim == 3 and left.shape[2] == 4,
            "Raster-control extents disagree")
    left, right = left[:, :, :3].astype(np.float32), right[:, :, :3].astype(np.float32)
    require(np.isfinite(left).all() and np.isfinite(right).all(), "Nonfinite raster-control RGB")
    delta = np.max(np.abs(left - right), axis=2)
    return {
        "maximum_hdr_error": float(delta.max()),
        "pixels_above_original_threshold": int(np.count_nonzero(delta > ABSOLUTE_HDR_TOLERANCE)),
        "different_rgb_values": int(np.count_nonzero(left != right)),
    }


def verify(root, module, probe, selftest=False, require_same_raster=False):
    manifest = json.loads((root / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    version = manifest["glass_reference_fixture_version"]
    fixture = 2 if version == 5 else 1
    require(version in (4, 5) and manifest["window_sweep"] and
            manifest["window_sweep_fixture_version"] == fixture and manifest["draw_frame"] == 600 and
            manifest["head_turn_pitch_degrees"] == -15 and not manifest["body"] and
            not manifest["physical_camcorder"], "Not the explicit raster-discrimination fixture")
    require((root / "capture-complete.txt").is_file(), "Native raster capture did not finish")
    for name, path in (("mod_sha256", module), ("probe_sha256", probe)):
        require(hashlib.sha256(path.read_bytes()).hexdigest().upper() == manifest[name],
                "Raster evidence belongs to a different native binary")
    with (root / "frames.jsonl").open() as stream:
        frames, required_frames = select_sweep_frames((json.loads(line) for line in stream), fixture)
    poses = sweep_poses(frames, required_frames, fixture)
    folder = root / "native-glass-reference"
    records = [json.loads(line) for line in (folder / "references.jsonl").read_text().splitlines()]
    require(records and {x["frame"] for x in records} == required_frames,
            "Raster discriminator omitted a required native frame")
    results, controls, pixel_controls, native_controls, equivalent_controls = [], 0, 0, 0, 0
    for index, record in enumerate(records):
        validate_reference_layout(record, manifest.get("reference_eye_ownership_version", 0))
        require(record["sample"] == index and record["prefix"] == f"sample_{index}" and
                record["pixel_sha256"] == PIXEL_SHADER and record["position_only_raster_control"] and
                record["original_retail_shaders"] and record["native_depth_retained"] and
                record["native_depth_and_stencil_read_only"] and record["tracked_native_world_scissor"] and
                record["diagnostic_reference_draws"] == 2 and record["diagnostic_only_control_draws"] == 4,
                "Native raster-control provenance missing")
        if version == 5:
            require(record["gameplay_frame"] == poses[record["frame"]]["gameplay_frame"],
                    "Raster-control pixels belong to a different native gameplay sample")
        prefix = record["prefix"]
        code = {}
        for stage in ("vertex", "pixel", "raster_vertex"):
            code[stage] = (folder / f"{prefix}_{stage}.bin").read_bytes()
            require(hashlib.sha256(code[stage]).hexdigest() == record[f"{stage}_sha256"],
                    "Native raster-control shader identity changed")
        require(code["pixel"].startswith(b"\x00\x03\xff\xff") and
                code["vertex"].startswith(b"\x00\x03\xfe\xff"),
                "Original native shader versions changed")
        validate_vertex(code["vertex"], code["raster_vertex"], record["raster_constant"])
        width, height = record["width"], record["height"]
        require((width, height) == native_hdr_extent(manifest),
                "Raster-control target differs from its explicit native HDR extent")
        images = {}
        for name in ("before", "after", "reference", "raster_control", "legacy_scissor_control"):
            payload = read_native_pixels(folder, prefix, name, record.get("pixel_storage", RAW),
                                         width * height * 8)
            require(len(payload) == width * height * 8 and
                    hashlib.sha256(payload).hexdigest() == record[f"{name}_sha256"],
                    "Changed or truncated native raster-control pixels")
            images[name] = np.frombuffer(payload, dtype="<f2").reshape(height, width, 4)
        result = {"sample": index, "frame": record["frame"]}
        for label, left, right in (("stereo_vs_half_viewport", "after", "reference"),
                                   ("raster_vs_half_viewport", "raster_control", "reference"),
                                   ("stereo_vs_raster", "after", "raster_control")):
            result[label] = difference(images[left], images[right])
        result["changed_pixels"] = [
            int(np.any(images["after"][:, eye * width // 2:(eye + 1) * width // 2, :3] !=
                       images["before"][:, eye * width // 2:(eye + 1) * width // 2, :3], axis=2).sum())
            for eye in range(2)]
        if require_same_raster:
            compare(images["before"], images["after"], images["raster_control"])
            if max(result["changed_pixels"]) >= 128:
                try:
                    compare(images["before"], images["raster_control"], images["legacy_scissor_control"])
                except ValueError:
                    try:
                        compare(images["before"], images["after"], images["legacy_scissor_control"])
                    except ValueError:
                        native_controls += 1
                    else:
                        raise ValueError("Same-raster oracle accepted a genuinely different native GPU control")
                else:
                    equivalent_controls += 1
                if selftest:
                    for actual, expected in (
                        (images["after"], images["before"]),
                        (images["before"], images["raster_control"]),
                        (images["after"], images["raster_control"] + np.float16(.025)),
                        (images["after"], np.full_like(images["raster_control"], np.nan)),
                    ):
                        try:
                            compare(images["before"], actual, expected)
                        except ValueError:
                            pixel_controls += 1
                            continue
                        raise ValueError("Same-raster oracle accepted altered or omitted native glass")
        results.append(result)
        if selftest and index == 0:
            wrong_tail = bytearray(code["raster_vertex"])
            wrong_tail[-8] ^= 1
            for damaged in (bytes(wrong_tail), code["raster_vertex"][:-4], code["vertex"]):
                try:
                    validate_vertex(code["vertex"], damaged, record["raster_constant"])
                except ValueError:
                    controls += 1
                    continue
                raise ValueError("Raster discriminator accepted a changed native vertex program")
    for frame in required_frames:
        require(all(sum(x["changed_pixels"][eye] for x in results if x["frame"] == frame) >= 128
                    for eye in range(2)), "Raster discriminator has no substantial visible glass")
    if require_same_raster:
        require(native_controls > 0 and (not selftest or pixel_controls >= 4),
                "Same-raster acceptance lacks a genuine native GPU counterexample")
    return {
        "module_sha256": manifest["mod_sha256"], "probe_sha256": manifest["probe_sha256"],
        "diagnostic_analysis_only": True, "release_acceptance": False,
        "temporal_window_flicker_acceptance": False, "physical_headset_acceptance": False,
        "absolute_hdr_tolerance_unchanged": ABSOLUTE_HDR_TOLERANCE,
        "same_raster_original_pixel_shader_acceptance": require_same_raster,
        "original_half_viewport_acceptance": all(
            x["stereo_vs_half_viewport"]["maximum_hdr_error"] <= ABSOLUTE_HDR_TOLERANCE for x in results),
        "native_gpu_counterexamples_rejected": native_controls,
        "equivalent_legacy_scissor_controls_not_counterexamples": equivalent_controls,
        "pixel_corruption_controls": pixel_controls,
        "submitted_poses": poses, "vertex_corruption_controls": controls, "samples": results,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--require-same-raster", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.root, args.module, args.probe, args.selftest,
                                args.require_same_raster), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f"FAIL: {error}\n")
