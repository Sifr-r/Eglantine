# Qt translations

This directory holds Qt Linguist `.ts` translation sources for the desktop frontend.
[`.tx/config`](../../.tx/config) configures `en.ts` as the English source for the Qt
resource on Transifex. Submit translation contributions to the
[Eden project on Transifex](https://app.transifex.com/edenemu/eden-emulator).

Use the [translation scripts guide](../../tools/translations/README.md) to refresh the
English source or pull a maintainer snapshot. With an authenticated Transifex CLI,
`tx pull -t -a` from the repository root pulls translations for the resources in
`.tx/config`, including Android strings.

The Qt build compiles localized `.ts` sources to `.qm` files. The English runtime
plural rules use a separate [checked-in plural catalogue](../english_plurals/README.md).

## Contribution rules

- Translate accurately and keep the content appropriate.
- Developers can clarify the source text. Review of the target language depends on the translation community.
- Bad-faith translations or attempts to insert illicit content may result in immediate removal of access.

New language requests are currently closed.
