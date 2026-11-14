import argparse
import hashlib
import json
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Retain the actual recovered hero rig for independent IK acceptance.")
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--pose-capture", type=Path, required=True)
    parser.add_argument("--reference-capture", type=Path, required=True)
    args = parser.parse_args()
    raw = args.capture.read_bytes()
    rows = [json.loads(line) for line in raw.splitlines()]
    candidates = [
        row for row in rows
        if row["basic"] == 0 and row["special"] == 0 and len(row["atoms"]) == 73
        and row["atoms"][8][6] > 145 and not row.get("ik_hands", 0)
        and len(row["bindings"]) in (20, 36)
        and all(binding["index"] >= 0 and binding["matrix_error"] == 0 for binding in row["bindings"])
    ]
    if not candidates:
        raise ValueError("Capture has no unmodified standing 73-bone hero with exact native matrix agreement")
    row = candidates[-1]
    indices = [binding["index"] for binding in row["bindings"][:20]]
    data = struct.pack("<QI20i", 0x314B49524C4F, len(row["atoms"]), *indices)
    data += struct.pack("<" + "i" * len(row["parents"]), *row["parents"])
    data += struct.pack("<16f", *row["world"])
    for atom in row["atoms"]:
        data += struct.pack("<8f", *atom)
    if args.output.exists() and args.output.read_bytes() != data:
        raise ValueError("Refusing to replace a different retained native rig fixture")
    if not args.output.exists():
        args.output.write_bytes(data)
    manifest = {
        "capture": str(args.capture.resolve()),
        "capture_sha256": hashlib.sha256(raw).hexdigest(),
        "native_serial": row["serial"],
        "fixture_sha256": hashlib.sha256(data).hexdigest(),
        "bones": len(row["atoms"]),
        "native_matrix_max_error": 0,
    }
    pose_raw = args.pose_capture.read_bytes()
    pose_rows = [json.loads(line) for line in pose_raw.splitlines()]
    native_poses = [row["before_atoms"] for row in pose_rows if row.get("before_atoms")
                    and row["targets"]["physical"]][::30]
    if len(native_poses) < 8 or len(native_poses) > 64:
        raise ValueError("Actual native pose corpus lacks bounded crouch and transition coverage")
    corpus = struct.pack("<QII", 0x3153504F4C4F, len(native_poses), 73)
    for pose in native_poses:
        for atom in pose:
            corpus += struct.pack("<8f", *atom)
    pose_path = args.output.with_suffix(".poses.bin")
    if pose_path.exists() and pose_path.read_bytes() != corpus:
        raise ValueError("Refusing to replace a different retained native pose corpus")
    if not pose_path.exists():
        pose_path.write_bytes(corpus)
    manifest.update({
        "pose_capture": str(args.pose_capture.resolve()),
        "pose_capture_sha256": hashlib.sha256(pose_raw).hexdigest(),
        "pose_fixture": str(pose_path.resolve()),
        "pose_fixture_sha256": hashlib.sha256(corpus).hexdigest(),
        "pose_count": len(native_poses),
    })
    reference_row = None
    with args.reference_capture.open(encoding="utf-8") as stream:
        for line in stream:
            candidate = json.loads(line)
            if candidate.get("reference_records"):
                reference_row = candidate
                break
    if reference_row is None or len(reference_row["reference_records"]) != 73:
        raise ValueError("Capture lacks immutable native reference palm records")
    reference = b"".join(struct.pack("<20I", *record) for record in reference_row["reference_records"])
    reference_path = args.output.with_suffix(".reference.bin")
    if reference_path.exists() and reference_path.read_bytes() != reference:
        raise ValueError("Refusing to replace a different native reference skeleton")
    if not reference_path.exists():
        reference_path.write_bytes(reference)
    manifest.update({
        "reference_capture": str(args.reference_capture.resolve()),
        "reference_capture_sha256": hashlib.file_digest(args.reference_capture.open("rb"), "sha256").hexdigest(),
        "reference_fixture": str(reference_path.resolve()),
        "reference_fixture_sha256": hashlib.sha256(reference).hexdigest(),
    })
    args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("PASS: retained actual 73-bone native rig fixture; " + manifest["fixture_sha256"])


if __name__ == "__main__":
    main()
