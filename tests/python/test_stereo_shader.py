import struct
import unittest

from sm3 import instructions, resources
from stereo_shader import declaration, op, patch_pair, patch_pixel, patch_vertex, reg


def shader(version, *parts):
    words = [version, *(word for part in parts for word in part), 0xFFFF]
    return struct.pack(f"<{len(words)}I", *words)


def vertex():
    return shader(0xFFFE0300, declaration(0, 0, 1, 0),
                  declaration(0, 0, 6, 0), declaration(5, 0, 6, 1, 7),
                  op(1, reg(6, 0, mask=15), reg(1, 0)),
                  op(1, reg(6, 1, mask=7), reg(1, 0)))


def pixel(full=False, masks=7):
    inputs = [declaration(5, index, 1, index, masks) for index in range(8)] if full else [declaration(5, 0, 1, 0, 7)]
    if full:
        inputs += [declaration(10, index - 8, 1, index, masks) for index in range(8, 10)]
    return shader(0xFFFF0300, *inputs, op(1, reg(8, 0, mask=15), reg(1, 0)))


def dense_vertex():
    outputs = [declaration(5, index, 6, index + 1, 7) for index in range(8)]
    outputs += [declaration(10, index - 8, 6, index + 1, 7) for index in range(8, 10)]
    writes = [op(1, reg(6, index + 1, mask=7), reg(1, 0)) for index in range(10)]
    return shader(0xFFFE0300, declaration(0, 0, 1, 0),
                  declaration(0, 0, 6, 0), *outputs,
                  op(1, reg(6, 0, mask=15), reg(1, 0)), *writes)


class StereoShaderTests(unittest.TestCase):
    def test_single_pass_instructions_use_add_neg_not_sub(self):
        vs, vm = patch_vertex(vertex())
        ps, pm = patch_pixel(pixel())
        self.assertNotIn(3, [opcode for opcode, _ in instructions(vs)])
        self.assertNotIn(3, [opcode for opcode, _ in instructions(ps)])
        self.assertEqual(vm["eye_semantic"], pm["eye_semantic"])
        self.assertFalse(pm["requires_paired_vertex_variant"])

    def test_definitions_precede_declarations(self):
        code, _ = patch_vertex(vertex())
        body_started = False
        declarations_started = False
        for opcode, _ in instructions(code):
            if opcode == 31:
                declarations_started = True
            elif opcode in (47, 48, 81):
                self.assertFalse(declarations_started)
                self.assertFalse(body_started)
            elif opcode != 0xFFFE:
                body_started = True

    def test_full_pixel_inputs_pack_only_an_undeclared_component(self):
        (vs, vm), (ps, pm) = patch_pair(vertex(), pixel(full=True))
        self.assertTrue(pm["requires_paired_vertex_variant"])
        self.assertEqual((pm["eye_semantic"], pm["eye_component"]), (0, 3))
        self.assertEqual(vm["eye_component"], 3)
        self.assertEqual(len([d for d in resources(ps)["declarations"] if d["register_type"] == 1]), 10)
        output = next(d for d in resources(vs)["declarations"]
                      if d["register_type"] == 6 and d["usage"] == 5 and d["usage_index"] == 0)
        self.assertEqual(output["mask"], 15)
        self.assertNotIn(15, [d["usage_index"] for d in resources(ps)["declarations"] if d["usage"] == 5])

    def test_refuse_when_every_pixel_component_is_declared(self):
        with self.assertRaisesRegex(ValueError, "no undeclared component"):
            patch_pixel(pixel(full=True, masks=15))

    def test_refuse_mismatched_packed_pair(self):
        other_vertex = shader(0xFFFE0300, declaration(0, 0, 1, 0),
                              declaration(0, 0, 6, 0), op(1, reg(6, 0, mask=15), reg(1, 0)))
        with self.assertRaisesRegex(ValueError, "no common spare component"):
            patch_pair(other_vertex, pixel(full=True))

    def test_dense_vertex_packs_without_adding_an_eleventh_varying(self):
        code, metadata = patch_vertex(dense_vertex())
        self.assertTrue(metadata["packed_eye"])
        self.assertEqual((metadata["eye_semantic"], metadata["eye_component"]), (0, 3))
        self.assertEqual(len([d for d in resources(code)["declarations"]
                              if d["register_type"] == 6 and d["usage"] not in (0, 4)]), 10)

    def test_dense_vertex_requires_matching_packed_pixel_even_with_free_pixel_inputs(self):
        (vs, vm), (ps, pm) = patch_pair(dense_vertex(), pixel())
        self.assertTrue(vm["packed_eye"])
        self.assertTrue(pm["packed_eye"])
        self.assertEqual((vm["eye_semantic"], vm["eye_component"]),
                         (pm["eye_semantic"], pm["eye_component"]))
        self.assertEqual(len([d for d in resources(ps)["declarations"]
                              if d["register_type"] == 1]), 1)
        self.assertEqual(len([d for d in resources(vs)["declarations"]
                              if d["register_type"] == 6 and d["usage"] not in (0, 4)]), 10)

    def test_full_pixel_pair_chooses_a_component_produced_by_the_vertex_shader(self):
        other_vertex = shader(0xFFFE0300, declaration(0, 0, 1, 0),
                              declaration(0, 0, 6, 0), declaration(5, 2, 6, 1, 7),
                              op(1, reg(6, 0, mask=15), reg(1, 0)),
                              op(1, reg(6, 1, mask=7), reg(1, 0)))
        (_, vm), (_, pm) = patch_pair(other_vertex, pixel(full=True))
        self.assertEqual((vm["eye_semantic"], vm["eye_component"]), (2, 3))
        self.assertEqual((pm["eye_semantic"], pm["eye_component"]), (2, 3))

    def test_dense_pair_refuses_without_a_common_unused_pixel_component(self):
        with self.assertRaisesRegex(ValueError, "no common spare component"):
            patch_pair(dense_vertex(), pixel(full=True, masks=15))

    def test_refuse_conflicting_input_semantic(self):
        conflict = shader(0xFFFE0300, declaration(5, 15, 1, 0),
                          declaration(0, 0, 6, 0), op(1, reg(6, 0, mask=15), reg(1, 0)))
        with self.assertRaisesRegex(ValueError, "conflicts"):
            patch_vertex(conflict)

    def test_refuse_wrong_profiles_and_truncated_tokens(self):
        for code in (pixel(), b"", b"\0\3\xfe\xff"):
            with self.assertRaises((ValueError, struct.error)):
                patch_vertex(code)
        broken = struct.pack("<3I", 0xFFFE0300, 0x03000001, 0xFFFF)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            list(instructions(broken))


if __name__ == "__main__":
    unittest.main()
