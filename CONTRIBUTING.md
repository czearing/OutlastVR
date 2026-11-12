# Contributing

Use the [build guide](docs/building.md), keep changes focused, and add regression
tests for changed behavior. Pull requests must pass the same CMake/CTest and
packaging checks as releases.

Keep world rendering single-pass, retain game-version checks, and handle lost
focus or tracking safely. Experimental features should be disabled by default.

For each release, add `releases\<version>.json`. The release workflow must use
`tools\release_notes.py` and `releases\template.md`, never generated commit notes.
Keep notes to short player-visible changes and relevant known issues. No em
dashes, test counts, verification details, internal jargon or completion claims.

Do not commit game binaries, extracted shaders/models, saves, captures, tokens,
local paths or build products. Open an issue before large architectural changes.
