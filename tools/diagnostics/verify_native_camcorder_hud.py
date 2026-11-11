"""Verify private native GFx source-over coverage, not complete HUD/layout acceptance."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np

from verify_native_camcorder import rows
from verify_native_probe import require


def grade(directory, module, alpha_control, fault=None):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    digest = hashlib.sha256(module.read_bytes()).hexdigest().upper()
    require((directory / "capture-complete.txt").is_file() and manifest["native_camera_hud"] and
            manifest["mod_sha256"] == digest and fault != "wrong-module",
            "Native HUD coverage capture does not match the exact module")
    observed = rows(directory / "native-retail-camera-observation.jsonl")
    require(observed[-1]["kind"] == "complete", "Independent HUD observer did not complete")
    identities = set()
    for row in observed:
        hud = row.get("native_hud", {})
        if hud.get("target") and hud.get("native_view"):
            require(hud["native_owner"] == hud["movie"] and hud["native_target"] == hud["target"] and
                    hud["movie_class"] == "OLCamcorderHud" and
                    hud["target_class"] == "TextureRenderTarget2D" and
                    hud["render_texture_mode"] == 1 and hud["scale_mode"] == 2,
                    "Independent stock HUD identity or native target configuration disagrees")
            identities.add((hud["movie"], hud["target"]))
    require(identities, "No independently identified native HUD movie and target")
    commands = rows(directory / "native-hud-commands.jsonl")
    roots = set()
    for command in commands:
        record = bytearray.fromhex(command["record_hex"])
        handle = bytearray.fromhex(command["handle_hex"])
        require(len(record) == 0x8c and len(handle) == 0x38 and
                0 <= command["index"] < command["count"] <= 128,
                "Invalid retained native HUD command shape")
        if fault == "wrong-owner":
            record[0x70] ^= 1
        if fault == "wrong-display-root":
            handle[0x30] ^= 1
        require(struct.unpack_from("<QQ", record, 0x70) in identities,
                "Native render record does not belong to the independently identified HUD")
        require(struct.unpack_from("<I", handle, 0x28)[0] == 1 and
                struct.unpack_from("<Q", handle, 0x30)[0] == command["root"],
                "Actual native display handle does not select the owned HUD root")
        roots.add(command["root"])
    require(roots and all(roots), "No actual native HUD display roots")

    draws = rows(directory / "native-hud-draws.jsonl")
    native = None
    corrected = 0
    outputs = 0
    rgb_states = {"27": 1, "19": 5, "20": 6, "171": 1}
    for draw in draws:
        if draw["exact_hud_output"]:
            require(not draw["owned_native_hud"] and not draw["corrected_alpha"],
                    "HUD coverage correction leaked into the final copy")
            outputs += 1
            continue
        if not draw["corrected_alpha"]:
            native = draw
            if draw["owned_native_hud"]:
                require(draw["states"]["206"] == 0,
                        "Private HUD alpha state was not restored before the next native draw")
            continue
        require(native and native["owned_native_hud"] and draw["owned_native_hud"] and
                draw["display_root"] in roots and
                all(draw[key] == native[key] for key in
                    ("frame", "surface", "display_root", "pixel_shader_sha256", "pixel_c0_bits")),
                "Corrected draw lost its actual private native source")
        if fault == "unowned-draw":
            draw["owned_native_hud"] = False
        if fault == "rgb-state":
            draw["states"]["19"] = 2
        if fault == "squared-alpha":
            draw["states"]["207"] = 5
        if fault == "unrestored-state":
            native["states"]["206"] = 1
        require(draw["owned_native_hud"] and native["states"]["206"] == 0 and
                all(native["states"][key] == value == draw["states"][key]
                    for key, value in rgb_states.items()),
                "HUD source correction changed RGB blending or native caller state")
        expected = dict(native["states"])
        expected.update({"206": 1, "207": 2, "208": 6, "209": 1})
        require(draw["states"] == expected,
                "Native HUD does not retain source-over coverage with unchanged RGB state")
        program = directory / ("native-hud-shader-" + draw["pixel_shader_sha256"] + ".bin")
        actual_hash = hashlib.sha256(program.read_bytes()).hexdigest()
        require(actual_hash == draw["pixel_shader_sha256"] and fault != "wrong-program",
                "Actual native HUD pixel program does not match its retained source")
        corrected += 1
    require(corrected >= 50 and outputs >= 2, "Insufficient scoped native HUD draw observations")

    pixels = 0
    visible = []
    samples = rows(directory / "native-lens-samples.jsonl")
    require(len(samples) == 3, "Expected all three actual normal/NV HUD samples")
    for sample in samples:
        data = bytearray((directory / f"native-hud-{sample['sample']}.bgra").read_bytes())
        if fault == "hud-file-byte":
            data[0] ^= 1
        require(len(data) == 512 * 288 * 4 and
                hashlib.sha256(data).hexdigest() == sample["native_hud_sha256"],
                "Actual native HUD readback checksum or extent disagrees")
        image = np.frombuffer(data, dtype=np.uint8).reshape(-1, 4).copy()
        if fault == "nonpremultiplied-pixel":
            image[0] = [255, 0, 0, 0]
        require(np.all(image[:, :3] <= image[:, 3:4]),
                "Actual native HUD pixels violate premultiplied coverage")
        require(np.count_nonzero(image[:, 3]) >= 1000 and np.count_nonzero(image[:, 3] == 0) >= 100000,
                "Native HUD disappeared or lost its transparent background")
        baseline = np.frombuffer((alpha_control / f"native-hud-{sample['sample']}.bgra").read_bytes(),
                                 dtype=np.uint8).reshape(-1, 4)
        require(np.count_nonzero(np.any(baseline[:, :3] > baseline[:, 3:4], axis=1)) >= 100,
                "Alpha negative control does not reproduce the actual prior native coverage defect")
        pixels += len(image)
        visible.append(int(np.count_nonzero(image[:, 3])))
    return dict(native_hud_coverage=True, full_hud_layout_acceptance=False,
                owned_display_roots=len(roots), corrected_native_draws=corrected,
                unchanged_native_final_copies=outputs, checked_hud_pixels=pixels,
                visible_hud_pixels=visible, native_module_sha256=digest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--alpha-control", type=Path, required=True)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    result = grade(args.directory, args.module, args.alpha_control)
    if args.selftest:
        faults = ("wrong-module", "wrong-owner", "wrong-display-root", "unowned-draw",
                  "rgb-state", "squared-alpha", "unrestored-state", "wrong-program",
                  "hud-file-byte", "nonpremultiplied-pixel")
        for fault in faults:
            try:
                grade(args.directory, args.module, args.alpha_control, fault)
            except ValueError:
                continue
            raise ValueError("Native HUD coverage corruption was not rejected: " + fault)
        result["corruptions_rejected"] = len(faults)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
