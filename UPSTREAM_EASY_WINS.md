# First upstream fixes

| Issue | File responsibility | Verification |
| --- | --- | --- |
| #669 | `src/qt_common/config/qt_config.cpp` persists Qt hotkeys. Match stored entries to their defaults by stable identity. | Show that clearing the Restart Emulation controller binding survives a save and reload, while default and custom bindings still resolve correctly. |
| #381 | Android `model/Game.kt` defines game-file discovery extensions. `src/core/loader/nup.cpp` validates the selected executable filesystem before exposing the program ID to Android. | Check both Android extension gates and exercise the loader with valid NUPs and incomplete executable filesystems. |

Keep fixes in the existing domains. Add a focused regression test only where the existing tests cannot catch the reported behavior. Update the architecture ledger with any new source or test file and its responsibility.
