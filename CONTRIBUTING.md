# Contributing

Use the [build guide](docs/building.md), keep changes focused, and add regression
tests for changed behavior. Pull requests must pass the same CMake/CTest and
packaging checks as releases.

Preserve one instanced world pass; do not fix stereo by rendering the scene once
per eye. Keep unsupported-executable rejection and focus/tracking safety gates.
New unfinished features belong behind explicit, default-off configuration.

Do not commit game binaries, extracted shaders/models, saves, captures, tokens,
local paths, build products or historic candidate packages. Diagnostic captures
stay local; report reproducible results and limitations rather than claiming
all scenes/headsets passed. Open an issue before large architectural changes.
