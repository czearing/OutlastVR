"""SM3 single-draw stereo transformation with register-safe vertex texture fetch."""

import hashlib
import struct

from sm3 import instructions, register_index, register_type, resources


def reg(kind, index, *, mask=None, swizzle=0xE4):
    value = 0x80000000 | ((kind & 7) << 28) | ((kind & 0x18) << 8) | index
    return value | ((mask if mask is not None else swizzle) << 16)


def op(opcode, *operands):
    return [(len(operands) << 24) | opcode, *operands]


def declaration(usage, index, kind, number, mask=15):
    return op(31, 0x80000000 | usage | (index << 16), reg(kind, number, mask=mask))


def split(code):
    header, body = [], []
    in_body = False
    for opcode, tokens in instructions(code):
        if opcode not in (0xFFFE, 31, 47, 48, 81):
            in_body = True
        (body if in_body else header).extend(tokens)
    return header, body


def replace_register(token, kind, number):
    return (token & ~(0x70001800 | 0x7FF)) | ((kind & 7) << 28) | ((kind & 0x18) << 8) | number


def canonical_header(header):
    comments, definitions, declarations = [], [], []
    cursor = 0
    while cursor < len(header):
        opcode = header[cursor] & 0xFFFF
        count = (header[cursor] >> 16) & 0x7FFF if opcode == 0xFFFE else (header[cursor] >> 24) & 15
        tokens = header[cursor:cursor + count + 1]
        if opcode == 0xFFFE:
            comments.extend(tokens)
        elif opcode == 31:
            declarations.append(tokens)
        else:
            definitions.extend(tokens)
        cursor += count + 1
    declarations.sort(key=lambda item: (
        {17: 0, 1: 1, 10: 2, 6: 3}.get(register_type(item[2]), 4),
        register_index(item[2]),
    ))
    return comments + definitions + [token for item in declarations for token in item]


def extend_declaration(header, kind, number, component):
    cursor = 0
    while cursor < len(header):
        opcode = header[cursor] & 0xFFFF
        count = (header[cursor] >> 16) & 0x7FFF if opcode == 0xFFFE else (header[cursor] >> 24) & 15
        if opcode == 31 and register_type(header[cursor + 2]) == kind and register_index(header[cursor + 2]) == number:
            header[cursor + 2] |= 1 << (16 + component)
            return
        cursor += count + 1
    raise ValueError("Packed eye declaration was not found")


def needs_packed_vertex(info):
    return sum(item["register_type"] == 6 and item["usage"] not in (0, 4)
               for item in info["declarations"]) >= 10


