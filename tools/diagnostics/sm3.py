"""Bounded SM3 token inspection shared by corpus analysis and stereo rewriting."""

import struct

def constant_table(code: bytes):
    cursor = 4
    while cursor + 4 <= len(code):
        token = struct.unpack_from("<I", code, cursor)[0]
        opcode = token & 0xFFFF
        if opcode == 0xFFFF:
            break
        words = (token >> 16) & 0x7FFF if opcode == 0xFFFE else (token >> 24) & 0xF
        if opcode == 0xFFFE and code[cursor + 4:cursor + 8] == b"CTAB":
            base = cursor + 8
            comment_end = cursor + 4 + words * 4
            size, creator, version, count, info, flags, target = struct.unpack_from("<7I", code, base)
            if size != 28 or base + info + count * 20 > comment_end:
                raise ValueError("Invalid CTAB constant table")
            constants = []
            for index in range(count):
                name_offset, register_set, register, register_count, reserved, type_offset, default = (
                    struct.unpack_from("<I4H2I", code, base + info + index * 20)
                )
                name_start = base + name_offset
                end = code.find(b"\0", name_start, comment_end)
                if name_start >= comment_end or end < 0:
                    raise ValueError("Invalid CTAB name")
                constants.append({
                    "name": code[name_start:end].decode("ascii"),
                    "register_set": register_set,
                    "register": register,
                    "count": register_count,
                })
            return constants
        cursor += (words + 1) * 4
    return []


def register_type(token):
    return ((token >> 28) & 7) | ((token >> 8) & 0x18)


def register_index(token):
    return token & 0x7FF


def instructions(code):
    if len(code) % 4 or len(code) < 8:
        raise ValueError("Invalid SM3 bytecode size")
    words = struct.unpack(f"<{len(code) // 4}I", code)
    if words[0] not in (0xFFFE0300, 0xFFFF0300):
        raise ValueError("Only SM3 vertex/pixel bytecode is supported")
    cursor = 1
    while cursor < len(words):
        token = words[cursor]
        opcode = token & 0xFFFF
        if opcode == 0xFFFF:
            if token != 0xFFFF or cursor + 1 != len(words):
                raise ValueError("Invalid SM3 terminator")
            return
        count = (token >> 16) & 0x7FFF if opcode == 0xFFFE else (token >> 24) & 0xF
        if cursor + 1 + count > len(words):
            raise ValueError("Truncated SM3 instruction")
        yield opcode, words[cursor:cursor + count + 1]
        cursor += count + 1
    raise ValueError("SM3 bytecode has no terminator")


def resources(code):
    used = {index: set() for index in range(20)}
    declarations = []
    count = 0
    for opcode, tokens in instructions(code):
        if opcode == 0xFFFE:
            continue
        count += 1
        if opcode == 31:
            declarations.append({
                "usage": tokens[1] & 0xF,
                "usage_index": (tokens[1] >> 16) & 0xF,
                "register_type": register_type(tokens[2]),
                "register": register_index(tokens[2]),
                "mask": (tokens[2] >> 16) & 0xF,
            })
            operands = tokens[2:]
        elif opcode in (47, 48, 81):
            operands = tokens[1:2]
        else:
            operands = tokens[1:]
        for token in operands:
            kind = register_type(token)
            if kind in used:
                used[kind].add(register_index(token))
    constants = constant_table(code)
    for constant in constants:
        if constant["register_set"] == 2:
            used[2].update(range(constant["register"], constant["register"] + constant["count"]))
    return {
        "constants": constants,
        "used": used,
        "declarations": declarations,
        "instruction_count": count,
        "free_constants": [index for index in range(256) if index not in used[2]],
        "free_temps": [index for index in range(32) if index not in used[0]],
        "free_inputs": [index for index in range(16) if index not in used[1]],
        "free_outputs": [index for index in range(12) if index not in used[6]],
        "view_projection": any(
            value["name"] == "ViewProjectionMatrix" and value["register"] == 0
            and value["count"] == 4 and value["register_set"] == 2 for value in constants
        ),
    }
