"""Verify exactly the release files, manifest, DLL dependencies and safe preflight."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import re
import subprocess


def verify_documentation(readme, source_commit):
    if not isinstance(source_commit, str) or not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ValueError("Package documentation requires a full source commit")
    revisions = re.findall(
        r"https://github\.com/czearing/OutlastVR/(?:blob|tree)/([^/\s)]+)/", readme)
    if not revisions or any(revision != source_commit for revision in revisions):
        raise ValueError("Packaged documentation links must use the package source commit")


def verify(folder):
    folder = folder.resolve(strict=True)
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8-sig"))
    if manifest["physical_camcorder_default"] is not False:
        raise ValueError("Physical camcorder must be opt-in")
    expected = {"OutlastVRLauncher.exe", "OutlastVRSinglePass.dll", "openxr_loader.dll",
                "OutlastVR.ini", "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md",
                "licenses/MinHook.txt", "licenses/OpenXR.txt", "licenses/OpenXR-MIT.txt", "licenses/JsonCpp.txt",
                "licenses/Winpthreads.txt"}
    listed = [entry["path"] for entry in manifest["files"]]
    if len(listed) != len(set(listed)) or set(listed) != expected:
        raise ValueError("Package manifest contains missing, duplicate or unapproved files")
    actual = {path.relative_to(folder).as_posix() for path in folder.rglob("*") if path.is_file()}
    if actual != expected | {"manifest.json"}:
        raise ValueError("Unexpected release content, including developer diagnostics")
    for entry in manifest["files"]:
        data = (folder / entry["path"]).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError("Release content differs from its manifest: " + entry["path"])
    verify_documentation((folder / "README.md").read_text(encoding="utf-8-sig"),
                         manifest["source_commit"])
    config = (folder / "OutlastVR.ini").read_text(encoding="utf-8-sig")
    if "\nphysical_camcorder=false\n" not in "\n" + config.replace("\r\n", "\n"):
        raise ValueError("Release config does not disable the experimental camcorder")
    # Restricted DLL search proves the package does not rely on the developer's PATH.
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    kernel.FreeLibrary.restype = wintypes.BOOL
    for name in ("openxr_loader.dll", "OutlastVRSinglePass.dll"):
        library = ctypes.WinDLL(str(folder / name), winmode=0x100 | 0x800)
        export = "xrGetInstanceProcAddr" if name == "openxr_loader.dll" else "OutlastVR_Initialize"
        try:
            getattr(library, export)
        finally:
            if not kernel.FreeLibrary(library._handle):
                raise ctypes.WinError(ctypes.get_last_error())
    help_result = subprocess.run([str(folder / "OutlastVRLauncher.exe"), "--help"],
                                 capture_output=True, text=True, check=True)
    if "Outlast VR" not in help_result.stdout or "--verify" not in help_result.stdout:
        raise ValueError("Packaged launcher help is incomplete")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    args = parser.parse_args()
    result = verify(args.folder)
    print(f"PASS: Outlast VR {result['version']}; exact package and standalone DLL loads; no game launched")
