# Device behaviour — what the console tells a running game

Scope: the *device-side* state a Switch title can observe or change — operation mode,
applet lifecycle, save mounting, user profiles, language, clock, catalogue, network,
peripherals. Graphics and shaders live in [shader_recompiler](shader_recompiler.md);
the machine, saves and HLE contracts live in [core](core.md); controllers in
[input_common](input_common.md).

This card records where the behavioural spec lives, and what Eglantine currently
reproduces of it.

## The external reference: recomp.gg

[recomp.gg](https://recomp.gg) is a read-only catalogue of games whose code came back —
decompiled, recompiled, reimplemented or source-ported — with the projects, releases and
people behind each. It is an *aggregator*, not a reference implementation. Its data model
(`Game`, `Project`, `Way`, `Release`, `News`, `Content`) has no device-behaviour fields
anywhere in the schema: nothing describes operation mode, applets, service coverage or
save semantics.

What it holds instead is a pointer graph, and the pointers resolve to the most valuable
behavioural artefacts in the ecosystem. Two things follow:

- The Switch category is a **popularity-ranked list of the commercial titles that decide
  what "device behaviour" has to mean in practice**. Emulation effort aimed at these
  titles is aimed at what users actually run.
- Most of the underlying projects are **recompilations**: they translate the original
  Switch AArch64 code to native code and re-implement the Switch OS services it calls.
  That runtime is an *independent implementation of the same device contracts we must
  match*, written by people with the same reverse-engineering problem. It is the closest
  thing to an executable behavioural spec that is publicly available.

Provenance, from recomp.gg's [About page](https://recomp.gg/about) and each record's
`sources`: SubmitGame's `awesome-decomp-games` list (CC0-1.0), the GitHub API, decomp.dev,
IGDB and Wikipedia. It hosts no game data and links to no game files.

### The Switch slice (read 2026-11-24)

| Measure | Value |
| --- | --- |
| Titles | 45 |
| Project records (one per title/project pair) | 78 |
| Recompilations — original Switch code run natively | 55, across 33 titles |
| Source ports | 9 |
| Decompilations | 7 |
| Reimplementations | 6 |
| Disassembly | 1 |

The API is the cheap way to read it, and it needs no key:
`GET /api/v1/games?platform=switch&limit=100` returns summaries; `GET /api/v1/games/{slug}`
returns the full record; `GET /api/v1/projects/{slug}` returns a project with its
releases, tags and sources. The schema is documented at
`https://recomp.gg/api/v1/openapi.json`. Read `sources` before reusing a fact, and keep
the `id` rather than the slug — slugs move and answer `308`.

Most-starred projects, in order: `totk-pc-recomp` (8.1K), `D2R Mobile` (5.4K),
`SavageWindRecomp` (5.1K), `LASRecomp` (2.6K), `zeldaret/botw` (2.5K),
`BOTWRecompAndroid` (1.1K), `MinecraftLCE` (629), `ssbu-recomp` (624).

### Three artefact kinds, three different uses

**Recompilation runtimes are the behavioural spec.** A recompiled game calls the same
`nn::` library entry points the original did, so the runtime must answer them. The
`runtime/` directory of such a project is a second implementation of the Switch OS.
`ssbu-recomp` states the scale plainly: `runtime/nn*.c` is "about 1,100 system-library
entry points (files, time, audio, input, movies, fonts, applets, accounts, network)"
under the heading "what the console provided, re-implemented". `ACNHRecomp` says the
same thing in prose: "the system services the game calls are reimplemented". This is the
artefact to diff against `src/core/hle/service/`.

**Decompilations expose the call sites, but only partially.** `zeldaret/botw` is a
source-level reimplementation with ~8,150 files under `src/Game/`, and it does contain
device-touching actions such as `src/Game/Actor/Action/actionDownloadAlbum.cpp`. Reading
that file, however, shows the decompilation's device-facing code is still stubbed out at
this path — so a decompilation tells you *that* a game calls a service, not reliably *how*
the real console answers. Treat it as a hint list, not a contract.

**Source ports and reimplementations show behaviour that was re-derived from scratch**,
which is useful where the original service semantics were unknown: how a title behaves
without online services, how it stores saves, which applet it opens.

## Parity table: recomp runtime modules vs Eglantine HLE

`ACNHRecomp` ships one module per service under `runtime/src/hle_*` (23 service modules
plus two headers); `ssbu-recomp` ships the same surface split as `runtime/nn*.c` (25 C
modules plus three generated `.inc` tables and `nnpad.h`). Both line up one-to-one with
the Switch service names, which makes them a ready-made parity table against
`src/core/hle/service/`. Eglantine paths in the third column are relative to
`src/core/hle/service/` unless they start with `src/`.

| Recomp module | Switch service | Eglantine | State in Eglantine |
| --- | --- | --- | --- |
| `nnapplet.c` | `am::applet` | `am/` | Present; HOME-menu applet calls unimplemented (below) |
| `nnalbum.c`, `hle_album.cpp` | `nn::album` | — | **No service.** Only `nn::album::*` type comments in `caps/caps_types.h` |
| `nnpl.c`, `hle_pl.cpp` | `nn::pl` (Permission Library) | — | **No service.** Only a comment at `ns/platform_service_manager.cpp:293` |
| `nnfontll.c` | `nn::font` | — | **No service** (font data comes from `ns`, not a font service) |
| `nnmovie.c`, `hle_movie.cpp` | `nn::movie` | — | **No service** (`hle/service/nx2_movie_pacing.h` is NX2 video pacing, unrelated) |
| `nnmisc.c` | `nn::misc` | — | **No service** |
| `hle_jpeg.cpp` | `nn::jpeg` | — | **No service** |
| `hle_vsock.cpp` | `nn::vsock` | — | **No service** (host I/O is handled inside `sockets/`) |
| `nnaudio.c`, `hle_audio.cpp` | `nn::audio` | `audio/` | Present, 11 stubbed paths |
| `nnfs.c`, `nnfsdir.c`, `hle_fs.cpp` | `nn::fs` | `filesystem/` | Present, 21 stubbed paths |
| `nnhid.c`, `nnpad_*.c`, `hle_hid.cpp` | `nn::hid` | `hid/` | Present, 89 stubbed paths — the largest stub cluster |
| `nnldn.c`, `hle_ldn.cpp` | `nn::ldn` | `ldn/` | Present, 13 stubbed paths |
| `nniostream.c` | `nn::stream` | `filesystem/` | Covered by the FS layer |
| `nnlibc2.c`, `nncxxlib.c` | `nn::libc2`, C++ runtime | `os/` | Partial |
| `nnmii.c` | `nn::mii` | `mii/` | Present, 12 unwired commands |
| `nnnet.c`, `hle_net.cpp`, `hle_hostnet.cpp` | `nn::net` | `sockets/`, `ssl/` | Present, 31 stubbed paths in `ssl/` |
| `nnro.c`, `hle_ro.cpp` | `nn::ro` | `ro/` | Present |
| `nnsdk.c`, `nnsdk2.c` | `nn::sdk` | `am/` | Present |
| `nntime.c`, `hle_time.cpp` | `nn::time` | `glue/time/` + kernel ticks | Present via `glue/time/standard_steady_clock_resource.cpp`; `custom_rtc_*` overrides the wall clock |
| `nnunwind.c` | unwinder | `core/` | Present |
| `hle_bcat.cpp` | `nn::bcat` | `bcat/` | Present, 10 stubbed paths |
| `hle_friends.cpp` | `nn::friends` | `friend/` | Present, 20 stubbed paths |
| `hle_ngc.cpp` | `ngc` (NX2 catalogue) | `ngc/` | Present, 6 stubbed paths; update check unwired |
| `hle_npns.cpp` | `nn::npns` | `npns/` | Present, 6 stubbed paths |
| `hle_olsc.cpp` | `nn::olsc` | `olsc/` | Present, 17 stubbed paths |
| `hle_ssl.cpp` | `ssl` | `ssl/` | Present, 31 stubbed paths |
| `hle_vi.cpp` | `vi` | `vi/` | Present, 13 stubbed paths |
| `hle_web.cpp` | web applet | `am/frontend/applet_web_browser.cpp` | Present |
| `hle_nfp.cpp`, `hle_hid_system_nfc.cpp` | `nn::nfp`, `nn::nfc` | `nfp/`, `nfc/` | Present, 2 stubbed paths (`nfp` `WriteNtf`) |
| `hle_hostnet.cpp` | `nn::hostnet` | `sockets/` | Present |

Read this table as a coverage statement, not a defect list. A recomp runtime implements the
services its *one* game happens to touch; Eglantine implements the services a *whole
catalogue* of games might touch, so some rows are deliberately thinner. The seven rows
where Eglantine has no counterpart at all — `nn::album`, `nn::pl`, `nn::font`,
`nn::movie`, `nn::jpeg`, `nn::misc`, `nn::vsock` — are the only genuine absences.

## Device-behaviour surfaces, ordered by what the catalogue demands

Grouping the 45 titles by the device behaviour they stress gives the priority order.

**Operation mode and display state** is the most broadly demanded surface, because every
title branches on it. The recomp runtimes show the two ways it is modelled: ACNHRecomp
ships `NX_HANDHELD=1` for 720p handheld against 1080p TV mode, and SavageWindRecomp's
`runtime/handheld.h` detects the host machine (vendor/product DMI strings, Steam Deck
special case) to decide what to report to the game. Eglantine models docked/handheld
through `Settings::values.use_docked_mode` (`src/common/settings.h:837`, default
`Handheld`) and pushes it into the guest via `AppletManager::OperationModeChanged`
(`am/applet_manager.cpp:256`) — but several of the paths a game can query are unwired:
`omm/operation_mode_manager.cpp` leaves all 29 commands (0–28) as `nullptr`, including
`GetOperationMode`, `GetOperationModeChangeEvent`, `GetDefaultDisplayResolution` and
`GetOperationModeSystemInfo`; `glue/bgtc.cpp:49` `GetOperationMode` (command 101) is
`nullptr`; and `prepo` command 20200 and `ldn` command 402 `SetOperationMode` are
`nullptr`.

**Catalog, updates and DLC.** `totk-pc-recomp` supports "Game updates 1.1.0, 1.2.1 and
1.4.3; 1.2.1 and 1.4.3 saves carry over between them", and `ssbu-recomp`'s installer
applies the update and the DLC. On Eglantine's side the Switch 2 catalogue service
`ngc` leaves `ConfigureAutoUpdateSetting` (100) and `RequestResourceUpdateCheck` (101)
unwired, `bcat` is 10 stubbed paths, and NUP content is recognised but **not mounted**
(`src/core/loader/nup.h:25`).

**Save mounting and versioning.** `ssbu-recomp` lays its save out as a `save_data`
folder holding `system_data.bin`, `mii`, `spirits` and `stage`, and imports from JKSV,
Checkpoint, yuzu/Eden and Ryujinx. Eglantine's `SaveDataFactory` / `SaveDataController` /
`FspSrv` already own the disk layout (`save/<id>/<uid>`, `save/0/<uid>/<tid>`,
`save/cache/<tid>[_<idx>]`) with idempotent deletes — this is the strongest device
behaviour the emulator already has.

**Online services.** The catalogue separates cleanly: `MK8-online` (Mario Kart 8 Deluxe)
plays online races over Nextendo, `ssbu-recomp` implements Local Wireless over LAN, a room
code or a lobby server and marks it *Experimental*, `ACNHRecomp` offers Nextendo Dodo
Codes and Search for a friend while stating plainly that "Nintendo's online services are
not available", and `swsh-recomp` states that "Online features (raids with other players,
trades, Y-Comm) are not supported". In every case the online path is a *third-party
service*, not Nintendo's — so the honest target is `nn::ldn` local wireless plus
`nn::nifm` connectivity, not Nintendo's servers. Eglantine has `ldn/` (13 stubbed paths),
`nifm/` (20 stubbed paths), `sockets/` and `ssl/` (31 stubbed paths).

**Applets and the HOME menu.** A title needs applets for settings, the software
keyboard, the album, the web browser and the controller pairing. Eglantine's applet core
is real (`am/applet_manager.cpp`, `am/window_system.cpp` with three foreground roots:
QLaunch, the running application and the overlay), but the HOME applet itself is not
finished — `am/service/home_menu_functions.cpp:32-33` leaves `LaunchSystemApplet` (50)
and `LaunchStarter` (51) unwired. The default frontend applets are
not interactive: the software keyboard submits the literal string `"Eden"`
(`src/core/frontend/applets/software_keyboard.cpp:77,120`) and the amiibo cabinet fails
immediately (`src/core/frontend/applets/cabinet.cpp:21-22`). Only Android installs real
applet implementations; `eden-cli` always launches `AppletId::Application`
(`src/yuzu_cmd/yuzu.cpp:849-851`), so the SDL host has no HOME menu, no keyboard UI and
no applet UI at all, and the GPUI shell has no applet code.

**Amiibo and NFC.** SavageWindRecomp ships `runtime/amiibo.c` and documents a "virtual
amiibo reader"; ACNHRecomp has `runtime/src/amiibo_decode.cpp` and
`acnh_amiibo_hook.cpp`. Eglantine's `nfp/` is a shim over the Switch 2 NFC backend with
`AmiiboCrypto` handling plain and encrypted NTAG215, plus `random_amiibo_id` to bypass
load limits; only `nfp` `WriteNtf` is stubbed (`nfp/nfp_interface.cpp:447`).

**Language, region, clock, model.** The runtimes treat these as first-class device state:
`ACNHRecomp` reports "12 languages detected from the system", exposes `NX_LANGUAGE`, and
states that the "Island clock follows your computer's date, time and time zone, as on the
console"; SavageWindRecomp has `runtime/language.c`, `runtime/dlc_check.c` and
`runtime/game_versions.h`. Eglantine has `language_index`, `region_index`,
`time_zone_index` and `custom_rtc_*` settings, but `set/system_settings_server.cpp:1222`
returns a hard-coded `GetProductModel` of `1` and `:1432` a hard-coded `GetPlatformRegion`
of `Global`.

**Launch flags.** `SavageWindRecomp/runtime/launch_flags.h` is worth reading as a
statement of what the Switch hands a game at launch — operation mode, profile, locale —
because it models the same per-launch state vector the guest reads before its first
frame.

## Gaps, in the order they are worth closing

1. **Operation-mode query paths.** `omm` (all 29 commands), `glue/bgtc`, `prepo` and
   `ldn` leave the operation-mode reads and writes unwired while `use_docked_mode`
   already exists as a setting. Titles that branch on docked state are the whole
   catalogue, so a missing query is a missing branch in every game.
2. **A HOME menu that can actually be entered.** `LaunchSystemApplet` / `LaunchStarter`
   are unwired and only Android supplies applets. Until the SDL and GPUI hosts can
   present QLaunch, every applet path in the catalogue is unreachable in the two
   frontends most people use.
3. **Non-interactive default applets.** The software keyboard returning `"Eden"` and the
   amiibo cabinet failing immediately mean text-entry and amiibo titles break in a way
   that looks like a game bug.
4. **`nn::album` and `nn::pl` do not exist.** Both are implemented by the ACNH runtime;
   `nn::album` is reachable from ordinary gameplay (the BOTW decompilation has an
   `actionDownloadAlbum` action), and `nn::pl` supplies built-in font data that
   `ns/platform_service_manager.cpp:293` already references in a comment.
5. **Catalogue and update checks.** `ngc` update commands and the unwired NUP mounting
   mean update-carrying titles (`totk-pc-recomp` supports three update levels) and DLC
   cannot be exercised.
6. **Profile selection is global.** `current_user` is a single global setting with no
   per-game override, and `am/applet_manager.cpp:307-308` still carries the TODO for
   reading a preselected user from the NACP.

Lower priority, but cheap: `set` `GetProductModel` / `GetPlatformRegion` are hard-coded
constants a game can compare against, and `ns` carries 47 stubbed paths.

## Mining it further

The site is a directory, not a source of truth; the repos it points at are the source of
truth. The efficient loop is:

1. `GET /api/v1/games?platform=switch&limit=100` → slugs; `GET /api/v1/games/{slug}` →
   the project list with repos.
2. For a project, list `runtime/` (the GitHub trees API answers
   `GET /repos/{owner}/{repo}/git/trees/HEAD?recursive=1` in one request) and diff the
   module list against `src/core/hle/service/`.
3. Read the project's `docs/` — ACNHRecomp's `docs/STATUS.md` is a service-by-service
   feature matrix, and `swsh-recomp` keeps runtime audits under `docs/engineering/`.
4. Release notes and READMEs are where behavioural surprises surface in plain language
   ("mechanics that break above 30 fps", "the first save only happens at the end of the
   first in-game day").

Per AGENTS.md, console keys, firmware, game files and raw captures stay in local-only
storage; the catalogue JSON, project trees and READMEs pulled for this card are under
`ignore/recomp-gg/` and are not tracked.
