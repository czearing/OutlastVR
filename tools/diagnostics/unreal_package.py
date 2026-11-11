"""Read-only decompression of the observed Outlast UE3 package profile."""

import struct
import zlib

from dissect.util.compression import lzo


TAG = 0x9E2A83C1


def decompress_package(data: bytes) -> bytes:
    if len(data) < 117 or struct.unpack_from("<I", data)[0] != TAG:
        raise ValueError("Not an Unreal package")
    version, license_version = struct.unpack_from("<HH", data, 4)
    if (version, license_version) != (868, 3):
        raise ValueError(f"Unsupported Outlast package version: {version}/{license_version}")
    folder_length = struct.unpack_from("<i", data, 12)[0]
    if folder_length != 5 or data[16:21] != b"None\0":
        raise ValueError("Package summary does not match the observed None-folder profile")
    generations = struct.unpack_from("<I", data, 85)[0]
    summary = 89 + generations * 12
    if summary + 16 > len(data):
        raise ValueError("Truncated package generation table")
    flags, count = struct.unpack_from("<II", data, summary + 8)
    if count == 0:
        return data
    if flags not in (1, 2):
        raise ValueError(f"Unsupported package compression flags: {flags}")
    table = summary + 16
    if count > 65536 or table + count * 16 > len(data):
        raise ValueError("Invalid package compression table")
    chunks = [struct.unpack_from("<4I", data, table + index * 16) for index in range(count)]
    virtual_size = max(offset + size for offset, size, _, _ in chunks)
    if virtual_size > 512 * 1024 * 1024:
        raise ValueError("Package exceeds the bounded 512 MiB static analysis profile")
    first = min(offset for offset, _, _, _ in chunks)
    if first > len(data):
        raise ValueError("Uncompressed summary prefix is outside the source file")
    result = bytearray(virtual_size)
    result[:first] = data[:first]
    previous_end = first
    for offset, size, compressed_offset, compressed_size in sorted(chunks):
        if offset != previous_end or compressed_offset + compressed_size > len(data):
            raise ValueError("Package chunks contain a gap, overlap or truncated payload")
        chunk = memoryview(data)[compressed_offset:compressed_offset + compressed_size]
        if len(chunk) < 16:
            raise ValueError("Truncated compressed chunk header")
        tag, block_size, compressed_total, uncompressed_total = struct.unpack_from("<4I", chunk)
        if tag != TAG or block_size == 0 or uncompressed_total != size:
            raise ValueError("Compressed chunk header contradicts the package summary")
        blocks = (size + block_size - 1) // block_size
        header_size = 16 + blocks * 8
        if header_size + compressed_total != len(chunk):
            raise ValueError("Compressed chunk extent does not match its block table")
        cursor = header_size
        write_cursor = offset
        compressed_seen = 0
        for index in range(blocks):
            compressed, uncompressed = struct.unpack_from("<II", chunk, 16 + index * 8)
            expected = min(block_size, offset + size - write_cursor)
            if uncompressed != expected or cursor + compressed > len(chunk):
                raise ValueError("Invalid compressed block extent")
            payload = bytes(chunk[cursor:cursor + compressed])
            decoded = zlib.decompress(payload) if flags == 1 else lzo.decompress(
                payload, header=False, buflen=uncompressed
            )
            if len(decoded) != uncompressed:
                raise ValueError("Decoded block length contradicts the package summary")
            result[write_cursor:write_cursor + uncompressed] = decoded
            cursor += compressed
            write_cursor += uncompressed
            compressed_seen += compressed
        if compressed_seen != compressed_total or write_cursor != offset + size:
            raise ValueError("Compressed chunk totals are inconsistent")
        previous_end = offset + size
    return bytes(result)
