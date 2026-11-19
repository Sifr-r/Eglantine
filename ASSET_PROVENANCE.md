# Asset provenance

## Controller font

The standard fallback font embeds Open Sans 1.10. The font’s name table credits digitized data copyright 2010–2011 Google Corporation and identifies the [Apache License 2.0](LICENSES/Apache-2.0.txt). The original yuzu import (`9dd19c9a99`) records its generator as `FearlessTobi/yuzu_system_archives` at `0a24b0c9f38d71fb2c4bba5645a39029e539a5ec`.

Eglantine’s font merge (`be2f49ed43`) added 24 controller glyphs. The Unicode maps, outlines and horizontal metrics show that the existing Open Sans characters survived the merge. All 24 added glyphs match the corresponding outlines in the bundled [YuzuOSSExtension font](src/core/file_sys/system_archive/data/font_nintendo_extended.cpp) after scaling and translation, within half a font unit of rounding. The extension source credits the yuzu Emulator Project under GPL-2.0-or-later. The current [standard font source](src/core/file_sys/system_archive/data/font_standard.cpp) contains both inputs. Distribute it with the project [GPL license](LICENSE.txt) and the Apache license above.

## Eglantine artwork

The macOS app bundle uses [`eglantine.icns`](dist/eglantine.icns). Commit `514cc97980` identifies [`Eglantine.png`](dist/Eglantine.png) as its source image. Linux AppImages use the same PNG, added in the rebrand (`16414d3019`). The original artwork attribution and redistribution grant still need confirmation from the rebrand contributor.

The old precompiled `dist/Assets.car` originated in the earlier Eden macOS icon work (`bf23921f07`). The Eglantine bundle now loads the `.icns` through `CFBundleIconFile`. The legacy catalog was removed from the source tree and archived locally in `ignore/provenance/Assets.car`.

## Firmware keyboard maps

The [`KeyCodeMap` tables](src/core/hle/service/set/key_code_map.h) are described in the source as extracted from the version 16.2.0 settings sysmodule. They supply the keyboard-layout response for `GetKeyCodeMap`, `GetKeyCodeMap2`, and `GetKeyCodeMapByPort`. The checked-in source and its introduction (`098788d96a`) provide no redistribution grant for the underlying tables.

This remains open for public distribution. Obtain documented permission for the data, or replace it with independently generated layouts and verify the response size, locale selection, modifier behavior, and fallback for missing layouts. The firmware version and table descriptions record the inputs that need review.
