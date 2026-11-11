import copy
import unittest

from verify_native_reflective_glass import projection_readback_policy


class NativeMaterialReadback(unittest.TestCase):
    def fixture(self):
        identity = dict(no_projection_images_fixture_version=2,
                        asymmetric_fov_fixture_version=1, recommended_resolution=True,
                        headset_extent_2688x2880=True, draw_pixel_readback="constants_only",
                        draw_hdr=False)
        frames = [dict(no_projection_images_fixture_version=2,
                       asymmetric_fov_fixture_version=1, captured=False,
                       eye_width=2688, eye_height=2880,
                       views=[dict(fov=[-1., .85, .9, -.95]),
                              dict(fov=[-.85, 1., .9, -.95])])]
        return identity, frames

    def test_bounded_original_material_fixture(self):
        identity, frames = self.fixture()
        result = projection_readback_policy(identity, frames)
        self.assertTrue(result["asymmetric_fov_is_synthetic_stress_fixture"])
        self.assertFalse(result["projection_image_stereo_acceptance_claimed"])

    def test_legacy_fixture_keeps_original_policy(self):
        result = projection_readback_policy({}, [dict(captured=True)])
        self.assertEqual(result["no_projection_images_fixture_version"], 0)
        self.assertFalse(result["asymmetric_fov_is_synthetic_stress_fixture"])

    def test_rejects_unrecognized_or_mislabeled_manifest(self):
        identity, frames = self.fixture()
        for key, value in (
            ("no_projection_images_fixture_version", True),
            ("no_projection_images_fixture_version", 1),
            ("asymmetric_fov_fixture_version", True),
            ("asymmetric_fov_fixture_version", 2),
            ("recommended_resolution", False), ("headset_extent_2688x2880", False),
            ("draw_pixel_readback", "images"), ("draw_hdr", True),
        ):
            with self.subTest(key=key, value=value):
                bad = dict(identity, **{key: value})
                with self.assertRaises(ValueError):
                    projection_readback_policy(bad, frames)

    def test_rejects_changed_or_missing_actual_frame_policy(self):
        identity, frames = self.fixture()
        for key, value in (
            ("captured", True), ("captured", None), ("eye_width", 3840),
            ("eye_height", 2160), ("asymmetric_fov_fixture_version", 0),
            ("no_projection_images_fixture_version", 0),
            ("no_projection_images_fixture_version", True),
        ):
            with self.subTest(key=key, value=value):
                bad = copy.deepcopy(frames)
                bad[0][key] = value
                with self.assertRaises(ValueError):
                    projection_readback_policy(identity, bad)
        with self.assertRaises(ValueError):
            projection_readback_policy(identity, [])

    def test_rejects_wrong_eye_fov_and_swapped_eyes(self):
        identity, frames = self.fixture()
        for eye in range(2):
            bad = copy.deepcopy(frames)
            bad[0]["views"][eye]["fov"][0] = -.8
            with self.assertRaises(ValueError):
                projection_readback_policy(identity, bad)
        frames[0]["views"].reverse()
        with self.assertRaises(ValueError):
            projection_readback_policy(identity, frames)


if __name__ == "__main__":
    unittest.main()
