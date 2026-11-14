import configparser
import importlib.util
from pathlib import Path
import re
import unittest
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("verify_package", ROOT / "tools" / "verify_package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class DocumentationTests(unittest.TestCase):
    def test_settings_reference_covers_shipped_keys_and_defaults(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "config" / "OutlastVR.ini", encoding="utf-8")
        shipped = {f"{section}.{key}": value
                   for section in config.sections() for key, value in config[section].items()}
        text = (ROOT / "docs" / "settings.md").read_text(encoding="utf-8")
        rows = re.findall(r"^\| `([^`]+)` \| `([^`]+)` \|", text, re.MULTILINE)
        self.assertEqual(len(rows), len(dict(rows)), "Duplicate setting in reference")
        documented = {key: "" if value == "empty" else value for key, value in rows}
        self.assertEqual({key: documented[key] for key in shipped}, shipped)
        header = (ROOT / "src" / "outlast_config.h").read_text(encoding="utf-8")
        self.assertEqual(set(documented), set(re.findall(r'OutlastSetting\{"([^"]+)"', header)))
        self.assertTrue(all(key.startswith("vr.") and key != "vr.openxr_loader" for key in shipped),
                        "Advanced overrides or experiments leaked into the player-facing sample")

    def test_compatibility_identifies_the_accepted_executable(self):
        header = (ROOT / "src" / "outlast_native_image.h").read_text(encoding="utf-8")
        digest = re.search(r'outlast_retail_sha256\s*=\s*"([0-9a-f]{64})"', header).group(1)
        text = (ROOT / "docs" / "compatibility.md").read_text(encoding="utf-8")
        self.assertEqual(re.findall(r"\b[0-9a-f]{64}\b", text), [digest])

    def test_dependency_notices_follow_cmake_pins(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        pins = re.findall(r"GIT_TAG ([0-9a-f]{40})", cmake)
        notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        self.assertTrue(pins, "No dependency revisions found")
        self.assertEqual(set(re.findall(r"`([0-9a-f]{40})`", notices)), set(pins))

    def test_inline_repository_links_resolve(self):
        documents = [*ROOT.glob("*.md"), *(ROOT / "docs").rglob("*.md"),
                     *(ROOT / "releases").glob("*.md")]
        for document in documents:
            for destination in re.findall(r"\]\(([^)]+)\)", document.read_text(encoding="utf-8")):
                with self.subTest(document=document.relative_to(ROOT), destination=destination):
                    link = urlsplit(destination)
                    if link.netloc:
                        parts = link.path.strip("/").split("/")
                        if (link.netloc != "github.com" or
                                parts[:3] != ["czearing", "OutlastVR", "blob"]):
                            continue
                        self.assertEqual(parts[3], "master", "Source guides must link to the working branch")
                        target = ROOT.joinpath(*unquote("/".join(parts[4:])).split("/"))
                    else:
                        target = (document.parent.joinpath(*unquote(link.path).split("/"))
                                  if link.path else document)
                    self.assertTrue(target.is_file(), f"Missing documentation target: {target}")
                    if link.fragment and target.suffix == ".md":
                        headings = re.findall(r"^#{1,6}\s+(.+)$", target.read_text(encoding="utf-8"), re.MULTILINE)
                        anchors = {re.sub(r"[^\w -]", "", heading.lower()).replace(" ", "-")
                                   for heading in headings}
                        self.assertIn(unquote(link.fragment), anchors, "Missing heading anchor")

    def test_packaged_guides_are_bound_to_source_commit(self):
        commit = "a" * 40
        prefix = "https://github.com/czearing/OutlastVR/"
        readme = f"[Controls]({prefix}blob/{commit}/docs/controls.md)\n"
        package.verify_documentation(readme, commit)
        for revision in ("master", "main", "b" * 40):
            with self.subTest(revision=revision), self.assertRaisesRegex(ValueError, "source commit"):
                package.verify_documentation(
                    readme + f"[Settings]({prefix}blob/{revision}/docs/settings.md)", commit)
        with self.assertRaisesRegex(ValueError, "source commit"):
            package.verify_documentation("No guide links", commit)
        with self.assertRaisesRegex(ValueError, "full source commit"):
            package.verify_documentation(readme, "master")

    def test_package_accepts_omitted_safe_experimental_defaults(self):
        package.verify_safe_defaults("[vr]\nbloom=true\n")
        package.verify_safe_defaults("[experimental]\nphysical_camcorder=false\n")
        for value in ("true", "invalid"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "camcorder"):
                package.verify_safe_defaults(f"[experimental]\nphysical_camcorder={value}\n")


if __name__ == "__main__":
    unittest.main()
