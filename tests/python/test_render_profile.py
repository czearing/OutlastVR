import unittest

from verify_native_probe import render_profile


class RenderProfileTests(unittest.TestCase):
    def frames(self):
        return [dict(native_frame=n, world_panel=n < 40, source_width=512, source_height=512,
                     cpu_render_us=2000, xr_wait_us=7000, shader_variants=3,
                     gpu_timing_status=2 if n >= 8 else 1, gpu_timing_frame=max(0, n-8),
                     gpu_work_us=9000 if n-8 < 40 else 3000)
                for n in range(80)]

    def test_delayed_gpu_results_follow_source_phase_and_warmup(self):
        result = render_profile(self.frames())
        self.assertEqual(result["menu"]["gpu_samples"], 10)
        self.assertEqual(result["menu"]["gpu_render"]["p50_ms"], 9)
        self.assertEqual(result["gameplay"]["gpu_samples"], 2)
        self.assertEqual(result["gameplay"]["gpu_render"]["p50_ms"], 3)
        self.assertEqual(result["unattributed_gpu_samples"], 0)

    def test_repeated_results_are_counted_once(self):
        frames = self.frames()
        for frame in frames[72:]:
            frame.update(gpu_timing_frame=70, gpu_work_us=3000)
        self.assertEqual(render_profile(frames)["gameplay"]["gpu_samples"], 1)

    def test_missing_source_identity_cannot_certify_gpu_cost(self):
        frames = self.frames()
        for frame in frames:
            del frame["native_frame"]
        result = render_profile(frames)
        self.assertIsNone(result["gameplay"]["gpu_render"])
        self.assertGreater(result["unattributed_gpu_samples"], 0)

    def test_malformed_or_conflicting_samples_fail_explicitly(self):
        for field, value, error in (
                ("gpu_work_us", float("nan"), "Invalid GPU"),
                ("cpu_render_us", float("nan"), "Invalid CPU"),
                ("gpu_timing_frame", 900, "future native"),
                ("gpu_timing_frame", 63, "Conflicting GPU"),
                ("native_frame", 78, "Duplicate native")):
            frames = self.frames()
            frames[79][field] = value
            if field == "gpu_timing_frame" and value == 63:
                frames[79]["gpu_work_us"] = 4000
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, error):
                render_profile(frames)