def patch_vertex(code, *, eye_semantic=15, eye_input_semantic=15, packed_component=None):
    if struct.unpack_from("<I", code)[0] != 0xFFFE0300:
        raise ValueError("Expected an SM3 vertex shader")
    info = resources(code)
    if packed_component is None and needs_packed_vertex(info):
        candidates = [(item, component) for item in info["declarations"]
                      if item["register_type"] == 6 and item["usage"] == 5
                      for component in range(4) if not item["mask"] & (1 << component)]
        if not candidates:
            raise ValueError("Vertex shader has no undeclared component within the ten-varying limit")
        item, packed_component = candidates[0]
        eye_semantic = item["usage_index"]
    if not info["free_constants"] or not info["free_inputs"] or (packed_component is None and not info["free_outputs"]):
        raise ValueError("Vertex shader has no spare constant, input or output")
    if packed_component is not None and packed_component not in range(4):
        raise ValueError("Packed eye component must be between zero and three")
    if any(item["usage"] == 5 and
           ((packed_component is None and item["register_type"] == 6 and item["usage_index"] == eye_semantic) or
            (item["register_type"] == 1 and item["usage_index"] == eye_input_semantic))
           for item in info["declarations"]):
        raise ValueError("Eye semantic conflicts with the original vertex shader")
    positions = [item for item in info["declarations"]
                 if item["register_type"] == 6 and item["usage"] == 0 and item["usage_index"] == 0]
    if len(positions) != 1:
        raise ValueError("Vertex shader must have one declared position output")
    mapped = {}
    if info["view_projection"]:
        mapped.update({index: None for index in range(4)})
        for name, number in (("CameraPositionVS", 4), ("PreViewTranslation", 5)):
            if any(item["name"] == name and item["register"] == number for item in info["constants"]):
                mapped[number] = None
    available_samplers = [index for index in range(4) if index not in info["used"][10]]
    if mapped and not available_samplers:
        raise ValueError("No free vertex sampler for stereo constants")
    if len(info["free_temps"]) < len(mapped) + 2:
        raise ValueError("Insufficient temporary registers for stereo transformation")
    for index, original in enumerate(mapped):
        mapped[original] = info["free_temps"][index]
    uv = info["free_temps"][len(mapped)]
    position = info["free_temps"][len(mapped) + 1]
    eye_input = info["free_inputs"][0]
    if packed_component is None:
        eye_output = info["free_outputs"][0]
        eye_component = 0
    else:
        matching = [item for item in info["declarations"] if item["register_type"] == 6
                    and item["usage"] == 5 and item["usage_index"] == eye_semantic]
        if len(matching) != 1:
            raise ValueError("Paired vertex shader does not produce the packed pixel semantic")
        eye_output = matching[0]["register"]
        eye_component = packed_component
    coordinate = info["free_constants"][0]
    position_output = positions[0]["register"]
    sampler = available_samplers[0] if mapped else None
    header, _ = split(code)
    header += declaration(5, eye_input_semantic, 1, eye_input, 15)
    if packed_component is None:
        header += declaration(5, eye_semantic, 6, eye_output, 1)
    else:
        extend_declaration(header, 6, eye_output, eye_component)
    if mapped:
        header += op(31, 0x90000000, reg(10, sampler, mask=15))
    header += op(81, reg(2, coordinate, mask=15),
                 *struct.unpack("<4I", struct.pack("<4f", .125, .5, .0625, .25)))
    prefix = []
    if mapped:
        prefix += op(1, reg(0, uv, mask=1), reg(2, coordinate, swizzle=0xAA))
        prefix += op(4, reg(0, uv, mask=2), reg(1, eye_input, swizzle=0),
                     reg(2, coordinate, swizzle=0x55), reg(2, coordinate, swizzle=0xFF))
        prefix += op(2, reg(0, uv, mask=12), reg(2, coordinate), reg(2, coordinate) | 0x01000000)
        for row in range(max(mapped) + 1):
            if row in mapped:
                prefix += op(95, reg(0, mapped[row], mask=15), reg(0, uv), reg(10, sampler))
            prefix += op(2, reg(0, uv, mask=1), reg(0, uv, swizzle=0), reg(2, coordinate, swizzle=0))
    body = []
    in_body = False
    for opcode, original in instructions(code):
        if opcode not in (0xFFFE, 31, 47, 48, 81):
            in_body = True
        if not in_body:
            continue
        tokens = list(original)
        if opcode not in (0xFFFE, 31, 47, 48, 81):
            for index, token in enumerate(tokens[1:], 1):
                kind, number = register_type(token), register_index(token)
                if kind == 2 and number in mapped:
                    if token & 0x2000:
                        raise ValueError("Relative view-register addressing is not supported")
                    tokens[index] = replace_register(token, 0, mapped[number])
                if kind == 6 and number == position_output:
                    if index != 1:
                        raise ValueError("Original vertex shader reads its position output")
                    tokens[index] = replace_register(token, 0, position)
        body.extend(tokens)
    suffix = op(2, reg(0, uv, mask=2), reg(1, eye_input, swizzle=0),
                reg(2, coordinate, swizzle=0x55) | 0x01000000)
    suffix += op(5, reg(0, uv, mask=1), reg(0, position, swizzle=0xFF), reg(0, uv, swizzle=0x55))
    suffix += op(4, reg(6, position_output, mask=1), reg(0, position, swizzle=0),
                 reg(2, coordinate, swizzle=0x55), reg(0, uv, swizzle=0))
    suffix += op(1, reg(6, position_output, mask=14), reg(0, position))
    suffix += op(1, reg(6, eye_output, mask=1 << eye_component), reg(1, eye_input, swizzle=0))
    words = [0xFFFE0300, *canonical_header(header), *prefix, *body, *suffix, 0xFFFF]
    transformed = struct.pack(f"<{len(words)}I", *words)
    return transformed, {
        "mode": "view_texture" if mapped else "clip_atlas",
        "eye_semantic": eye_semantic, "eye_input_semantic": eye_input_semantic,
        "eye_component": eye_component, "packed_eye": packed_component is not None,
        "requires_paired_pixel_variant": packed_component is not None,
        "eye_input_register": eye_input,
        "eye_output_register": eye_output, "vertex_sampler": sampler,
        "mapped_view_registers": sorted(mapped),
        "source_sha256": hashlib.sha256(code).hexdigest(),
        "transformed_sha256": hashlib.sha256(transformed).hexdigest(),
    }


