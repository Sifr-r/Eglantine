# Arch Linux development package

This directory contains a draft `eglantine-git` PKGBUILD for the open Linux
packaging request in [issue #167](https://github.com/Djihads80/Eglantine/issues/167).
It builds the `nx2` branch from source against Arch's system Qt, SDL3 and FFmpeg.
That avoids shipping Ubuntu-linked binaries to a rolling-release distribution
and gives the Qt frontend access to the host's platform plugins, including
Wayland when `qt6-wayland` is installed.

[`PKGBUILD`](./PKGBUILD) owns the package dependencies, source branch and CMake
options. It stages the normal CMake install into the package directory.
This draft is for local review. The project release workflow is maintained
separately. Package updates use pacman or AUR helpers, so the in-app updater is disabled.

## Local build

On an up-to-date Arch Linux system with the usual package build tools, run these
commands from the recipe directory. `--syncdeps` installs missing dependencies
and `--cleanbuild` removes the previous extracted build tree first.

```sh
cd dist/packaging/arch/eglantine-git
makepkg --syncdeps --cleanbuild
makepkg --packagelist
```

## Package review

Use Arch's `extra-x86_64-build` for a clean-chroot check. Run `namcap` on
`PKGBUILD` and the package archive reported by `makepkg --packagelist`. Record
the source revision with the result because `nx2` is a moving branch. Before any
AUR publication, regenerate and review `.SRCINFO` after updating `pkgver`.
