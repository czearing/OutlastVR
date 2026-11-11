"""Check every actual native glass draw across 101 consecutive moving-head frames.

Complete all-byte XOR evidence is losslessly encoded against zero. Full HDR
inputs and original GPU controls remain retained at the original six poses.
This covers the specified native lobby path, not arbitrary levels or headset UX.
"""
import argparse
from collections import Counter
import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile

from native_pixel_delta import decode_native_pixel_delta
from verify_native_glass import PIXEL_SHADER, native_hdr_extent, require, sweep_poses
from verify_native_glass_raster import validate_reference_layout, validate_vertex


def sha(payload):
    return hashlib.sha256(payload).hexdigest()


def validate_record(record, folder, decoded_cache, extent=(7680, 2160)):
    validate_reference_layout(record)
    width, height = record["width"], record["height"]
    require((width, height) == extent and record["decoded_xor_bytes"] == width * height * 8,
            "Continuous glass did not use its explicitly declared full-resolution native HDR atlas")
    require(record["original_retail_pixel_shader"] and record["native_depth_and_stencil_read_only"] and
            record["diagnostic_reference_eye_draws"] == 2 and record["production_geometry_submissions"] == 1,
            "Continuous glass changed native depth, reference scope or production single-pass submission")
    require(record["pixel_sha256"] == PIXEL_SHADER, "Continuous glass used the wrong native pixel program")
    programs = {}
    for stage in ("vertex", "pixel", "raster_vertex"):
        identity = record[stage + "_sha256"]
        require(len(identity) == 64 and all(char in "0123456789abcdef" for char in identity),
                "Invalid native program identity")
        program = folder / ("temporal-program-" + identity + ".bin")
        programs[stage] = program.read_bytes()
        require(sha(programs[stage]) == identity, "Retained native GPU program changed")
    validate_vertex(programs["vertex"], programs["raster_vertex"], record["raster_constant"])
    name = f"temporal_{record['sample']}.xor.delta"
    require(record["xor_image"] == name, "Continuous XOR evidence has an unrelated source identity")
    payload = (folder / name).read_bytes()
    require(len(payload) <= 16 + record["decoded_xor_bytes"] * 3 // 2 and
            sha(payload) == record["xor_payload_sha256"], "Continuous XOR evidence changed")
    key = (record["decoded_xor_bytes"], payload)
    if key not in decoded_cache:
        decoded = decode_native_pixel_delta(payload, bytes(record["decoded_xor_bytes"]),
                                            record["decoded_xor_bytes"])
        decoded_cache[key] = decoded.count(0) == len(decoded)
    require(decoded_cache[key] and record["byte_exact"] and
            record["actual_sha256"] == record["raster_sha256"] and
            len(record["actual_sha256"]) == 64,
            "Actual continuous native glass differs from its original same-raster GPU reference")


def validate_coverage(records, frames):
    expected = set(range(320, 421))
    observed = Counter(record["gameplay_frame"] for record in records)
    require(set(observed) == expected, "Continuous native glass lost one or more gameplay frames")
    selected = [frame for frame in frames if not frame.get("canceled") and
                frame.get("gameplay_frame") in expected]
    require(len(selected) == 101 and {frame["gameplay_frame"] for frame in selected} == expected,
            "Actual XR submission stream omitted or duplicated a continuous gameplay frame")
    native = {frame["native_frame"]: frame for frame in selected}
    require(len(native) == 101 and set(native) == set(range(min(native), min(native) + 101)),
            "Continuous native render frames are not consecutive")
    poses = sweep_poses(selected, set(native), 2)
    for record in records:
        require(record["frame"] in native and
                poses[record["frame"]]["gameplay_frame"] == record["gameplay_frame"],
                "Continuous image belongs to the wrong actual native/XR frame")
    require(len({record["raster_sha256"] for record in records}) >= 101,
            "Continuous native references replayed frozen images instead of moving-head output")
    return observed, poses


