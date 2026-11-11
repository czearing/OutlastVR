import argparse
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def grade(directory, fault=None):
    manifest_path = directory / "native-run-manifest.json"
    telemetry_path = directory / "desktop-cursor.jsonl"
    complete = directory / "capture-complete.txt"
    require(complete.is_file(), "Native capture did not finish")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    require(manifest["scenario"] == "Continue", "Cursor proof requires native gameplay")
    rows = [json.loads(line) for line in telemetry_path.read_text().splitlines()]
    require(len(rows) >= 1000, "Cursor proof needs at least 1000 native frames")
    if fault == "confined":
        rows[len(rows) // 2]["clip"][2] -= 100
    elif fault == "no-hooks":
        for row in rows:
            row["suppressed_clips"] = row["nonexclusive_mouse_requests"] = 0
    elif fault == "counter-regression":
        rows[-1]["suppressed_clips"] = 0
    require(all(row["clip"] == row["desktop"] and
                row["desktop"][2] > row["desktop"][0] and
                row["desktop"][3] > row["desktop"][1] for row in rows),
            "Desktop cursor was confined instead of spanning the virtual desktop")
    for key in ("frame", "suppressed_clips", "suppressed_warps", "nonexclusive_mouse_requests"):
        require(all(current[key] >= previous[key] for previous, current in zip(rows, rows[1:])),
                "Native cursor evidence counters regressed")
    require(rows[-1]["suppressed_clips"] > 0 and rows[-1]["nonexclusive_mouse_requests"] > 0,
            "Native mouse hooks were never exercised")
    return {
        "native_frames": len(rows),
        "unconfined_frames": len(rows),
        "suppressed_clip_requests": rows[-1]["suppressed_clips"],
        "suppressed_warp_requests": rows[-1]["suppressed_warps"],
        "nonexclusive_mouse_requests": rows[-1]["nonexclusive_mouse_requests"],
        "foreground_outlast_frames": sum(row["foreground_is_outlast"] for row in rows),
        "mod_sha256": manifest["mod_sha256"],
        "inputs": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in (manifest_path, telemetry_path, complete)},
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--fault", choices=("confined", "no-hooks", "counter-regression"))
    args = parser.parse_args()
    try:
        result = grade(args.directory, args.fault)
        print(json.dumps(result, sort_keys=True))
        if not args.fault:
            (args.directory / "desktop-cursor-proof.json").write_text(
                json.dumps(result, indent=2) + "\n")
    except (ValueError, KeyError, OSError, TypeError) as error:
        parser.exit(1, f"FAIL: {error}\n")
