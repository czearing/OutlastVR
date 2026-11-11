"""Check actual GPU draw-probe output against independently specified fixture state."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def check_hdr_fixture(draw: dict, payload: bytes, image: bytes) -> None:
    if (draw["native_frame"], draw["draw"], draw["format"], draw["hdr_format"],
            draw["hdr_width"], draw["hdr_height"], draw["sample_source_width"],
            draw["sample_source_height"]) != (21, 1, 113, "RGBA16F_LE", 512, 256, 512, 256):
        raise ValueError("Exact HDR fixture identity or source sampling extent changed")
    if hashlib.sha256(payload).hexdigest() != draw["hdr_sha256"]:
        raise ValueError("Exact HDR fixture checksum changed")
    expected = struct.pack("<4H", 0x1400, 0x4100, 0xB400, 0x3C00) * (512 * 256)
    if payload != expected:
        raise ValueError("Native sub-byte, above-one or negative HDR values were lost")
    if image != b"P6\n512 256\n255\n" + bytes((0, 255, 0)) * (512 * 256):
        raise ValueError("Legacy image no longer represents the actual clamped HDR fixture")


def verify_hdr_fixture(root: Path) -> dict:
    folder = root / "hdr-draw-probe"
    records = [json.loads(line) for line in (folder / "draws.jsonl").read_text().splitlines()]
    draws = [record for record in records if "draw" in record]
    if len(draws) != 1 or not draws[0]["hdr_image"]:
        raise ValueError("Missing exact native HDR fixture")
    draw = draws[0]
    payload = (folder / "draw_1.rgba16f").read_bytes()
    image = (folder / "draw_1.ppm").read_bytes()
    check_hdr_fixture(draw, payload, image)
    faults = [
        struct.pack("<4H", 0, 0x4100, 0xB400, 0x3C00) * (512 * 256),
        struct.pack("<4H", 0x1400, 0x3C00, 0, 0x3C00) * (512 * 256),
        payload[:-2],
    ]
    for fault in faults:
        changed = dict(draw, hdr_sha256=hashlib.sha256(fault).hexdigest())
        try:
            check_hdr_fixture(changed, fault, image)
        except ValueError:
            continue
        raise ValueError("HDR oracle accepted a correctly checksummed corrupt sample payload")
    return {"exact_hdr_channel_checks": 512 * 256 * 4,
            "hdr_corruption_controls": len(faults),
            "window_flicker_acceptance": False}


def verify(root: Path) -> dict:
    folder = root / "draw-probe"
    records = [json.loads(line) for line in (folder / "draws.jsonl").read_text().splitlines()]
    draws = [record for record in records if "draw" in record]
    if len(draws) != 2 or len([record for record in records if "shader" in record]) != 2:
        raise ValueError("Probe did not retain two draws and two unique source shaders")
    expected = ((1, .5, .25, 1), (.125, .75, .5, None))
    for index, draw in enumerate(draws):
        if draw["native_frame"] != 20 or draw["draw"] != index + 1 or not draw["image"]:
            raise ValueError("Draw identity or native image missing")
        for stage in ("vertex", "pixel"):
            words = (folder / f"{draw[stage]}.bin").read_bytes()
            if (len(words) < 8 or len(words) % 4 or
                    hashlib.sha256(words).hexdigest() != draw[stage].lower()):
                raise ValueError("Source shader was not preserved")
        tint = next(value for value in draw["pixel_constants"] if value["name"] == "TestTint")
        offset = next(value for value in draw["vertex_constants"] if value["name"] == "VertexOffset")
        if tint["register"] != 5 or tint["count"] != 1 or tint["values"] != list(expected[index]):
            raise ValueError("Per-draw material constants were stale or incorrect")
        if offset["register"] != 7 or offset["values"] != [0, 0, index + 1, index + 2]:
            raise ValueError("Per-draw vertex constants were stale or incorrect")
        if tint["nonfinite"] != bool(index):
            raise ValueError("Nonfinite material state was hidden")
        if index:
            bits = tint["raw_bits"][3]
            if not math.isnan(struct.unpack("<f", struct.pack("<I", bits))[0]):
                raise ValueError("Nonfinite material payload was not retained")
        states = draw["material_state"]
        bias = struct.unpack("<I", struct.pack("<f", .00025 if index else 0))[0]
        if (states["alpha_test"], states["alpha_ref"], states["alpha_func"],
                states["depth_bias_bits"]) != (index, 128, 5, bias):
            raise ValueError("Per-draw alpha/depth state was lost")
        textures = draw["textures"]
        if len(textures) != 16 or any(texture["resource"] is not None for texture in textures[3:]):
            raise ValueError("Bound and missing native texture slots were not preserved")
        for slot, kind, extent, levels in ((0, 3, (16, 8, 1), 1),
                                          (1, 5, (32, 32, 1), 2),
                                          (2, 4, (16, 8, 4), 1)):
            texture = textures[slot]
            if (texture["slot"], texture["type"], texture["levels"]) != (slot, kind, levels):
                raise ValueError("Native texture kind or mip count changed")
            if tuple(texture[key] for key in ("width", "height", "depth")) != extent:
                raise ValueError("Native texture descriptor changed")
        if (textures[0]["sampler"]["address_u"], textures[0]["sampler"]["srgb"]) != (3 if index else 1, index):
            raise ValueError("Per-draw sampler state was lost")
        image = (folder / f"draw_{index + 1}.ppm").read_bytes()
        header = b"P6\n512 256\n255\n"
        if not image.startswith(header) or len(image) != len(header) + 512 * 256 * 3:
            raise ValueError("Native diagnostic image was truncated")
        rgb = image[len(header) + (128 * 512 + 256) * 3:][:3]
        if any(abs(value - round(channel * 255)) > 1 for value, channel in zip(rgb, expected[index][:3])):
            raise ValueError("Retained native image does not match the actual changing material")
    return {"draws": 2, "unique_source_shaders": 2, "native_texture_kinds": 3,
            "per_draw_uniforms_and_sampler_raster_state": True,
            "nonfinite_json_and_payload_preserved": True, "image_sample_checks": 6,
            "scope": "GPU diagnostic fixture, not identification or repair of the user's carpet"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--hdr", action="store_true")
    args = parser.parse_args()
    try:
        result = verify(args.root)
        if args.hdr:
            result.update(verify_hdr_fixture(args.root))
        print("PASS: " + json.dumps(result, sort_keys=True))
    except (ValueError, KeyError, StopIteration) as error:
        parser.exit(1, f"FAIL: {error}\n")
