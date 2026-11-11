import struct
import unittest

from shader_carrier import decode_payload, decode_text


HEADER = struct.pack("<I", 1) + bytes(20)
TABLE = bytes(index & 255 for index in range(761))


class ShaderCarrierTests(unittest.TestCase):
    def test_fixed_native_formula_vector(self):
        # Ten payload bytes start at key index 256: the native seed is
        # (10 * 244109 + 240169) mod 761 = 256.
        encoded = bytes([102, 109, 109, 98, 112, 49, 38, 127, 51, 3])
        self.assertEqual(decode_payload(HEADER + encoded, TABLE), b"float4 x;\n")
        self.assertEqual(decode_text(HEADER + encoded, TABLE, ansi_encoding="ascii"), "float4 x;\n")

    def test_empty_valid_carrier(self):
        self.assertEqual(decode_payload(HEADER, TABLE), b"")

    def test_symmetric_xor_does_not_change_header(self):
        payload = bytes(range(256)) * 73
        encrypted = decode_payload(HEADER + payload, TABLE)
        self.assertNotEqual(encrypted, payload)
        self.assertEqual(decode_payload(HEADER + encrypted, TABLE), payload)

    def test_seed_uses_uint32_overflow(self):
        count = 18000
        encoded = bytes(count)
        decoded = decode_payload(HEADER + encoded, TABLE)
        # (18000 * 244109 + 240169) - 2**32 = 99234873;
        # 99234873 mod 761 = 473.
        start = 473
        self.assertEqual(decoded, bytes(TABLE[(start + offset) % 761] for offset in range(count)))

    def test_both_utf16_orders_and_explicit_ansi(self):
        for encoding, bom in (("utf-16-le", b"\xff\xfe"), ("utf-16-be", b"\xfe\xff")):
            text = "float4 \u00e9;\n"
            plain = bom + text.encode(encoding)
            encoded = decode_payload(HEADER + plain, TABLE)
            self.assertEqual(decode_text(HEADER + encoded, TABLE, ansi_encoding="ascii"), text)
        plain = b"// \xe9"
        encoded = decode_payload(HEADER + plain, TABLE)
        self.assertEqual(decode_text(HEADER + encoded, TABLE, ansi_encoding="cp1252"), "// \u00e9")
        with self.assertRaises(UnicodeDecodeError):
            decode_text(HEADER + encoded, TABLE, ansi_encoding="ascii")

    def test_rejects_malformed_input(self):
        for carrier in (b"", HEADER[:23], struct.pack("<I", 2) + bytes(20)):
            with self.assertRaises(ValueError):
                decode_payload(carrier, TABLE)
        for table in (b"", TABLE[:760], TABLE + b"\x00"):
            with self.assertRaises(ValueError):
                decode_payload(HEADER, table)


if __name__ == "__main__":
    unittest.main()