def grade(directory, module, probe, selftest):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require((directory / "capture-complete.txt").is_file() and
            manifest.get("temporal_glass_fixture_version") == 1 and manifest["window_sweep"] and
            manifest["head_turn_pitch_degrees"] == -15 and not manifest["body"] and
            manifest["mod_sha256"] == sha(module.read_bytes()).upper() and
            manifest["probe_sha256"] == sha(probe.read_bytes()).upper(),
            "Continuous glass does not identify the completed exact-module native fixture")
    folder = directory / "native-glass-reference"
    extent = native_hdr_extent(manifest)
    records = [json.loads(line) for line in (folder / "temporal-references.jsonl").open()]
    frames = [json.loads(line) for line in (directory / "frames.jsonl").open()]
    observed, poses = validate_coverage(records, frames)
    cache = {}
    for index, record in enumerate(records):
        require(record["sample"] == index, "Missing or reordered continuous native draw evidence")
        validate_reference_layout(record, manifest.get("reference_eye_ownership_version", 0))
        validate_record(record, folder, cache, extent)
    full = [json.loads(line) for line in (folder / "references.jsonl").open()]
    full_keys = Counter((row["frame"], row["after_sha256"], row["raster_control_sha256"],
                         row["vertex_sha256"], row["pixel_sha256"]) for row in full)
    temporal_keys = Counter((row["frame"], row["actual_sha256"], row["raster_sha256"],
                             row["vertex_sha256"], row["pixel_sha256"]) for row in records
                            if (row["gameplay_frame"] - 320) % 20 == 0)
    require(full_keys == temporal_keys and len(full) >= 6,
            "Continuous XOR observations disagree with retained full-image six-pose evidence")
    rejected = []
    if selftest:
        for fault in ("omit-frame", "wrong-frame", "frozen-images", "dual-pass", "corrupt-digest",
                      "wrong-pixel", "vertex-mutation", "nonzero-xor"):
            damaged = copy.deepcopy(records)
            if fault == "omit-frame":
                damaged = [row for row in damaged if row["gameplay_frame"] != 370]
            elif fault == "wrong-frame":
                damaged[0]["frame"] += 1
            elif fault == "frozen-images":
                for row in damaged:
                    row["raster_sha256"] = damaged[0]["raster_sha256"]
            elif fault == "dual-pass":
                damaged[0]["production_geometry_submissions"] = 2
            elif fault == "corrupt-digest":
                damaged[0]["actual_sha256"] = "0" * 64
            elif fault == "wrong-pixel":
                damaged[0]["pixel_sha256"] = "0" * 64
            elif fault == "vertex-mutation":
                damaged[0]["raster_constant"] += 1
            else:
                damaged[0]["byte_exact"] = False
            try:
                validate_coverage(damaged, frames)
                validate_record(damaged[0], folder, cache, extent)
            except ValueError:
                rejected.append(fault)
            else:
                raise ValueError("Continuous native oracle accepted corruption: " + fault)
        with tempfile.TemporaryDirectory(prefix="native-glass-xor-control-", dir=directory.parent) as temporary:
            control = Path(temporary)
            damaged = copy.deepcopy(records[0])
            for stage in ("vertex", "pixel", "raster_vertex"):
                name = "temporal-program-" + damaged[stage + "_sha256"] + ".bin"
                (control / name).write_bytes((folder / name).read_bytes())
            payload = (b"OLVRPD01" + struct.pack("<Q", damaged["decoded_xor_bytes"]) +
                       struct.pack("<I", 0x80000001) + b"\x01" + bytes(7) +
                       struct.pack("<I", damaged["decoded_xor_bytes"] // 8 - 1))
            damaged["xor_payload_sha256"] = sha(payload)
            (control / damaged["xor_image"]).write_bytes(payload)
            try:
                validate_record(damaged, control, {}, extent)
            except ValueError:
                rejected.append("literal-nonzero-xor-with-valid-digest")
            else:
                raise ValueError("Continuous native oracle accepted a decoded literal pixel-bit difference")
    return dict(exact_module_native_temporal_glass_path_acceptance=True,
                native_consecutive_moving_head_frames=101, native_glass_draws=len(records),
                minimum_glass_draws_per_frame=min(observed.values()), full_resolution=list(extent),
                bit_exact_RGBA_pixels_compared=sum(row["width"] * row["height"] for row in records),
                unchanged_six_pose_full_image_draws=len(full), differing_RGBA_bits=0,
                independently_decoded_unique_xor_payloads=len(cache), corruption_controls=rejected,
                physical_headset_acceptance=False, arbitrary_scene_acceptance=False,
                first_native_frame=min(poses), last_native_frame=max(poses))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    print(json.dumps(grade(args.directory, args.module, args.probe, args.selftest), indent=2))


if __name__ == "__main__":
    main()
