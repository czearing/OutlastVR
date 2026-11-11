"""Grade stock camera FX inputs, unchanged-retail-shader copies and LCD-only output.

With --raw-optics, also grade checksummed native scene inputs before stock
postprocess distortion using the unchanged optical-direction thresholds.
This is not exact HUD-layout, finger-wrap, hand-contact or headset acceptance.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

import cv2
import numpy as np

from verify_native_camcorder import direction_grade, load, rows, screen_grade
from verify_native_probe import require


LUT_PATH = "Asylum_post_process.lut_night_vision_desatured"
AUTHORED_LUT_SHADER = "b799a1a6c9648367c2499010a0554a2dcd3c8d69e03858d76f507b2ee7459dd4"
CAMERA_COLOR_HDR_SHADER = "db5c5029225113e570c43fd3de1103e3757e4bfc41b9a208c600c4f544e77eaa"
FX_FAULTS = ("wrong-asset", "wrong-settings", "missing-lut", "unowned-override",
             "missing-authored-lut-shader", "identity-lut-output", "missing-nv-lut", "wrong-lut-mode",
             "rgb-code", "missing-hud", "fullscreen-nv", "missing-screen", "screen-pose")
RAW_OPTICS_FAULTS = ("wrong-optical-frame", "raw-code-error", "reversed-optical-axis",
                    "wrong-optical-origin", "wrong-rendered-eye", "wrong-first-hdr-program")


def identities(directory):
    observed = rows(directory / "native-retail-camera-observation.jsonl")
    require(observed[-1]["kind"] == "complete" and observed[-1]["samples"] >= 100,
            "Independent native camera observation did not complete")
    managers = [row for row in observed if row["kind"] == "native_fx_manager_identity"]
    luts = [row for row in observed if row["kind"] == "native_color_lut_identity"]
    require(managers and luts, "Missing independent stock settings owner or LUT asset identity")
    return managers, luts


def grade_luts(directory, bindings, fault):
    images = {1: [], 2: []}
    samples = []
    for path in sorted(directory.glob("native-postprocess-lut-*.ppm")):
        frame = int(path.stem.rsplit("-", 1)[1])
        owned = [row for row in bindings if not row["main"] and row["frame"] == frame]
        modes = {row["mode"] for row in owned}
        require(len(modes) == 1 and modes <= {1, 2},
                "Native LUT snapshot lacks an unambiguous same-frame owned camera mode")
        mode = modes.pop()
        if fault == "missing-nv-lut" and mode == 2:
            continue
        if fault == "wrong-lut-mode":
            mode = 3 - mode
        require(any(row["actual_pixel"] == AUTHORED_LUT_SHADER for row in owned) == (mode == 2),
                "Native LUT snapshot mode disagrees with its actual authored-LUT shader")
        image = cv2.imread(str(path))
        require(image is not None and image.shape == (16, 256, 3),
                "Missing or malformed actual native LUT snapshot")
        images[mode].append(image)
        samples.append({"frame": frame, "mode": "night_vision" if mode == 2 else "normal",
                        "image": path.name})
    require(images[1] and images[2], "Actual native LUT observations do not contain both camera modes")
    if fault == "identity-lut-output":
        images[2] = [images[1][0]]
    require(all(not np.array_equal(normal, night) for normal in images[1] for night in images[2]),
            "Actual NV LUT output remained the normal identity LUT")
    return samples


def grade(directory, module, stock_control, fault=None):
    manifest = json.loads((directory / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require((directory / "capture-complete.txt").is_file() and manifest["physical_camcorder"] and
            manifest["native_camera_hud"] and not manifest["stock_camera_fx_control"] and
            manifest["mod_sha256"] == hashlib.sha256(module.read_bytes()).hexdigest().upper(),
            "Stock camera FX proof does not match an isolated exact-module capture")
    control_manifest = json.loads((stock_control / "native-run-manifest.json").read_text(encoding="utf-8-sig"))
    require(control_manifest["stock_camera_fx_control"], "Native stock reference is not an explicit positive control")
    control_managers, control_luts = identities(stock_control)
    require(any(row["path"] == LUT_PATH and row["class"] == "Texture2D" for row in control_luts),
            "Positive control did not independently identify the actual stock NV LUT")
    control_factory = rows(stock_control / "native-postprocess-factory.jsonl")
    require(any(row["source_mode"] == 2 and row["settings_lut"] and
                row["proxy_lut"] == row["settings_lut"] for row in control_factory),
            "Stock positive control did not actually pass its native LUT into a real NV proxy")

    managers, luts = identities(directory)
    if fault == "wrong-asset":
        luts = copy.deepcopy(luts)
        for row in luts:
            row["path"] = "WrongPackage.WrongLUT"
    admitted = {row["object"] for row in luts if row["path"] == LUT_PATH and row["class"] == "Texture2D"}
    source_settings = {}
    control_flags = {struct.unpack_from("<II", bytes.fromhex(row["header_hex"]), 0x60)
                     for row in control_managers}
    for row in managers:
        header = bytes.fromhex(row["header_hex"])
        flags = struct.unpack_from("<II", header, 0x60)
        lut = struct.unpack_from("<Q", header, 0x124)[0]
        require(row["class"] == "OLFXManager" and flags in control_flags and flags[1] & 0x20 and
                lut in admitted, "Owned camera settings are not the independently identified stock NV settings")
        source_settings[row["object"] + 0x60] = (flags, lut)

    factory = rows(directory / "native-postprocess-factory.jsonl")
    if fault in ("wrong-settings", "missing-lut", "unowned-override"):
        factory = copy.deepcopy(factory)
        for row in factory:
            if row["private_capture"] and row["source_mode"] == 2:
                if fault == "wrong-settings":
                    row["selected_settings"] += 8
                elif fault == "missing-lut":
                    row["selected_lut"] = 0
                else:
                    row["private_capture"] = False
    private_nv = [row for row in factory if row["private_capture"] and row["source_mode"] == 2]
    private_normal = [row for row in factory if row["private_capture"] and row["source_mode"] == 1]
    untouched = [row for row in factory if not row["private_capture"]]
    require(min(len(private_nv), len(private_normal), len(untouched)) >= 100,
            "Insufficient actual private normal/NV and unmodified other-view factory observations")
    for row in private_nv:
        expected = source_settings.get(row["selected_settings"])
        require(expected is not None and tuple(row["selected_flags"]) == expected[0] and
                row["selected_lut"] == expected[1] == row["proxy_lut"],
                "Private NV factory lost the actual native settings/LUT input")
    require(all(row["selected_settings"] == row["settings"] and
                row["selected_lut"] == row["settings_lut"] for row in private_normal + untouched),
            "Owned NV override changed a normal camera or another native view")

    bindings = [row for row in rows(directory / "native-postprocess-dispatch.jsonl")
                if row["kind"] == "bound_shader"]
    if fault == "missing-authored-lut-shader":
        bindings = [row for row in bindings if row["actual_pixel"] != AUTHORED_LUT_SHADER]
    require(bindings and all(row["actual_pixel"] == row["requested_pixel"] for row in bindings) and
            any(not row["main"] and row["mode"] == 2 and
                row["actual_pixel"] == AUTHORED_LUT_SHADER for row in bindings),
            "Actual owned NV GPU pipeline did not bind the requested native authored-LUT shader")
    lut_samples = grade_luts(directory, bindings, fault)

    data = load(directory)
    require(len(data["samples"]) == 3, "Missing native normal/NV final-copy samples")
    checked = 0
    for sample in data["samples"]:
        reference = next((row for row in data["references"] if row["frame"] == sample["frame"] and
                          row["sample"] == sample["sample"]), None)
        require(reference is not None and reference["original_retail_shader"] and
                reference["checked_pixels"] == 512 * 288 and not reference["different_pixels"],
                "Missing same-frame complete original-retail-shader mono reference")
        actual = cv2.imread(str(directory / f"native-lens-{sample['sample']}.ppm"))
        expected = cv2.imread(str(directory / reference["reference_image"]))
        if fault == "rgb-code" and actual is not None:
            actual[20, 20, 0] ^= 1
        require(actual is not None and expected is not None and np.array_equal(actual, expected),
                "Processed lens RGB differs from its actual native mono reference")
        checked += 512 * 288
    if fault == "missing-hud":
        for row in data["composition"]:
            row["native_hud_rhi_texture"] = 0
    require(all(row["native_hud_rhi_texture"] and not row["synthetic_night_vision"]
                for row in data["composition"]), "Native LCD lost its actual stock HUD or used synthetic NV")
    # Stock NV can be nearly white; the verified native LUT/shader/source chain,
    # rather than a saturated-green threshold, establishes its effect identity.
    screens = screen_grade(data, fault, native_postprocess=True)
    return dict(private_nv_factories=len(private_nv), private_normal_factories=len(private_normal),
                unchanged_other_view_factories=len(untouched), matched_native_shader_bindings=len(bindings),
                mode_bound_lut_samples=lut_samples,
                native_lut_asset=LUT_PATH, complete_original_shader_reference_pixels=checked,
                stereo_screen_observations=len(screens),
                surrounding_world_rgb_max_delta=max(row["surrounding_world_rgb_max_delta"] for row in screens),
                optical_and_exact_hud_acceptance=False)


def raw_program_grade(directory, row, fault=None):
    version = row.get("raw_optics_schema_version", 1)
    require(type(version) is int and version in (1, 2), "Unknown raw optics program provenance version")
    if version == 1:
        return None
    name = row.get("first_hdr_program")
    size = row.get("first_hdr_program_bytes")
    require(type(row.get("frame")) is int and name == f"native-raw-optics-program-{row['frame']}.bin" and
            type(size) is int and 8 <= size <= 1048576 and size % 4 == 0,
            "Raw optics do not identify their exact same-frame bound native program")
    path = directory / name
    require(path.is_file(), "Missing actual first HDR native program bytes")
    payload = path.read_bytes()
    require(len(payload) == size and hashlib.sha256(payload).hexdigest() == row["first_hdr_postprocess_shader"] and
            fault != "wrong-first-hdr-program", "Bound native raw optics shader bytes or digest changed")
    return path


def raw_optics_grade(directory, fault=None):
    data = load(directory)
    records = rows(directory / "native-raw-optics.jsonl")
    require(len(records) == len(data["samples"]) == 3 and len({row["frame"] for row in records}) == 3,
            "Raw optics do not identify three unique native normal/NV samples")
    if fault == "wrong-optical-frame":
        records[0]["frame"] += 1
    if fault == "wrong-first-hdr-program":
        records[0]["first_hdr_postprocess_shader"] = "0" * 64
    if fault in ("reversed-optical-axis", "wrong-optical-origin"):
        for view in data["views"]:
            if fault == "wrong-optical-origin":
                view["origin"][0] += 10000
            else:
                view["backward"] = [-value for value in view["backward"]]
                view["right"] = [-value for value in view["right"]]
    if fault == "wrong-rendered-eye":
        for camera in data["cameras"].values():
            camera["rendered_eyes"][0][16] += 100
    result = []
    for sample in data["samples"]:
        raw = next((row for row in records if row["frame"] == sample["frame"]), None)
        require(raw is not None and raw["width"] == 512 and raw["height"] == 288 and
                raw["format"] == 113 and raw["source_rectangle"] == [0, 0, 512, 288] and
                raw["source_rhi_texture"] and raw["native_view"] and
                raw["first_hdr_postprocess_shader"] in {
                    "19448eae2e35467058e6fa2607f838656aa33d745e4c65ffb1fbc5de181e2111",
                    "b43683326321c59b0640bcd40f0ceeef14d3a851f65221b0794af2de0a3d39e3",
                    CAMERA_COLOR_HDR_SHADER},
                "Raw optical input is not the identified first HDR sampler of the owned native view")
        retained_program = raw_program_grade(directory, raw, fault)
        if raw["first_hdr_postprocess_shader"] == CAMERA_COLOR_HDR_SHADER:
            # Respecting disabled Deform exposes the already-retained stock
            # CameraColor program as the first normal-mode HDR scene reader.
            program = retained_program or directory / "draw-probe" / (CAMERA_COLOR_HDR_SHADER + ".bin")
            require(program.is_file() and hashlib.sha256(program.read_bytes()).hexdigest() ==
                    CAMERA_COLOR_HDR_SHADER and fault != "wrong-first-hdr-program" and raw["sampler"] == 0,
                    "First stock CameraColor HDR program lacks its exact native GPU bytecode")
            visibility = rows(directory / "native-effect-visibility.jsonl")
            require(any(row["private_capture"] and row["source_flags"] == 4 and
                        row["omitted_disabled_effect"] and not row["proxy"] for row in visibility),
                    "First CameraColor HDR input lacks native disabled-effect visibility evidence")
        payload = (directory / raw["image"]).read_bytes()
        if fault == "raw-code-error":
            payload = bytes([payload[0] ^ 1]) + payload[1:]
        require(len(payload) == 512 * 288 * 8 and
                hashlib.sha256(payload).hexdigest() == raw["image_sha256"],
                "Raw optics differ from the actual native half-float readback")
        pixels = np.frombuffer(payload, dtype="<f2").reshape(288, 512, 4).astype(np.float32)
        require(np.isfinite(pixels).all() and np.min(pixels[:, :, :3]) >= 0,
                "Raw native optical pixels are nonfinite or negative")
        # Photometric normalization is diagnostic only; delivered pixels retain
        # the actual stock material distortion, tone curve, LUT and noise.
        rgb = pixels[:, :, :3]
        normalized = np.uint8(np.clip(rgb / (1 + rgb), 0, 1) ** (1 / 2.2) * 255)
        frames = [frame for frame in data["frames"].values() if frame.get("lens_sample") == sample["sample"]]
        require(len(frames) == 1, "Raw optics lack their unique same-submission world image")
        view = min(data["views"], key=lambda row: abs(row["frame"] - sample["frame"]))
        result.append(direction_grade(data, sample, normalized[:, :, ::-1], frames[0], view))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--stock-control", required=True, type=Path)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--raw-optics", action="store_true",
                        help="Also grade checksummed rectilinear HDR input before stock camera distortion")
    args = parser.parse_args()
    result = grade(args.directory, args.module, args.stock_control)
    if args.raw_optics:
        result["raw_optical_direction"] = raw_optics_grade(args.directory)
    rejected = []
    if args.selftest:
        for fault in FX_FAULTS:
            try:
                grade(args.directory, args.module, args.stock_control, fault)
            except ValueError:
                rejected.append(fault)
            else:
                raise ValueError("Native camera FX oracle accepted corruption: " + fault)
        if args.raw_optics:
            for fault in RAW_OPTICS_FAULTS:
                try:
                    raw_optics_grade(args.directory, fault)
                except ValueError:
                    rejected.append(fault)
                else:
                    raise ValueError("Raw native optics oracle accepted corruption: " + fault)
    result["rejected_controls"] = rejected
    result["module_sha256"] = hashlib.sha256(args.module.read_bytes()).hexdigest()
    result["verifier_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
