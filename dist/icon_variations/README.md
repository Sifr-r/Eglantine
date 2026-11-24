# Icon variations

The source icons in this directory are licensed under GPLv3. The generator uses
[`base.svg`](./base.svg) and [`base_bgcolor`](./base_bgcolor) by default.
A seasonal variant needs both `<variant>.svg` and `<variant>_bgcolor`. The
`original.svg` and `original_small.svg` files retain the original artwork variants.

## Regenerate the icons

From the repository root, use a POSIX shell with ImageMagick (`magick`), OptiPNG
(`optipng`), and Python 3. Install `png2icns` to generate `dist/eden.icns`.

```sh
tools/update-icons.sh
# To generate a seasonal variant:
VARIATION=aprilfools2026 tools/update-icons.sh
```

The [generator](../../tools/update-icons.sh) writes the desktop SVG, Windows ICO and BMP,
Qt and Android icons, Android launcher background color and `dist/eden.icns`. The current
macOS package reads `dist/eglantine.icns`, and the Linux AppImage uses `dist/Eglantine.png`.
Review the packaging references when changing release artwork. See the
[Qt build rules](../../src/yuzu/CMakeLists.txt) and [Linux build workflow](../../.github/workflows/build-linux.yml).

The generator also synchronizes the compiled default avatar with `dist/eden_profile.jpg`.
Check that the source JPEG and the compiled bytes agree:

```sh
python3 -I tools/update_profile_avatar.py --check
```

Keep the source icons simple and small. `svgo` and `optipng` can reduce asset size.
