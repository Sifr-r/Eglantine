// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use eglantine_library::{ConsoleId, LibraryLayout};
use serde::{Deserialize, Serialize};

use crate::{Color, Theme, ThemeError, ThemeStyle, invalid};

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ThemeComponent {
    Shell,
    ConsoleTab,
    Toolbar,
    LibraryList,
    LibraryTile,
    Preview,
    Play,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ConsoleLayout {
    List,
    Grid,
    SplitPreview,
}

impl ConsoleLayout {
    pub const fn label(self) -> &'static str {
        match self {
            Self::List => "List",
            Self::Grid => "Grid",
            Self::SplitPreview => "Split preview",
        }
    }
}

impl ThemeComponent {
    pub const ALL: [Self; 7] = [
        Self::Shell, Self::ConsoleTab, Self::Toolbar, Self::LibraryList,
        Self::LibraryTile, Self::Preview, Self::Play,
    ];

    pub const fn label(self) -> &'static str {
        match self {
            Self::Shell => "Console interface",
            Self::ConsoleTab => "Console tab",
            Self::Toolbar => "Library toolbar",
            Self::LibraryList => "Game list",
            Self::LibraryTile => "Game tiles",
            Self::Preview => "Game preview",
            Self::Play => "Play button",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ComponentStyle {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub background: Option<Color>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub foreground: Option<Color>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub focus: Option<Color>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub selected_background: Option<Color>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub padding: Option<u16>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub radius: Option<u16>,
}

impl ComponentStyle {
    fn with(self, overrides: Option<&Self>) -> Self {
        let Some(overrides) = overrides else { return self };
        Self {
            background: overrides.background.or(self.background),
            foreground: overrides.foreground.or(self.foreground),
            focus: overrides.focus.or(self.focus),
            selected_background: overrides.selected_background.or(self.selected_background),
            padding: overrides.padding.or(self.padding),
            radius: overrides.radius.or(self.radius),
        }
    }

    pub(crate) fn validate(&self) -> Result<(), ThemeError> {
        if [self.background, self.foreground, self.focus, self.selected_background]
            .into_iter().flatten().any(|color| color.0 > 0xffffff)
            || self.padding.is_some_and(|value| value > 48)
            || self.radius.is_some_and(|value| value > 48)
        {
            return Err(invalid("A component color, padding, or corner radius is out of range"));
        }
        Ok(())
    }

    pub(crate) fn validate_contrast(&self, style: &ThemeStyle, component: ThemeComponent) -> Result<(), ThemeError> {
        let background = self.background.unwrap_or(match component {
            ThemeComponent::Shell => style.background,
            ThemeComponent::Play => style.focus_start,
            _ => style.panel,
        });
        let foreground = self.foreground.unwrap_or(match component {
            ThemeComponent::Play => style.focus_text,
            _ => style.foreground,
        });
        let selected = self.selected_background.unwrap_or(background);
        if [background, selected].into_iter().any(|surface| surface.contrast(foreground) < 4.5)
            || self.focus.is_some_and(|focus| [background, selected].into_iter().any(|surface| surface.contrast(focus) < 3.0))
        {
            return Err(invalid(format!("{} needs readable text and focus contrast", component.label())));
        }
        Ok(())
    }
}

impl Theme {
    pub fn component(&self, console: Option<ConsoleId>, part: ThemeComponent) -> ComponentStyle {
        let specific = console.and_then(|id| self.consoles.get(id.as_str())).and_then(|parts| parts.get(&part));
        console.map_or_else(ComponentStyle::default, |id| built_in(id, part))
            .with(self.components.get(&part))
            .with(specific)
    }

    pub fn console_layout(&self, console: ConsoleId) -> LibraryLayout {
        match self.console_layouts.get(console.as_str()) {
            Some(ConsoleLayout::List) => LibraryLayout::List,
            Some(ConsoleLayout::Grid) => LibraryLayout::Grid,
            Some(ConsoleLayout::SplitPreview) => LibraryLayout::SplitPreview,
            None => console.layout(),
        }
    }
}

fn surface(background: u32, foreground: u32, focus: Option<u32>, selected: Option<u32>) -> ComponentStyle {
    ComponentStyle {
        background: Some(Color(background)), foreground: Some(Color(foreground)),
        focus: focus.map(Color), selected_background: selected.map(Color), ..Default::default()
    }
}

fn built_in(console: ConsoleId, part: ThemeComponent) -> ComponentStyle {
    use ConsoleId::*;
    use ThemeComponent::*;
    match (console, part) {
        (GameBoy, Shell | Toolbar | ConsoleTab) => surface(0xdfded2, 0x21330e, None, None),
        (GameBoy, LibraryList | Preview) => surface(0xbbcc86, 0x21330e, Some(0x426227), Some(0xa6ba6f)),
        (GameBoy, Play) => surface(0x426227, 0xffffff, None, None),
        (GameBoyAdvance, Shell | Toolbar | Preview) => surface(0x232636, 0xf5f3ff, Some(0xbe9cff), None),
        (GameBoyAdvance, ConsoleTab) => surface(0x232636, 0xf5f3ff, Some(0xe9d9ff), Some(0x6640bf)),
        (GameBoyAdvance, LibraryList) => surface(0xbed28e, 0x1e2e15, Some(0x5920a8), Some(0xa5bd75)),
        (GameBoyAdvance, Play) => surface(0x6640bf, 0xffffff, None, None),
        (GameBoyColor, Shell | Toolbar | ConsoleTab | Preview) => surface(0x2c2344, 0xf7eeff, Some(0xe3a8ff), None),
        (GameBoyColor, LibraryTile) => surface(0xf0e6fd, 0x2c1943, Some(0x7543bd), Some(0xdacded)),
        (GameBoyColor, Play) => surface(0x7543bd, 0xffffff, None, None),
        (Ds | Dsi, Shell | Toolbar | LibraryTile | Preview | ConsoleTab) => surface(0xe4edf6, 0x172946, Some(0x2858bd), Some(0xf7fbff)),
        (Ds | Dsi, Play) => surface(0x2858bd, 0xffffff, None, None),
        (Nintendo3Ds, Shell | Toolbar | LibraryTile | Preview | ConsoleTab) => surface(0xf2eee8, 0x43292f, Some(0x94263b), Some(0xfffdf7)),
        (Nintendo3Ds, Play) => surface(0x94263b, 0xffffff, None, None),
        _ => ComponentStyle::default(),
    }
}
