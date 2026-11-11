"""Read byte-exact native HDR evidence, including lossless baseline-relative pixels."""

import struct


RAW = "raw-rgba16f"
DELTA = "rle-native-pixel-delta-v1"


def decode_native_pixel_delta(payload, baseline, expected_bytes):
    if not expected_bytes or expected_bytes % 8 or len(baseline) != expected_bytes:
        raise ValueError("Invalid native pixel-delta baseline extent")
    if len(payload) < 16 or payload[:8] != b"OLVRPD01" or \
            struct.unpack_from("<Q", payload, 8)[0] != expected_bytes:
        raise ValueError("Invalid native pixel-delta header")
    result = bytearray(baseline)
    cursor, position = 16, 0
    while position < expected_bytes:
        if cursor + 4 > len(payload):
            raise ValueError("Truncated native pixel-delta run")
        token = struct.unpack_from("<I", payload, cursor)[0]
        cursor += 4
        size = (token & 0x7fffffff) * 8
        if not size or size > expected_bytes - position:
            raise ValueError("Native pixel-delta run exceeds the original extent")
        if token & 0x80000000:
            if cursor + size > len(payload):
                raise ValueError("Truncated native pixel-delta literals")
            result[position:position + size] = payload[cursor:cursor + size]
            cursor += size
        position += size
    if cursor != len(payload):
        raise ValueError("Trailing native pixel-delta evidence")
    return bytes(result)


def read_native_pixels(folder, prefix, name, storage, expected_bytes):
    if storage not in (RAW, DELTA):
        raise ValueError(f"Unknown native pixel storage: {storage}")
    path = folder / f"{prefix}_{name}.rgba16f"
    if storage == RAW or name == "before":
        if path.stat().st_size != expected_bytes:
            raise ValueError("Changed or truncated native pixel extent")
        return path.read_bytes()
    path = path.with_suffix(path.suffix + ".delta")
    if path.stat().st_size > 16 + expected_bytes * 3 // 2:
        raise ValueError("Native pixel-delta evidence exceeds its bounded extent")
    baseline = read_native_pixels(folder, prefix, "before", RAW, expected_bytes)
    return decode_native_pixel_delta(path.read_bytes(), baseline, expected_bytes)
