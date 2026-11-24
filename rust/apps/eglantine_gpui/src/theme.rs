// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::{self, Display, Formatter};

use serde::{Deserialize, Serialize};

#[path = "theme_package.rs"]
mod package;
pub use package::{MAX_THEME_ASSET_BYTES, ThemePackage};

#[cfg(test)]
#[path = "theme_tests.rs"]
mod tests;

pub const DEFAULT_THEME_ID: &str = "eglantine-default";
pub const THEME_EXTENSION: &str = "eglantine-theme";
pub const FORMAT_VERSION: u32 = 1;
const MAX_ASSETS: usize = 24;

const DEFAULT_SHORTCUTS: [ComponentId; 6] = [
    ComponentId::Library,
    ComponentId::Controllers,
    ComponentId::SavesMods,
    ComponentId::Content,
    ComponentId::Settings,
    ComponentId::Exit,
];

#[derive(Debug)]
pub enum ThemeError {
    Invalid(String),
    Io(std::io::Error),
    Archive(zip::result::ZipError),
    Manifest(serde_json::Error),
}

impl Display for ThemeError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        match self {
            Self::Invalid(message) => formatter.write_str(message),
            Self::Io(error) => write!(formatter, "Theme file: {error}"),
            Self::Archive(error) => write!(formatter, "Theme archive: {error}"),
            Self::Manifest(error) => write!(formatter, "Theme manifest: {error}"),
        }
    }
}

impl std::error::Error for ThemeError {}

impl From<std::io::Error> for ThemeError {
    fn from(error: std::io::Error) -> Self {
        Self::Io(error)
    }
}

impl From<zip::result::ZipError> for ThemeError {
    fn from(error: zip::result::ZipError) -> Self {
        Self::Archive(error)
    }
}

impl From<serde_json::Error> for ThemeError {
    fn from(error: serde_json::Error) -> Self {
        Self::Manifest(error)
    }
}

fn invalid(message: impl Into<String>) -> ThemeError {
    ThemeError::Invalid(message.into())
}

fn valid_id(id: &str) -> bool {
    (1..=48).contains(&id.len())
        && id.as_bytes()[0].is_ascii_alphanumeric()
        && id.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'-' | b'_')
        })
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(try_from = "String", into = "String")]
pub struct Color(pub u32);

impl TryFrom<String> for Color {
    type Error = &'static str;

    fn try_from(value: String) -> Result<Self, Self::Error> {
        let hex = value.strip_prefix('#').ok_or("Expected a #RRGGBB color")?;
        if hex.len() != 6 || !hex.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err("Expected six hexadecimal color digits");
        }
        u32::from_str_radix(hex, 16)
            .map(Self)
            .map_err(|_| "Invalid RGB color")
    }
}

impl From<Color> for String {
    fn from(color: Color) -> Self {
        format!("#{:06x}", color.0)
    }
}

impl Color {
    fn relative_luminance(self) -> f32 {
        let linear = |shift: u32| {
            let value = ((self.0 >> shift) & 0xff_u32) as f32 / 255.0;
            if value <= 0.04045 {
                value / 12.92
            } else {
                ((value + 0.055) / 1.055).powf(2.4)
            }
        };
        0.2126 * linear(16) + 0.7152 * linear(8) + 0.0722 * linear(0)
    }

