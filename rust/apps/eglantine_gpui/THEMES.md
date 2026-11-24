# Eglantine GPUI themes

Themes change how the GPUI frontend looks and how it arranges built-in controls. Choose **Settings → Appearance** to preview a theme, apply it, or export it to share. **Restore default theme** reverts to Graphite and Amber. Start `eglantine-gpui --safe-theme` to use the default for one session.

## Edit a theme file

A `.eglantine-theme` file is a ZIP archive with `theme.json` at its root. Export the built-in theme for a complete example, or start with a small manifest. Omitted style and layout fields inherit the built-in values.

```json
{
  "version": 1,
  "id": "night-garden",
  "name": "Night Garden",
  "style": {
    "background": "#141c18",
    "panel": "#222d26",
    "focus_start": "#d1ed86",
    "focus_end": "#a5cd70",
    "corner_radius": 20,
    "cover_size": 220
  },
  "layout": {
    "home_mode": "grid",
    "home_navigation": "left"
  }
}
```

- Colors use `#RRGGBB`. The style also supports `foreground`, `muted`, `focus_text`, `spacing`, `text_scale` as a percentage, `font_family`, and `motion_ms`.
- `home_mode` takes `shelf` or `grid`. `home_navigation` takes `below`, `left`, or `right`. `home_shortcuts` orders `library`, `controllers`, `saves_mods`, `content`, `settings`, and `exit`. Include `settings` once so it stays directly accessible.
- `settings_side` and `pause_side` take `left` or `right`. `library_mode` takes `list` or `grid`. `dialog_width` specifies the preferred width in logical pixels.
- For a custom image or font, declare its ID and type in `assets`. For example, `"assets": {"fern": "png"}` declares `assets/fern.png`. Reference `fern` in `style.background_asset`, or assign it to a shortcut in `style.icons`. Supported extensions are `.png`, `.jpg` (`jpeg`), `.webp`, `.ttf`, and `.otf`. `style.font_asset` selects a bundled font and `font_family` names its family.

Use lowercase ASCII letters, digits, hyphens, and underscores for IDs, starting with a letter or digit. Include up to 24 assets. Each asset may use up to 8 MiB and all assets together may use up to 32 MiB. Image dimensions may be up to 4096 pixels per side. ZIP entries should contain the declared files using their relative paths. Importing previews the theme so you can check readability and navigation before applying it.
