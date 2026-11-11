"""Independent bit-exact reader, native writer and damaged-evidence checks."""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from native_pixel_delta import DELTA, RAW, decode_native_pixel_delta, read_native_pixels


WRITER = Path(os.environ.get("OLVR_PIXEL_DELTA_WRITER",
              str(Path(__file__).resolve().parents[2] / "build" / "bin" / "test_native_pixel_delta.exe")))


class NativePixelDeltaTest(unittest.TestCase):
    def test_native_writer_roundtrips_all_bits(self):
        baseline = bytes(range(64))
        variants = [baseline, bytes(64), bytes(x ^ 255 for x in baseline),
                    baseline[:8] + b"\xff" * 8 + baseline[16:48] + b"\x00\xfc\xff\x7f" * 4,
                    b"".join(baseline[i:i + 8] if i % 16 else bytes(8)
                             for i in range(0, 64, 8))]
        with tempfile.TemporaryDirectory(prefix="olvr-pixel-delta-") as name:
            folder = Path(name)
            (folder / "sample_0_before.rgba16f").write_bytes(baseline)
            for index, expected in enumerate(variants):
                original = folder / f"target_{index}"
                original.write_bytes(expected)
                output = folder / f"variant_{index}.delta"
                completed = subprocess.run(
                    [str(WRITER), str(folder / "sample_0_before.rgba16f"),
                     str(original), str(output)], capture_output=True, text=True, check=True)
                self.assertIn("invalid_extents_rejected=3", completed.stdout)
                self.assertEqual(decode_native_pixel_delta(output.read_bytes(), baseline, 64),
                                 expected)
                reader_path = folder / "sample_0_after.rgba16f.delta"
                reader_path.write_bytes(output.read_bytes())
                self.assertEqual(read_native_pixels(folder, "sample_0", "after", DELTA, 64),
                                 expected)
                xor_output = folder / f"variant_{index}.xor"
                subprocess.run([str(WRITER), str(folder / "sample_0_before.rgba16f"),
                                str(original), str(xor_output), "--xor"],
                               capture_output=True, text=True, check=True)
                self.assertEqual(decode_native_pixel_delta(xor_output.read_bytes(), bytes(64), 64),
                                 bytes(left ^ right for left, right in zip(baseline, expected)))

    def test_rejects_damaged_or_unbounded_evidence(self):
        baseline = bytes(range(64))
        header = b"OLVRPD01" + struct.pack("<Q", 64)
        valid = header + struct.pack("<I", 0x80000008) + baseline
        self.assertEqual(decode_native_pixel_delta(valid, baseline, 64), baseline)
        damaged = [
            b"wrongtag" + valid[8:], valid[:15],
            b"OLVRPD01" + struct.pack("<Q", 72) + valid[16:],
            header, header + struct.pack("<I", 0),
            header + struct.pack("<I", 0x80000000),
            header + struct.pack("<I", 9),
            header + struct.pack("<I", 0x80000009) + baseline + bytes(8),
            valid[:-1], valid + b"\x00",
            header + struct.pack("<I", 4),
        ]
        for payload in damaged:
            with self.subTest(payload=payload[:20]), self.assertRaises(ValueError):
                decode_native_pixel_delta(payload, baseline, 64)
        for wrong_base, extent in ((baseline[:-1], 64), (baseline, 0), (baseline, 63)):
            with self.subTest(extent=extent), self.assertRaises(ValueError):
                decode_native_pixel_delta(valid, wrong_base, extent)

    def test_raw_compatibility_and_explicit_storage(self):
        with tempfile.TemporaryDirectory(prefix="olvr-pixel-reader-") as name:
            folder = Path(name)
            (folder / "sample_0_before.rgba16f").write_bytes(bytes(range(64)))
            self.assertEqual(read_native_pixels(folder, "sample_0", "before", DELTA, 64),
                             bytes(range(64)))
            self.assertEqual(read_native_pixels(folder, "sample_0", "before", RAW, 64),
                             bytes(range(64)))
            for storage, size in ((RAW, 56), (RAW, 72), ("unknown", 64)):
                with self.subTest(storage=storage, size=size), self.assertRaises(ValueError):
                    read_native_pixels(folder, "sample_0", "before", storage, size)
            with self.assertRaises(FileNotFoundError):
                read_native_pixels(folder, "sample_0", "after", DELTA, 64)
            (folder / "sample_0_after.rgba16f.delta").write_bytes(bytes(113))
            with self.assertRaises(ValueError):
                read_native_pixels(folder, "sample_0", "after", DELTA, 64)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--writer", type=Path, required=True)
    options, remaining = parser.parse_known_args()
    WRITER = options.writer.resolve()
    unittest.main(argv=[__file__] + remaining)