    fn contrast(self, other: Self) -> f32 {
        let a = self.relative_luminance();
        let b = other.relative_luminance();
        (a.max(b) + 0.05) / (a.min(b) + 0.05)
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ComponentId {
    Library,
    Controllers,
    SavesMods,
    Content,
    Settings,
    Exit,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum HomeMode {
    #[default]
    Shelf,
    Grid,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Placement {
    #[default]
    Below,
    Left,
    Right,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PanelSide {
    Left,
    Right,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LibraryMode {
    #[default]
    List,
    Grid,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ThemeLayout {
    pub home_mode: HomeMode,
    pub home_shortcuts: Vec<ComponentId>,
    pub home_navigation: Placement,
    pub settings_side: PanelSide,
    pub pause_side: PanelSide,
    pub library_mode: LibraryMode,
    pub dialog_width: u16,
}

impl Default for ThemeLayout {
    fn default() -> Self {
        Self {
            home_mode: HomeMode::Shelf,
            home_shortcuts: DEFAULT_SHORTCUTS.to_vec(),
            home_navigation: Placement::Below,
            settings_side: PanelSide::Right,
            pause_side: PanelSide::Left,
            library_mode: LibraryMode::List,
            dialog_width: 480,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AssetKind {
    Png,
    Jpeg,
    Webp,
    Ttf,
    Otf,
}

impl AssetKind {
    pub fn extension(self) -> &'static str {
        match self {
            Self::Png => "png",
            Self::Jpeg => "jpg",
            Self::Webp => "webp",
            Self::Ttf => "ttf",
            Self::Otf => "otf",
        }
    }

    fn is_image(self) -> bool {
        matches!(self, Self::Png | Self::Jpeg | Self::Webp)
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ThemeStyle {
    pub background: Color,
    pub panel: Color,
    pub foreground: Color,
    pub muted: Color,
    pub focus_start: Color,
    pub focus_end: Color,
    pub focus_text: Color,
    pub corner_radius: u16,
    pub spacing: u16,
    pub cover_size: u16,
    pub text_scale: u16,
    pub motion_ms: u16,
    pub font_family: String,
    pub background_asset: Option<String>,
    pub font_asset: Option<String>,
    pub icons: BTreeMap<ComponentId, String>,
}

impl Default for ThemeStyle {
    fn default() -> Self {
        Self {
            background: Color(0x201d1a),
            panel: Color(0x2d2721),
            foreground: Color(0xfff4e4),
            muted: Color(0xb6a993),
            focus_start: Color(0xffd374),
            focus_end: Color(0xd18f51),
            focus_text: Color(0x201d1a),
            corner_radius: 16,
            spacing: 16,
            cover_size: 246,
            text_scale: 100,
            motion_ms: 140,
            font_family: ".SystemUIFont".to_owned(),
            background_asset: None,
            font_asset: None,
            icons: BTreeMap::new(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Theme {
    pub version: u32,
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub style: ThemeStyle,
    #[serde(default)]
    pub layout: ThemeLayout,
    #[serde(default)]
    pub assets: BTreeMap<String, AssetKind>,
}

impl Default for Theme {
    fn default() -> Self {
        Self {
            version: FORMAT_VERSION,
            id: DEFAULT_THEME_ID.to_owned(),
            name: "Graphite and Amber".to_owned(),
            style: ThemeStyle::default(),
            layout: ThemeLayout::default(),
            assets: BTreeMap::new(),
        }
    }
}

impl Theme {
    pub fn validate(&self) -> Result<(), ThemeError> {
        self.validate_draft()?;
        self.style.validate_contrast()
    }

    pub fn validate_draft(&self) -> Result<(), ThemeError> {
        if self.version != FORMAT_VERSION {
            return Err(invalid(format!("Unsupported theme version {}", self.version)));
        }
        if !valid_id(&self.id) || !valid_label(&self.name) {
            return Err(invalid("Theme requires a valid ID and display name"));
        }
        self.style.validate()?;
        self.layout.validate()?;

        if self.assets.len() > MAX_ASSETS || self.assets.keys().any(|id| !valid_id(id)) {
            return Err(invalid("Asset IDs must be simple names and at most 24 are allowed"));
        }
        for image in self.style.background_asset.iter().chain(self.style.icons.values()) {
            if !self.assets.get(image).is_some_and(|kind| kind.is_image()) {
                return Err(invalid(format!("Missing or invalid theme image {image:?}")));
            }
        }
        if let Some(font) = &self.style.font_asset {
            if !self.assets.get(font).is_some_and(|kind| !kind.is_image()) {
                return Err(invalid(format!("Missing or invalid theme font {font:?}")));
            }
        }
        Ok(())
    }

    pub fn duplicate(&self) -> Self {
        let mut copy = self.clone();
        let trimmed = self.name.trim();
        let base_name: String = trimmed.chars().take(75).collect();
        copy.name = if base_name.is_empty() {
            "Custom Copy".to_owned()
        } else {
            format!("{base_name} Copy")
        };
        let base_id = self.id.trim_end_matches(['-', '_']);
        let prefix: String = base_id.chars().take(43).collect();
        copy.id = if prefix.is_empty() || !valid_id(&format!("{prefix}-copy")) {
            "custom-copy".to_owned()
        } else {
            format!("{prefix}-copy")
        };
        copy
    }
}

impl ThemeStyle {
    fn validate(&self) -> Result<(), ThemeError> {
        let colors = [self.background, self.panel, self.foreground, self.muted,
            self.focus_start, self.focus_end, self.focus_text];
        if colors.iter().any(|color| color.0 > 0xffffff) {
            return Err(invalid("Theme colors must be six-digit RGB values"));
        }
        if self.corner_radius > 48 || !(4..=48).contains(&self.spacing)
            || !(104..=360).contains(&self.cover_size) || !(75..=200).contains(&self.text_scale)
            || self.motion_ms > 1000 || !valid_label(&self.font_family)
        {
            return Err(invalid("A theme size, motion duration, or font family is out of range"));
        }
        Ok(())
    }

    fn validate_contrast(&self) -> Result<(), ThemeError> {
        for surface in [self.background, self.panel] {
            if [self.foreground, self.muted].into_iter().any(|text| surface.contrast(text) < 4.5) {
                return Err(invalid("Text must have 4.5:1 contrast on the background and panels"));
            }
        }
        // Check intermediate stops because two colors can straddle the surrounding luminance.
        for step in 0..=16 {
            let stop = mix(self.focus_start, self.focus_end, step);
            if stop.contrast(self.focus_text) < 4.5
                || stop.contrast(self.background) < 3.0
                || stop.contrast(self.panel) < 3.0
            {
                return Err(invalid("The focus gradient needs 4.5:1 text contrast and 3:1 surface contrast"));
            }
        }
        Ok(())
    }
}

impl ThemeLayout {
    fn validate(&self) -> Result<(), ThemeError> {
        let ids: BTreeSet<_> = self.home_shortcuts.iter().copied().collect();
        if ids.len() != self.home_shortcuts.len() || !ids.contains(&ComponentId::Settings) {
            return Err(invalid("Home shortcuts must include Settings and cannot repeat"));
        }
        if !(280..=960).contains(&self.dialog_width) {
            return Err(invalid("Dialog width must be between 280 and 960"));
        }
        Ok(())
    }
}

fn valid_label(label: &str) -> bool {
    !label.trim().is_empty() && label.chars().count() <= 80 && !label.chars().any(char::is_control)
}

fn mix(start: Color, end: Color, step: u32) -> Color {
    let channel = |shift: u32| -> u32 {
        let a = (start.0 >> shift) & 0xff_u32;
        let b = (end.0 >> shift) & 0xff_u32;
        ((a * (16 - step) + b * step) / 16_u32) << shift
    };
    Color(channel(16) | channel(8) | channel(0))
}
