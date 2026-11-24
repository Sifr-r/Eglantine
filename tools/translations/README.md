# Translation scripts

Run commands from the repository root. [`.tx/config`](../../.tx/config) maps the Qt and
Android source resources to their respective translations.

## Refresh the Qt source

Install Qt Linguist tools and put Qt 6's `lupdate` on `PATH`. The script scans the
Qt frontend and shared Qt code, then rewrites `dist/languages/en.ts`.

```sh
lupdate -version
tools/translations/qt-source.sh
git diff -- dist/languages/en.ts
```

An authorized maintainer uploads the reviewed source to Transifex with `tx push -s`.
The [English plurals guide](../../dist/english_plurals/README.md) covers the English
runtime translation separately.

## Refresh localized resources

- [`dist/languages`](../../dist/languages/README.md) documents the translation workflow for contributors.
- [`update-translations.sh`](./update-translations.sh) uses an authenticated `tx` or
  `tx-cli` to force-pull translations. It stages the Qt `.ts` and Android
  `strings.xml` files and creates a signed commit. Use it for an intentional
  maintainer update.
