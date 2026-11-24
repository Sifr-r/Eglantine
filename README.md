<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

<h1 align="center">
  <br>
  <a href="https://git.eden-emu.dev/eden-emu/eden"><img src="https://github.com/user-attachments/assets/35031845-4bfa-4916-8bf5-bc3bce67d5e2" alt="Eglantine" width="200"></a>
  <br>
  <b>Eglantine</b>
  <br>
</h1>

<h4 align="center"><b>Eglantine</b> is a free and open-source emulator for Nintendo Switch and Switch 2. Camille LaVey started the project using work from Eden, Yuzu, and Sudachi.
It uses C++ and Rust, with desktop builds for Windows, Linux, and macOS.
</h4>

<p align="center">
    <a href="https://discord.gg/XkS5NVPTUY">
        <img src="https://img.shields.io/discord/1367654015269339267?color=5865F2&label=Eglantine&logo=discord&logoColor=white"
            alt="Discord">
    </a>
  
</p>

<p align="center">
  <a href="#compatibility">Compatibility</a> |
  <a href="#development">Development</a> |
  <a href="#documentation">Documentation</a> |
  <a href="#building">Building</a> |
  <a href="#download">Download</a> |
  <a href="#support">Support</a> |
  <a href="#license">License</a>
</p>

## Compatibility

Game compatibility and performance depend on the title version, build, driver, and settings. For a reproducible report, include those details and list the steps from launch to the issue. The [contribution guide](CONTRIBUTING.md#what-to-include-in-a-report) describes what to share.

## Development

For development discussions, please join us on [Discord](https://discord.gg/XkS5NVPTUY)

See the [contribution guide](CONTRIBUTING.md) for how to get involved. You can also contact any of the developers on Discord or Stoat to learn more about the current state of the emulator.

## Documentation

See the [User Handbook](docs/content/user/README.md) for setup, settings and troubleshooting.

| If you are working on | Start here |
| --- | --- |
| A code or documentation change | [Repository instructions](AGENTS.md) and [contribution guide](CONTRIBUTING.md) |
| A subsystem or test | [Architecture task map](ARCHITECTURE.md#task-map) |
| A Switch 2 runtime or shader investigation | [Debugging and lab tools](tools/README.md#switch-2-and-emulator-debugging) |
| The reasoning behind an earlier design | [Historical migration guide](POLYGLOT_IMPLEMENTATION_PLAN.md) and [Switch 2 development notes](NX2_NOTES.md) |
| Release history or asset attribution | [Changelog](CHANGELOG.md) and [asset provenance](ASSET_PROVENANCE.md) |

## Building

See the [General Build Guide](docs/content/Build.md).

From a configured desktop build directory, the main CMake targets are `yuzu` (Qt), `yuzu-cmd` (SDL), and `tests` (Catch2). See the [architecture task map](ARCHITECTURE.md#task-map) for targeted checks. The Android frontend lives in `src/android`. Check its [Gradle configuration](src/android/app/build.gradle.kts) for the SDK, NDK, and CMake pins.

## Download

You can download the latest releases from [here](https://github.com/Djihads80/Eglantine/releases).

## Support

We currently do not take in any donations for the time being

If you would prefer to support us in a different way, please join our [Discord](https://discord.gg/XkS5NVPTUY)

## Star History

<a href="https://www.star-history.com/?repos=eglantine-community%2Feglantine&type=date&legend=bottom-right">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=eglantine-community/eglantine&type=date&theme=dark&legend=bottom-right" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=eglantine-community/eglantine&type=date&legend=bottom-right" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=eglantine-community/eglantine&type=date&legend=bottom-right" />
 </picture>
</a>

## License

Eglantine is licensed under the GPLv3 (or any later version). Refer to [LICENSE.txt](LICENSE.txt).
