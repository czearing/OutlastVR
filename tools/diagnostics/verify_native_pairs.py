"""Compare native paired rewriting with Python on private, constructed retail-stage pairs."""

import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

from sm3 import instructions, register_type, resources
from stereo_shader import patch_pair


def sample(paths, limit=256):
    paths = sorted(paths)
    if not paths:
        raise ValueError("Retail shader stage inventory is empty")
    if len(paths) <= limit:
        return paths
    return [paths[index * (len(paths) - 1) // (limit - 1)] for index in range(limit)]


def compatible(vertex, pixel):
    produced = {
        (item["usage"], item["usage_index"]): item["mask"]
        for item in vertex["declarations"] if item["register_type"] == 6
    }
    return all(
        not item["mask"] & ~produced.get((item["usage"], item["usage_index"]), 0)
        for item in pixel["declarations"] if item["register_type"] == 1
    )


def vertex_pool(source):
    paths = sorted(source.glob("vertex-*.bin"))
    packed = []
    for path in paths:
        varyings = sum(
            opcode == 31 and register_type(tokens[2]) == 6 and tokens[1] & 15 not in (0, 4)
            for opcode, tokens in instructions(path.read_bytes())
        )
        if varyings >= 10:
            packed.append(path)
    return list(dict.fromkeys(packed + sample(paths)))


def verify(source, native, output, count):
    if not 1 <= count <= 65536:
        raise ValueError("Reference pair count must be between 1 and 65536")
    if output.exists():
        raise FileExistsError(f"Proof output must be a new directory: {output}")
    vertices = [(path, path.read_bytes()) for path in vertex_pool(source)]
    pixels = [(path, path.read_bytes()) for path in sample(source.glob("pixel-*.bin"))]
    vertices = [(path, code, resources(code)) for path, code in vertices]
    pixels = [(path, code, resources(code)) for path, code in pixels]
    pairs, refused = [], []
    for vertex_path, vertex_code, vertex_info in vertices:
        for pixel_path, pixel_code, pixel_info in pixels:
            if not compatible(vertex_info, pixel_info):
                continue
            try:
                vertex, pixel = patch_pair(vertex_code, pixel_code)
            except ValueError as error:
                refused.append({
                    "vertex": vertex_path.name, "pixel": pixel_path.name,
                    "reason": str(error),
                })
                continue
            pairs.append((vertex_path, vertex_code, pixel_path, pixel_code, vertex, pixel))
            break
        if len(pairs) == count:
            break
    if len(pairs) != count:
        raise RuntimeError(f"Insufficient compatible reference pairs: {len(pairs)} of {count}")
    output.mkdir(parents=True, exist_ok=False)
    reference = output / "reference.bin"
    inventory = []
    with reference.open("wb") as stream:
        stream.write(b"OLVRPAIR" + struct.pack("<I", len(pairs)))
        for vertex_path, original_vertex, pixel_path, original_pixel, vertex, pixel in pairs:
            for code in (original_vertex, original_pixel, vertex[0], pixel[0]):
                stream.write(struct.pack("<I", len(code)) + code)
            stream.write(struct.pack(
                "<7I", vertex[1]["vertex_sampler"] or 0, pixel[1]["guard_constant"],
                vertex[1]["eye_input_semantic"], pixel[1]["eye_semantic"],
                pixel[1]["eye_component"], int(pixel[1]["packed_eye"]),
                int(vertex[1]["mode"] == "view_texture"),
            ))
            inventory.append({
                "vertex": vertex_path.name, "pixel": pixel_path.name,
                "packed": pixel[1]["packed_eye"], "mode": vertex[1]["mode"],
                "vertex_sha256": vertex[1]["transformed_sha256"],
                "pixel_sha256": pixel[1]["transformed_sha256"],
            })
    result = subprocess.run(
        [str(native), "--verify-reference", str(reference)],
        capture_output=True, text=True, check=False,
    )
    if result.returncode or not re.search(rf"byte_exact_pairs={count};", result.stdout):
        raise RuntimeError(f"Native pair verification failed: {result.stdout}\n{result.stderr}")
    controls = {}
    original = reference.read_bytes()
    for name, contents in (
        ("truncated", original[:-1]),
        ("wrong-metadata", original[:-4] + struct.pack("<I", 2)),
        ("trailing", original + b"\x00"),
    ):
        fault = output / f"{name}.bin"
        fault.write_bytes(contents)
        negative = subprocess.run(
            [str(native), "--verify-reference", str(fault)],
            capture_output=True, text=True, check=False,
        )
        if negative.returncode != 1 or "FAIL:" not in negative.stderr:
            raise RuntimeError(f"Native verifier accepted invalid {name} control")
        controls[name] = negative.stderr.strip()
        fault.unlink()
    report = {
        "status": "native_pair_reference_passed",
        "count": count, "native_output": result.stdout.strip(),
        "bundle_sha256": hashlib.sha256(original).hexdigest(),
        "pairs": inventory, "controls": controls, "reference_refusals": refused,
        "limitations": [
            "Pairs are constructed from compatible retail declarations, not observed Outlast bindings.",
            "Byte agreement does not establish game hook, screen-space effect or actual retail draw correctness.",
        ],
    }
    (output / "proof.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"{result.stdout.strip()}; invalid_bundle_controls={len(controls)}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, default=128)
    args = parser.parse_args()
    verify(args.source, args.native, args.output, args.count)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
