import unittest

from verify_native_camcorder import freshness_grade


def event(now=500_000, sampled=250_000, matches=True, current=True, held=True):
    return dict(clock=now, body_request_schema_version=1, body_request_clock_us=sampled,
                body_request_age_us=now - sampled if sampled is not None and now >= sampled else None,
                body_request_matches_hero=matches, body_request_current=current, held=held)


class NativeCameraFreshnessTests(unittest.TestCase):
    def test_exact_boundary_remains_fresh(self):
        self.assertEqual(freshness_grade([event()])["current_updates"], 1)

    def test_expired_body_is_released_without_relaxing_the_gate(self):
        row = event(now=500_001, current=False, held=False)
        self.assertEqual(freshness_grade([row])["expired_updates"], 1)
        row["held"] = True
        with self.assertRaises(ValueError):
            freshness_grade([row])

    def test_future_and_absent_requests_are_not_current(self):
        rows = [event(sampled=500_001, current=False, held=False),
                event(sampled=None, matches=False, current=False, held=False)]
        result = freshness_grade(rows)
        self.assertEqual(result["future_clock_updates"], 1)
        self.assertEqual(result["absent_request_updates"], 1)

    def test_other_hero_cannot_retain_held_camera(self):
        row = event(matches=False, current=False, held=False)
        self.assertEqual(freshness_grade([row])["mismatched_hero_updates"], 1)
        row["body_request_current"] = True
        with self.assertRaises(ValueError):
            freshness_grade([row])

    def test_fabricated_ages_versions_and_types_are_rejected(self):
        for key, value in (("body_request_age_us", 0), ("body_request_age_us", True),
                           ("body_request_schema_version", True), ("body_request_schema_version", 2),
                           ("body_request_current", 1), ("body_request_matches_hero", "true"),
                           ("body_request_clock_us", True), ("clock", -1), ("held", 1)):
            with self.subTest(key=key, value=value):
                row = event()
                row[key] = value
                with self.assertRaises(ValueError):
                    freshness_grade([row])

    def test_no_request_cannot_match_a_hero_or_invent_an_age(self):
        for key, value in (("body_request_matches_hero", True), ("body_request_age_us", 0)):
            row = event(sampled=None, matches=False, current=False, held=False)
            row[key] = value
            with self.assertRaises(ValueError):
                freshness_grade([row])
        with self.assertRaises(ValueError):
            freshness_grade([])

    def test_missing_native_fields_cannot_be_treated_as_a_valid_default(self):
        for key in event():
            row = event()
            del row[key]
            with self.assertRaises(ValueError):
                freshness_grade([row])


if __name__ == "__main__":
    unittest.main()
