"""Render short, player-facing release notes from the required template."""
import argparse
import json
from pathlib import Path
import re
from string import Template


ROOT = Path(__file__).resolve().parents[1]
INTERNAL = re.compile(
    r"\b(?:tests?|testing|verified|verification|regressions?|fixtures?|CI|DLL|"
    r"SHA256|commits?|pipelines?|hooks?|oracles?|acceptance|validated|validation|"
    r"evidence|harness|production.ready|bulletproof|perfect|rock.solid)\b|100%", re.IGNORECASE)


def release_version(tag):
    match = re.fullmatch(r"v?(\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?)", tag)
    if not match:
        raise ValueError("Release tag must contain a version, not a path")
    return match[1]


def render_release_notes(tag, data, template):
    version = release_version(tag)
    if not isinstance(data, dict) or set(data) != {"version", "changes", "known_issues"}:
        raise ValueError("Release notes require version, changes and known_issues")
    if data["version"] != version:
        raise ValueError("Release notes do not match the release tag")
    seen = set()
    for section in ("changes", "known_issues"):
        entries = data[section]
        if not isinstance(entries, list) or len(entries) > (8 if section == "changes" else 3):
            raise ValueError("Release notes must use short lists")
        if section == "changes" and not entries:
            raise ValueError("Release notes must describe a player-visible change")
        for text in entries:
            if not isinstance(text, str) or not text or text != text.strip():
                raise ValueError("Release note entries must be nonempty plain text")
            if len(text) > 160 or len(text.split()) > 25:
                raise ValueError("Release note entries must be at most 25 words and 160 characters")
            if any(character in text for character in "\r\n\t`#*<>\u2013\u2014"):
                raise ValueError("Release notes cannot contain dash typography or embedded formatting")
            if INTERNAL.search(text):
                raise ValueError("Release notes must describe player benefits, not development checks")
            if section == "changes" and not re.match(
                    r"^(?:Added|Fixed|Improved|Reduced|Updated|Removed)\b", text):
                raise ValueError("Each change must start with a plain action verb")
            if text.casefold() in seen:
                raise ValueError("Release notes cannot repeat entries")
            seen.add(text.casefold())
    changes = "\n".join(f"- {text}" for text in data["changes"])
    issues = ""
    if data["known_issues"]:
        issues = "\n\n## Known issues\n\n" + "\n".join(f"- {text}" for text in data["known_issues"])
    rendered = Template(template).substitute(changes=changes, known_issues=issues).rstrip() + "\n"
    if len(rendered.split()) > 180:
        raise ValueError("Release notes must stay under 180 words")
    return rendered


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        version = release_version(args.tag)
        directory = ROOT / "releases"
        data = json.loads((directory / f"{version}.json").read_text(encoding="utf-8"))
        template = (directory / "template.md").read_text(encoding="utf-8")
        rendered = render_release_notes(args.tag, data, template)
        with args.output.open("x", encoding="utf-8", newline="\n") as output:
            output.write(rendered)
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
