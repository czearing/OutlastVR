# Contributing

Use the [build guide](docs/building.md), keep changes focused, and add regression
tests for changed behavior. Pull requests must pass the same CMake/CTest and
packaging checks as releases.

Keep world rendering single-pass, retain game-version checks, and handle lost
focus or tracking safely. New experimental features must be opt-in; preserve
existing users' settings.

## Documentation

Update documentation in the same change as the behavior it describes.

| Page | Owns |
| --- | --- |
| `README.md` | Installation, updating and guide navigation |
| `docs\compatibility.md` | Required software, accepted executable and input profiles |
| `docs\controls.md` | Bindings and interaction behavior |
| `docs\settings.md` | Every shipped INI key, default and accepted value |
| `docs\troubleshooting.md` | Actionable checks and bug-report instructions |
| `docs\building.md`, `docs\testing.md` | Contributor commands and release checks |

Keep detailed references in their owning page and link to them elsewhere.
Compatibility lists requirements, not bugs, completed fixes or testing status.
Version-specific known issues belong in release notes; ongoing reports belong
in GitHub Issues. Do not rewrite historical release notes to describe new code.

Use short instructions, exact setting/input names and task-oriented headings.
Keep implementation details and test results out of player guides. Avoid hype,
completion claims, em/en dashes and repeated caveats. Distinguish included
bindings from hardware combinations actually tested.

`config\OutlastVR.ini` is the reference for shipped defaults. Documentation tests
check settings, build/dependency identities and repository links. Packaged
README links must point to the package's source commit, not mutable `master`
documentation.

## Releases

For each release, add `releases\<version>.json`. The release workflow must use
`tools\release_notes.py` and `releases\template.md`, never generated commit notes.
Keep notes to short player-visible changes and relevant known issues. No em
dashes, test counts, verification details, internal jargon or completion claims.

Do not commit game binaries, extracted shaders/models, saves, captures, tokens,
local paths or build products. Open an issue before large architectural changes.