def patch_pixel(code, *, eye_semantic=15, packed_semantic=None, packed_component=None):
    if struct.unpack_from("<I", code)[0] != 0xFFFF0300:
        raise ValueError("Expected an SM3 pixel shader")
    info = resources(code)
    available = [index for index in range(10) if index not in info["used"][1]]
    constants = [index for index in range(224) if index not in info["used"][2]]
    if not constants or not info["free_temps"]:
        raise ValueError("Pixel shader has no spare guard constant or temporary")
    if any(item["usage"] == 5 and item["usage_index"] == eye_semantic for item in info["declarations"]):
        raise ValueError("Eye semantic conflicts with the original pixel shader")
    if (packed_semantic is None) != (packed_component is None):
        raise ValueError("Packed pixel semantic and component must be specified together")
    if packed_component is not None and (packed_component not in range(4) or packed_semantic not in range(16)):
        raise ValueError("Packed pixel semantic or component is outside the SM3 range")
    packed = not available or packed_component is not None
    eye_component = 0
    if packed:
        candidates = [(item, component) for item in info["declarations"]
                      if item["register_type"] == 1 and item["usage"] == 5
                      for component in range(4) if not item["mask"] & (1 << component)
                      and (packed_component is None or
                           (item["usage_index"] == packed_semantic and component == packed_component))]
        if not candidates:
            raise ValueError("Pixel shader has no undeclared component for a paired eye varying")
        item, eye_component = candidates[0]
        eye_input, eye_semantic = item["register"], item["usage_index"]
    else:
        eye_input = available[0]
    guard, temporary = constants[0], info["free_temps"][0]
    header, body = split(code)
    if packed:
        extend_declaration(header, 1, eye_input, eye_component)
    else:
        header += declaration(5, eye_semantic, 1, eye_input, 1)
    if 0 not in info["used"][17]:
        header += declaration(0, 0, 17, 0, 3)
    prefix = op(4, reg(0, temporary, mask=1), reg(1, eye_input, swizzle=eye_component * 0x55),
                reg(2, guard, swizzle=0x55), reg(2, guard, swizzle=0xAA))
    prefix += op(2, reg(0, temporary, mask=2), reg(17, 0, swizzle=0),
                 reg(2, guard, swizzle=0) | 0x01000000)
    prefix += op(5, reg(0, temporary, mask=1), reg(0, temporary, swizzle=0), reg(0, temporary, swizzle=0x55))
    prefix += op(1, reg(0, temporary, mask=14), reg(2, guard, swizzle=0xFF))
    prefix += op(65, reg(0, temporary, mask=15))
    words = [0xFFFF0300, *canonical_header(header), *prefix, *body, 0xFFFF]
    transformed = struct.pack(f"<{len(words)}I", *words)
    return transformed, {
        "eye_semantic": eye_semantic, "eye_input_register": eye_input,
        "eye_component": eye_component, "packed_eye": packed,
        "requires_paired_vertex_variant": packed,
        "guard_constant": guard,
        "guard_value": "[atlas viewport midpoint X, 2, -1, 0]",
        "source_sha256": hashlib.sha256(code).hexdigest(),
        "transformed_sha256": hashlib.sha256(transformed).hexdigest(),
        "limitations": "Seam protection only; per-eye screen-space uniforms and sampler coordinates are separate obligations.",
    }


def patch_pair(vertex, pixel):
    vertex_info = resources(vertex)
    pixel_info = resources(pixel)
    if needs_packed_vertex(vertex_info) or len(pixel_info["used"][1]) >= 10:
        semantics = {item["usage_index"] for item in vertex_info["declarations"]
                     if item["register_type"] == 6 and item["usage"] == 5}
        candidates = [(item["usage_index"], component) for item in pixel_info["declarations"]
                      if item["register_type"] == 1 and item["usage"] == 5 and item["usage_index"] in semantics
                      for component in range(4) if not item["mask"] & (1 << component)]
        if not candidates:
            raise ValueError("Shader pair has no common spare component within the ten-varying limit")
        semantic, component = candidates[0]
        pixel_code, pixel_metadata = patch_pixel(pixel, packed_semantic=semantic, packed_component=component)
    else:
        pixel_code, pixel_metadata = patch_pixel(pixel)
    vertex_code, vertex_metadata = patch_vertex(
        vertex, eye_semantic=pixel_metadata["eye_semantic"],
        packed_component=pixel_metadata["eye_component"] if pixel_metadata["packed_eye"] else None,
    )
    return (vertex_code, vertex_metadata), (pixel_code, pixel_metadata)
