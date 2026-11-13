import unittest

from verify_frame_budget import grade


class FrameBudgetTests(unittest.TestCase):
    def rows(self):
        return [dict(frame_id=n, elapsed_ms=n*1000/90, app_cpu_ms=4., app_gpu_ms=8.,
                     dropped=0, reprojected=0) for n in range(10802)]

    def test_cpu_and_gpu_overlap_not_sum_and_xr_wait_is_not_work(self):
        result = grade(self.rows(), 90)
        self.assertEqual(result["status"], "passed")
        self.assertAlmostEqual(result["budget_ms"], 1000/90)

    def test_gpu_headroom_tail_and_reprojection_fail_separately(self):
        for kind, diagnostic in (("headroom", "gpu p95"), ("tail", "gpu p99"),
                                 ("reprojected", "1%"), ("pacing", "pacing"), ("stall", "stall")):
            rows = self.rows()
            for n, row in enumerate(rows):
                if kind == "headroom":
                    row["app_gpu_ms"] = 10.
                elif kind == "tail" and n % 50 == 0:
                    row["app_gpu_ms"] = 20.
                elif kind == "reprojected" and n % 50 == 0:
                    row["reprojected"] = 1
                elif kind == "pacing" and n % 50 == 1:
                    row["elapsed_ms"] += 3
                elif kind == "stall" and n == 50:
                    row["app_gpu_ms"] = 100.
            with self.subTest(kind=kind):
                result = grade(rows, 90)
                self.assertEqual(result["status"], "failed")
                self.assertTrue(any(diagnostic in f for f in result["failures"]))

    def test_invalid_partial_and_wrong_refresh_captures_are_not_passes(self):
        for kind, diagnostic in (("nan", "app_gpu_ms"), ("gap", "frame"),
                                 ("clock", "clock"), ("short", "120 seconds"),
                                 ("refresh", "cadence"), ("flag", "flag"), ("zero", "measured")):
            rows = self.rows()
            hz = 90
            if kind == "nan":
                rows[50]["app_gpu_ms"] = float("nan")
            elif kind == "gap":
                rows[50]["frame_id"] += 1
            elif kind == "clock":
                rows[50]["elapsed_ms"] = rows[49]["elapsed_ms"]
            elif kind == "short":
                rows = rows[:100]
            elif kind == "refresh":
                hz = 120
            elif kind == "flag":
                rows[50]["dropped"] = 2
            elif kind == "zero":
                for row in rows:
                    row["app_gpu_ms"] = 0
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, diagnostic):
                grade(rows, hz)
