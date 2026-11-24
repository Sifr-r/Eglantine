# English plurals in Qt

Qt chooses a singular or plural translation for a string with a count:

```cpp
tr("Building: %n shader(s)", "", i)

// i = 1 displays "Building: 1 shader".
// i = 2 displays "Building: 2 shaders".
```

The frontend source language is English. Translators handle the other languages'
plural forms through [the Qt translation workflow](../languages/README.md). English
plural forms are maintained here because the translation service uses English as the source.

## Refresh the English forms

In a Qt-enabled build with Qt Linguist tools, set `ENABLE_QT_TRANSLATION=ON` and
`GENERATE_QT_TRANSLATION=ON` at CMake configure time. Then build the
`translation` target from the repository root:

```sh
cmake --build build --target translation
```

The target refreshes `dist/languages/en.ts` and creates the Git-ignored
`generated_en.ts` in this directory. Its location preserves relative source references.
Use the generated plural messages to update the tracked `en.ts` here in Qt Linguist or
an XML editor. The Qt build compiles that reviewed file into `en.qm`.
See the [Qt CMake rules](../../src/yuzu/CMakeLists.txt).
