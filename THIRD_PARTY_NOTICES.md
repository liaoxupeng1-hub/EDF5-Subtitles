# Sources and third-party notices

## Original subtitle project

- Project: https://github.com/vixen256/subtitles
- Original license: Unlicense; retained in the root LICENSE.
- This project rebuilds the EDF5 subtitle runtime and retains the original project's attribution. Game resources are not included.

## Dear ImGui

- Upstream: https://github.com/ocornut/imgui
- Included source reports version 1.89.2 WIP (IMGUI_VERSION_NUM 18914).
- Copyright (c) 2014-2022 Omar Cornut.
- License: MIT, retained in vendor/imgui/LICENSE.txt.
- Bundled imstb headers retain their own upstream notices and license choices.

## Microsoft Detours

- Upstream: https://github.com/microsoft/Detours
- Included source reports DETOURS_VERSION 0x4c0c1 (4.0.1).
- Copyright (c) Microsoft Corporation. All rights reserved.
- License: MIT, retained in vendor/Detours/LICENSE.md; credits retained in vendor/Detours/CREDITS.md.

Only the source files required by this build and their notices are vendored. Vendor files are copied without changes from the local dependency snapshot used for the accepted build; no newer upstream revision is implied.

Game names and game data belong to their respective owners. This repository does not include game executables, game DLLs, subtitle tables, audio, fonts or loader binaries, and its code license does not grant rights to those resources.
