import copy
import unittest

from verify_native_glass_raster import validate_reference_layout


def layout():
    return dict(width=128, height=32, reference_eye_ownership_version=1,
                reference_source_viewport=[0, 0, 128, 32],
                reference_native_scissor_enabled=False,
                reference_tracked_scissors=[[0, 0, 64, 32], [0, 0, 64, 32]],
                reference_eye_scissors=[[0, 0, 64, 32], [64, 0, 128, 32]],
                reference_raster_mappings=[[.5, -.5, 0, 0], [.5, .5, 0, 0]])


class OriginalReferenceOwnershipTests(unittest.TestCase):
    def test_declared_eyes_are_isolated(self):
        self.assertTrue(validate_reference_layout(layout(), 1))
        self.assertFalse(validate_reference_layout({}, 0))

    def test_spill_and_relabel_are_rejected(self):
        for key, value in (("reference_eye_ownership_version", 0),
                           ("reference_eye_ownership_version", True),
                           ("reference_eye_scissors", [[0, 0, 128, 32], [64, 0, 128, 32]]),
                           ("reference_native_scissor_enabled", 0)):
            damaged = layout()
            damaged[key] = value
            with self.assertRaises(ValueError):
                validate_reference_layout(damaged, 1)
        with self.assertRaises(ValueError):
            validate_reference_layout({}, 1)

    def test_offset_viewport_and_native_clipping(self):
        sample = layout()
        sample.update(reference_source_viewport=[16, 4, 96, 24],
                      reference_native_scissor_enabled=True,
                      reference_tracked_scissors=[[12, 6, 52, 26], [12, 6, 52, 26]],
                      reference_eye_scissors=[[12, 6, 52, 26], [76, 6, 116, 26]],
                      reference_raster_mappings=[[.375, -.5, 0, 0], [.375, .5, 0, 0]])
        self.assertTrue(validate_reference_layout(sample, 1))
        damaged = copy.deepcopy(sample)
        damaged["reference_raster_mappings"] = [[.5, -.5, 0, 0], [.5, .5, 0, 0]]
        with self.assertRaises(ValueError):
            validate_reference_layout(damaged, 1)

    def test_empty_and_outside_native_rectangles_remain_bounded(self):
        sample = layout()
        sample.update(reference_native_scissor_enabled=True,
                      reference_tracked_scissors=[[90, 2, 100, 30], [-100, 2, -90, 30]],
                      reference_eye_scissors=[[64, 2, 64, 30], [64, 2, 64, 30]])
        self.assertTrue(validate_reference_layout(sample, 1))
        sample["reference_source_viewport"][2] = 127
        with self.assertRaises(ValueError):
            validate_reference_layout(sample, 1)

    def test_nonfinite_mapping_and_inverted_rectangles_are_rejected(self):
        for mutate in (
            lambda s: s["reference_raster_mappings"][0].__setitem__(0, float("nan")),
            lambda s: s["reference_tracked_scissors"][0].__setitem__(0, 65),
            lambda s: s["reference_source_viewport"].__setitem__(0, -2),
        ):
            sample = layout()
            mutate(sample)
            with self.assertRaises(ValueError):
                validate_reference_layout(sample, 1)


if __name__ == "__main__":
    unittest.main()
