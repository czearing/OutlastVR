import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get("OLVR_RETAIL_EXECUTABLE"), "Requires a private supported game executable")
class LauncherPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="outlastvr-launcher-test-")
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        launcher = Path(os.environ["OLVR_LAUNCHER"])
        self.launcher = self.folder / launcher.name
        shutil.copyfile(launcher, self.launcher)
        shutil.copyfile(launcher.with_name("OutlastVRSinglePass.dll"), self.folder / "OutlastVRSinglePass.dll")

    def run_verify(self):
        return subprocess.run([str(self.launcher), "--game", os.environ["OLVR_RETAIL_EXECUTABLE"], "--verify"],
                              capture_output=True, text=True, timeout=30)

    def test_complete_package_verifies_without_starting_game(self):
        shutil.copyfile(os.environ["OLVR_LOADER"], self.folder / "openxr_loader.dll")
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OpenXR loader; no game process created", result.stdout)

    def test_missing_loader_is_rejected(self):
        (self.folder / "OutlastVR.ini").write_text("[vr]\nopenxr_loader=missing-loader.dll\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Read configured OpenXR loader", result.stderr)

    def test_dll_without_openxr_entry_is_rejected(self):
        (self.folder / "OutlastVR.ini").write_text(
            "[vr]\nopenxr_loader=OutlastVRSinglePass.dll\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing xrGetInstanceProcAddr", result.stderr)
