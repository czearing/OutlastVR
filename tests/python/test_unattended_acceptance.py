import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "unattended_acceptance", Path(__file__).resolve().parents[2] / "tools" / "unattended_acceptance.py")
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


class UnattendedAcceptanceTests(unittest.TestCase):
    def test_negative_requires_exact_exit_and_oracle_not_any_crash(self):
        markers = ["REJECTED: compositor", "Compositor disparity failed:", "FATAL: xrEndFrame"]
        text = "\n".join(markers)
        acceptance.grade_result(0xC0000602, text, markers, 0xC0000602)
        for code, output in ((1, text), (0, text), (0xC0000005, text),
                             (0xC0000602, "FATAL: failed to load module")):
            with self.subTest(code=code, output=output):
                with self.assertRaises(ValueError):
                    acceptance.grade_result(code, output, markers, 0xC0000602)

    def test_positive_requires_complete_proof_and_no_hidden_error(self):
        with self.assertRaises(ValueError):
            acceptance.grade_result(0, "PASS: partial fixture", ["XR_cleanup_handles=4"])
        with self.assertRaisesRegex(ValueError, "concealed"):
            acceptance.grade_result(0, "PASS: complete\nFAIL: cleanup", ["PASS: complete"])
        acceptance.grade_result(0, "PASS: complete; XR_cleanup_handles=4", ["XR_cleanup_handles=4"])

    def test_junit_rejects_missing_duplicate_skipped_and_failed_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.xml"
            for body in ('<testcase name="a"/>', '<testcase name="a"/><testcase name="a"/>',
                         '<testcase name="a"/><testcase name="b"><skipped/></testcase>',
                         '<testcase name="a"/><testcase name="b"><failure/></testcase>',
                         '<testcase name="a"/><testcase name="b"><error/></testcase>',
                         '<testcase name="a"/><testcase name="b" status="notrun"/>',
                         '<testcase name="a"/><testcase name="b"><system-out>'
                         'Ran 10 tests\nOK (skipped=1)\n</system-out></testcase>'):
                path.write_text(f"<testsuite>{body}</testsuite>", encoding="utf-8")
                with self.subTest(body=body):
                    with self.assertRaises(ValueError):
                        acceptance.grade_junit(path, ["a", "b"])
            path.write_text('<testsuite><testcase name="b"/><testcase name="a"/></testsuite>', encoding="utf-8")
            self.assertEqual(acceptance.grade_junit(path, ["a", "b"]), 2)
            path.write_text('<testsuite skipped="1"><testcase name="a"/><testcase name="b"/></testsuite>',
                            encoding="utf-8")
            with self.assertRaises(ValueError):
                acceptance.grade_junit(path, ["a", "b"])


if __name__ == "__main__":
    unittest.main()
