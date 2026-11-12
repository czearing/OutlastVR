import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("release_notes", ROOT / "tools" / "release_notes.py")
notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notes)
TEMPLATE = (ROOT / "releases" / "template.md").read_text(encoding="utf-8")


class ReleaseNotesTests(unittest.TestCase):
    def data(self, changes=None, issues=None):
        return {"version": "0.1.0-rc.4", "changes": changes or ["Fixed hands getting stuck near walls."],
                "known_issues": issues or []}

    def test_template_has_only_player_changes_and_optional_known_issues(self):
        self.assertEqual(notes.render_release_notes("v0.1.0-rc.4", self.data(), TEMPLATE),
                         "## Changes\n\n- Fixed hands getting stuck near walls.\n")
        result = notes.render_release_notes(
            "0.1.0-rc.4", self.data(issues=["Physical camcorder controls are experimental."]), TEMPLATE)
        self.assertIn("\n\n## Known issues\n\n- Physical camcorder controls are experimental.\n", result)

    def test_rejects_jargon_verbose_duplicate_and_formatted_entries(self):
        for text in ("Fixed tracking \u2014 improved hands.", "Fixed tracking \u2013 improved hands.",
                     "Fixed hands.\nVerification passed.", "Fixed DLL hooks.",
                     "Verified 96 tests.", "Improved regression fixtures.", "Fixed " + "words " * 26,
                     "Fixed `native_cursor`.", "Amazing, rock-solid production-ready fixes!"):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    notes.render_release_notes("v0.1.0-rc.4", self.data([text]), TEMPLATE)
        data = self.data()
        data["changes"] *= 2
        with self.assertRaises(ValueError):
            notes.render_release_notes("v0.1.0-rc.4", data, TEMPLATE)

    def test_rejects_missing_mismatched_and_path_like_versions(self):
        for tag in ("../../notes", "v0.1.0-rc.5", "0.1.0/notes", "v0.1.0\n"):
            with self.subTest(tag=tag):
                with self.assertRaises(ValueError):
                    notes.render_release_notes(tag, self.data(), TEMPLATE)
        for data in ({}, {**self.data(), "tests": "passed"}, {**self.data(), "changes": []},
                     {**self.data(), "changes": "Fixed hands."}):
            with self.assertRaises(ValueError):
                notes.render_release_notes("v0.1.0-rc.4", data, TEMPLATE)

    def test_every_committed_release_has_valid_notes(self):
        for path in (ROOT / "releases").glob("*.json"):
            with self.subTest(path=path.name):
                notes.render_release_notes(path.stem, json.loads(path.read_text(encoding="utf-8")), TEMPLATE)

    def test_cli_writes_template_and_refuses_missing_notes_or_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "notes.md"
            command = [sys.executable, str(ROOT / "tools" / "release_notes.py"),
                       "--tag", "v0.1.0-rc.4", "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            rendered = output.read_text(encoding="utf-8")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_text(encoding="utf-8"), rendered)
            command[3] = "v999.0.0"
            command[-1] = str(Path(directory) / "missing.md")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(Path(command[-1]).exists())


if __name__ == "__main__":
    unittest.main()
