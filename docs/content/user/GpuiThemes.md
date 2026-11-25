# Eglantine GPUI Themes

Eglantine's GPUI frontend uses a portable `.eglantine-theme` package format for both the built-in **Graphite and Amber** theme and community-created themes. Themes control colors, gradients, typography, custom icons, background artwork, spacing, corner radii, motion duration, and layout placement across Home, Library, Settings, the pause sheet, and dialogs.

## Editing and Previewing Themes

Open **Settings → Appearance** to customize or manage themes:

- **Preview screen**: Cycle the inline sample preview between **Home**, **Library**, **Pause sheet**, and **Dialog**.
- **Duplicate theme**: Clone the active or drafted theme into a new editable draft (`<Name> Copy`).
- **Style & layout controls**: Adjust hex colors (`#RRGGBB`), font family, text scale, spacing, corner radius, cover size, motion duration, Home mode (`Shelf` / `Grid`), shortcut placement (`Below` / `Left` / `Right`), shortcut order/visibility, Library mode (`List` / `Grid`), Settings/Pause drawer side (`Left` / `Right`), and dialog width.
- **Assets**: Pick local `.png`, `.jpg`/`.jpeg`, or `.webp` images for the background or individual Home shortcuts, or a `.ttf`/`.otf` font file.
- **Import theme / Export theme**: Package or load a `.eglantine-theme` ZIP archive. Imports enter preview mode first; the previously active theme remains untouched until you select **Apply theme**.
- **Discard changes**: Revert unapplied draft edits back to the active theme.
- **Restore default theme**: Revert immediately to the built-in **Graphite and Amber** theme.

## Safe Recovery

If a custom theme makes navigation difficult, launch Eglantine with `--safe-theme` to start that session with the built-in **Graphite and Amber** theme without deleting your saved theme:

```sh
eglantine-gpui --safe-theme
```

From there, open **Settings → Appearance** and choose **Restore default theme** (or import a different package).

## Package Format (`.eglantine-theme`)

A `.eglantine-theme` file is a standard ZIP archive containing `theme.json` at the archive root and optional asset files under `assets/`:

```text
my-theme.eglantine-theme
├── theme.json
└── assets/
    ├── background.png
    ├── custom_font.ttf
    └── icon_library.png
```

Omitted fields in `theme.json` inherit their values from the built-in **Graphite and Amber** theme.

```json
{
  "version": 1,
  "id": "amber-slate",
  "name": "Amber Slate",
  "style": {
    "background": "#201d1a",
    "panel": "#2d2721",
    "foreground": "#fff4e4",
    "muted": "#b6a993",
    "focus_start": "#ffd374",
    "focus_end": "#d18f51",
    "focus_text": "#201d1a",
    "corner_radius": 16,
    "spacing": 16,
    "cover_size": 246,
    "text_scale": 100,
    "motion_ms": 140,
    "font_family": ".SystemUIFont",
    "background_asset": "bg",
    "font_asset": "display_font",
    "icons": {
      "library": "icon_library"
    }
  },
  "layout": {
    "home_mode": "shelf",
    "home_shortcuts": ["library", "controllers", "saves_mods", "content", "settings", "exit"],
    "home_navigation": "below",
    "settings_side": "right",
    "pause_side": "left",
    "library_mode": "list",
    "dialog_width": 480
  },
  "assets": {
    "bg": "png",
    "display_font": "ttf",
    "icon_library": "png"
  }
}
```

## Validation & Accessibility Rules

Packages are validated before assets are loaded or applied:

- **Version & IDs**: `version` must be `1`. Theme and asset IDs must be `1..=48` characters of lowercase ASCII letters, digits, `-`, or `_`, starting with an alphanumeric character.
- **Paths & archive safety**: No path traversal, duplicate archive entries, or undeclared assets.
- **Asset limits & formats**: Maximum `24` assets, `8 MiB` per asset, and `32 MiB` total. Images (`png`, `jpg`/`jpeg`, `webp`) are validated by header/dimensions (`<= 4096 px` per side); fonts (`ttf`, `otf`) are validated with `ttf-parser`.
- **Contrast**: `foreground` and `muted` must achieve at least `4.5:1` contrast against both `background` and `panel`. The focus gradient (`focus_start` → `focus_end`) must maintain at least `4.5:1` contrast against `focus_text` and `3.0:1` against `background` and `panel` across all intermediate gradient stops.
- **Required controls**: `layout.home_shortcuts` cannot repeat IDs and must always include `"settings"`. Any omitted optional shortcuts (`library`, `controllers`, `saves_mods`, `content`, `exit`) are automatically routed into Home's **More** menu.
- **User preferences**: Effective text size always honors the OS accessibility text scale factor on top of `style.text_scale`, and OS reduced-motion disables UI animations regardless of `style.motion_ms`.
