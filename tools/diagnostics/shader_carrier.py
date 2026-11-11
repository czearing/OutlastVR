"""Pure byte transformation recovered from static REA MCP evidence.

Loader: 0x1402b61f0, ev_d04db25a044acf44600ac924bc25ecc1c906abe1938c7b8a88d7dc132d945253.
XOR: 0x1400a9d80, ev_dd7029dcf3de407c324b5366516ce9ccf037a36efebd48dfa8cad7e886518829.
Text: 0x1400aa780, ev_ce1b243f1d573487edd07b2af442a6b0dfc329a0e2e52ba337d292fb704b473b.

No file access or execution. Callers supply a 761-byte table recovered through
REA read_bytes at 0x141dfd9b0 for the exact supported executable. This module
does not establish carrier integrity or compiled shader/register compatibility.
"""

import struct


def decode_payload(carrier: bytes, table: bytes) -> bytes:
    if len(table) != 0x2F9:
        raise ValueError("Shader XOR table must contain exactly 761 bytes")
    if len(carrier) < 0x18:
        raise ValueError("Shader carrier is shorter than its 24-byte header")
    if struct.unpack_from("<I", carrier)[0] != 1:
        raise ValueError("Unsupported shader carrier version")
    payload = bytearray(carrier[0x18:])
    state = (len(payload) * 0x3B98D + 0x3AA29) & 0xFFFFFFFF
    for offset in range(len(payload)):
        key_index = state % 0x2F9
        payload[offset] ^= table[key_index]
        state = key_index + 1
    return bytes(payload)


def decode_text(carrier: bytes, table: bytes, *, ansi_encoding: str) -> str:
    payload = decode_payload(carrier, table)
    if len(payload) % 2 == 0 and payload.startswith(b"\xff\xfe"):
        return payload[2:].decode("utf-16-le", errors="strict")
    if len(payload) % 2 == 0 and payload.startswith(b"\xfe\xff"):
        return payload[2:].decode("utf-16-be", errors="strict")
    # CP_ACP is environment-dependent in the recovered native implementation.
    # Require an explicit encoding rather than silently assuming the locale.
    return payload.decode(ansi_encoding, errors="strict")
