// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::{
    borrow::Cow,
    collections::{BTreeMap, BTreeSet},
    hash::{DefaultHasher, Hash, Hasher},
    path::Path,
    sync::Arc,
};

use eglantine_gpui::theme::{AssetKind, Color, ComponentId, MAX_THEME_ASSET_BYTES, PanelSide, Placement, ThemePackage};
use gpui::{Context, Image, ImageFormat, KeyDownEvent, PathPromptOptions, Window};

use crate::shell::Shell;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PreviewScreen {
    Home,
    Library,
    Pause,
    Dialog,
}

impl PreviewScreen {
    pub fn label(self) -> &'static str {
        match self {
            Self::Home => "Home",
            Self::Library => "Library",
            Self::Pause => "Pause sheet",
            Self::Dialog => "Dialog",
        }
    }

    pub fn next(self) -> Self {
        match self {
            Self::Home => Self::Library,
            Self::Library => Self::Pause,
            Self::Pause => Self::Dialog,
            Self::Dialog => Self::Home,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ThemeAction {
    Name, Background, Surface, Text, Muted, FocusStart, FocusEnd, FocusText,
    Font, TextSize, Spacing, Corners, CoverSize, Motion, HomeMode, Shortcuts,
    MoveShortcutEarlier, MoveShortcutLater, ToggleShortcut, LibraryMode, SettingsSide, PauseSide, DialogWidth,
    BackgroundImage, ClearBackground, FontFile, ClearFont, IconTarget, IconFile, ClearIcon,
    PreviewScreen, Duplicate, Apply, Import, Export, Discard, Default,
}

impl ThemeAction {
    pub const ALL: [Self; 37] = [
        Self::PreviewScreen, Self::Duplicate, Self::Apply, Self::Import, Self::Export, Self::Discard, Self::Default,
        Self::Name, Self::Background, Self::Surface, Self::Text, Self::Muted, Self::FocusStart,
        Self::FocusEnd, Self::FocusText, Self::Font, Self::TextSize, Self::Spacing, Self::Corners,
        Self::CoverSize, Self::Motion, Self::HomeMode, Self::Shortcuts, Self::IconTarget,
        Self::MoveShortcutEarlier, Self::MoveShortcutLater, Self::ToggleShortcut, Self::IconFile, Self::ClearIcon,
        Self::LibraryMode, Self::SettingsSide, Self::PauseSide, Self::DialogWidth,
        Self::BackgroundImage, Self::ClearBackground, Self::FontFile, Self::ClearFont,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Self::Name => "Theme name", Self::Background => "Background", Self::Surface => "Panels",
            Self::Text => "Text", Self::Muted => "Secondary text", Self::FocusStart => "Focus gradient start",
            Self::FocusEnd => "Focus gradient end", Self::FocusText => "Focused text",
            Self::Font => "Font family", Self::TextSize => "Text scale", Self::Spacing => "Spacing",
            Self::Corners => "Corner radius", Self::CoverSize => "Cover size", Self::Motion => "Motion duration",
            Self::HomeMode => "Home layout", Self::Shortcuts => "Shortcut placement",
            Self::MoveShortcutEarlier => "Move shortcut earlier", Self::MoveShortcutLater => "Move shortcut later",
            Self::ToggleShortcut => "Toggle Home shortcut", Self::LibraryMode => "Library layout",
            Self::SettingsSide => "Settings drawer", Self::PauseSide => "Pause sheet",
            Self::DialogWidth => "Dialog width", Self::BackgroundImage => "Choose background image",
            Self::ClearBackground => "Remove background image", Self::FontFile => "Choose font file",
            Self::ClearFont => "Remove embedded font", Self::IconTarget => "Shortcut to edit",
            Self::IconFile => "Choose shortcut icon", Self::ClearIcon => "Use default shortcut icon",
            Self::PreviewScreen => "Preview screen", Self::Duplicate => "Duplicate theme",
            Self::Apply => "Apply theme", Self::Import => "Import theme",
            Self::Export => "Export theme", Self::Discard => "Discard changes", Self::Default => "Restore default theme",
        }
    }

    fn image_or_font(self) -> bool {
        matches!(self, Self::BackgroundImage | Self::FontFile | Self::IconFile)
    }
}

pub struct Appearance {
    active: ThemePackage,
    draft: Option<ThemePackage>,
    draft_usable: bool,
    edit: Option<(ThemeAction, String)>,
    images: BTreeMap<String, Arc<Image>>,
    registered_fonts: BTreeSet<u64>,
    icon_target: ComponentId,
    preview_screen: PreviewScreen,
    pub message: Option<String>,
}

impl Appearance {
    pub fn new(active: ThemePackage, cx: &mut Context<Shell>) -> Self {
        let mut state = Self {
            active,
            draft: None,
            draft_usable: true,
            edit: None,
            images: BTreeMap::new(),
            registered_fonts: BTreeSet::new(),
            icon_target: ComponentId::Library,
            preview_screen: PreviewScreen::Home,
            message: None,
        };
        state.load_assets(cx);
        state
    }

    pub fn preview_screen(&self) -> PreviewScreen {
        self.preview_screen
    }

    pub fn package(&self) -> &ThemePackage {
        if self.draft_usable { self.editing_package() } else { &self.active }
    }

    fn editing_package(&self) -> &ThemePackage {
        self.draft.as_ref().unwrap_or(&self.active)
    }

    pub fn image(&self, id: &str) -> Option<Arc<Image>> {
        self.images.get(id).cloned()
    }

    pub fn begin(&mut self) {
        self.draft.get_or_insert_with(|| self.active.clone());
    }

    pub fn discard(&mut self, cx: &mut Context<Shell>) {
        if self.draft.take().is_some() {
            self.draft_usable = true;
            self.load_assets(cx);
        }
        self.edit = None;
        self.message = None;
    }

    pub fn value(&self, action: ThemeAction) -> String {
        if let Some((current, text)) = &self.edit && action == *current {
            return format!("{text}▏");
        }
        let theme = &self.editing_package().theme;
        let style = &theme.style;
        let layout = &theme.layout;
        use ThemeAction::*;
        match action {
            Name => theme.name.clone(),
            Background => hex(style.background),
            Surface => hex(style.panel),
            Text => hex(style.foreground),
            Muted => hex(style.muted),
            FocusStart => hex(style.focus_start),
            FocusEnd => hex(style.focus_end),
            FocusText => hex(style.focus_text),
            Font => style.font_family.clone(),
            TextSize => style.text_scale.to_string(),
            Spacing => style.spacing.to_string(),
            Corners => style.corner_radius.to_string(),
            CoverSize => style.cover_size.to_string(),
            Motion => style.motion_ms.to_string(),
            HomeMode => if layout.home_mode == eglantine_gpui::theme::HomeMode::Shelf { "Shelf" } else { "Grid" }.into(),
            Shortcuts => match layout.home_navigation { Placement::Below => "Below", Placement::Left => "Left", Placement::Right => "Right" }.into(),
            LibraryMode => if layout.library_mode == eglantine_gpui::theme::LibraryMode::List { "List" } else { "Grid" }.into(),
            SettingsSide => side_label(layout.settings_side).into(),
            PauseSide => side_label(layout.pause_side).into(),
            DialogWidth => layout.dialog_width.to_string(),
            IconTarget => crate::views::shortcut_label(self.icon_target).into(),
            ToggleShortcut => if layout.home_shortcuts.contains(&self.icon_target) { "On Home" } else { "In More" }.into(),
            PreviewScreen => self.preview_screen.label().into(),
            _ => String::new(),
        }
    }

    pub fn key(&mut self, event: &KeyDownEvent, cx: &mut Context<Shell>) -> bool {
        let Some((action, text)) = &mut self.edit else { return false };
        match &*event.keystroke.key {
            "escape" => self.cancel_edit(),
            "enter" => {
                let action = *action;
                let text = text.trim().to_owned();
                match self.set_value(action, &text) {
                    Ok(()) => {
                        self.edit = None;
                        self.check_preview();
                        self.load_assets(cx);
                    }
                    Err(error) => self.message = Some(error),
                }
            }
            "backspace" => { text.pop(); }
            _ => if let Some(character) = &event.keystroke.key_char
                && text.len() < 120 && !character.chars().any(char::is_control)
            {
                text.push_str(character);
            }
        }
        true
    }

    pub fn is_editing(&self) -> bool {
        self.edit.is_some()
    }

    pub fn cancel_edit(&mut self) {
        self.edit = None;
        self.check_preview();
    }

    pub fn nudge(&mut self, action: ThemeAction, direction: i32) -> bool {
        use ThemeAction::*;
        let step = match action {
            TextSize => 5,
            Spacing | Corners => 2,
            CoverSize | DialogWidth => 16,
            Motion => 20,
            _ => return false,
        };
        let Ok(current) = self.value(action).parse::<i32>() else { return true };
        let next = (current + direction * step).to_string();
        match self.set_value(action, &next) {
            Ok(()) => self.check_preview(),
            Err(error) => self.message = Some(error),
        }
        true
    }

    fn check_preview(&mut self) {
        self.message = self.editing_package().theme.validate().err().map(|error| error.to_string());
        self.draft_usable = self.message.is_none();
    }

    fn set_value(&mut self, action: ThemeAction, text: &str) -> Result<(), String> {
        self.begin();
        let draft = self.draft.as_mut().expect("begin creates a draft");
        let old = draft.theme.clone();
        let theme = &mut draft.theme;
        let style = &mut theme.style;
        let number = || text.parse().map_err(|_| "Enter a whole number.".to_owned());
        use ThemeAction::*;
        match action {
            Name => {
                theme.name = text.to_owned();
                let mut id: String = text.chars().filter_map(|character| {
                    if character.is_ascii_alphanumeric() {
                        Some(character.to_ascii_lowercase())
                    } else if character == ' ' { Some('-') } else { None }
                }).take(40).collect();
                if id.is_empty() {
                    id = "custom".into();
                }
                theme.id = format!("theme-{id}");
            }
            Background => style.background = parse_color(text)?,
            Surface => style.panel = parse_color(text)?,
            Text => style.foreground = parse_color(text)?,
            Muted => style.muted = parse_color(text)?,
            FocusStart => style.focus_start = parse_color(text)?,
            FocusEnd => style.focus_end = parse_color(text)?,
            FocusText => style.focus_text = parse_color(text)?,
            Font => style.font_family = text.to_owned(),
            TextSize => style.text_scale = number()?,
            Spacing => style.spacing = number()?,
            Corners => style.corner_radius = number()?,
            CoverSize => style.cover_size = number()?,
            Motion => style.motion_ms = number()?,
            DialogWidth => theme.layout.dialog_width = number()?,
            _ => return Err("That field does not accept text.".into()),
        }
        if let Err(error) = theme.validate_draft() {
            *theme = old;
            return Err(error.to_string());
        }
        Ok(())
    }

    pub fn activate(&mut self, action: ThemeAction, dir: &Path, window: &Window, cx: &mut Context<Shell>) {
        self.edit = None;
        self.message = None;
        if action == ThemeAction::PreviewScreen {
            self.preview_screen = self.preview_screen.next();
            return;
        }
        self.begin();
        if action == ThemeAction::Import || action == ThemeAction::Export || action.image_or_font() {
            self.pick(action, window, cx);
            return;
        }
        let draft = self.draft.as_mut().expect("begin creates a draft");
        use ThemeAction::*;
        let layout = &mut draft.theme.layout;
        match action {
            Duplicate => {
                draft.theme = draft.theme.duplicate();
                let name = draft.theme.name.clone();
                self.check_preview();
                if self.message.is_none() {
                    self.message = Some(format!("Duplicated as \"{name}\"."));
                }
                return;
            }
            Apply => match draft.save_active(dir) {
                Ok(()) => {
                    self.active = self.draft.take().expect("saved the draft");
                    self.draft_usable = true;
                    self.message = Some("Theme applied.".into());
                }
                Err(error) => self.message = Some(error.to_string()),
            },
            Default => match ThemePackage::restore_default(dir) {
                Ok(()) => {
                    self.active = ThemePackage::default();
                    self.draft = None;
                    self.draft_usable = true;
                    self.load_assets(cx);
                    self.message = Some("Default theme restored.".into());
                }
                Err(error) => self.message = Some(error.to_string()),
            },
            Discard => self.discard(cx),
            ClearBackground | ClearFont | ClearIcon => {
                let style = &mut draft.theme.style;
                let id = match action {
                    ClearBackground => style.background_asset.take(),
                    ClearFont => {
                        style.font_family = ThemePackage::default().theme.style.font_family;
                        style.font_asset.take()
                    }
                    ClearIcon => style.icons.remove(&self.icon_target),
                    _ => unreachable!(),
                };
                if let Some(id) = id {
                    let still_in_use = style.background_asset.as_deref() == Some(&id)
                        || style.font_asset.as_deref() == Some(&id) || style.icons.values().any(|asset| asset == &id);
                    if !still_in_use {
                        draft.remove_asset(&id);
                    }
                }
                self.check_preview();
                self.load_assets(cx);
            }
            HomeMode => layout.home_mode = match layout.home_mode {
                eglantine_gpui::theme::HomeMode::Shelf => eglantine_gpui::theme::HomeMode::Grid,
                eglantine_gpui::theme::HomeMode::Grid => eglantine_gpui::theme::HomeMode::Shelf,
            },
            Shortcuts => layout.home_navigation = match layout.home_navigation {
                Placement::Below => Placement::Left,
                Placement::Left => Placement::Right,
                Placement::Right => Placement::Below,
            },
            MoveShortcutEarlier | MoveShortcutLater => {
                let shortcuts = &mut layout.home_shortcuts;
                if let Some(index) = shortcuts.iter().position(|item| *item == self.icon_target) {
                    let destination = if action == MoveShortcutEarlier {
                        index.saturating_sub(1)
                    } else {
                        (index + 1).min(shortcuts.len() - 1)
                    };
                    shortcuts.swap(index, destination);
                }
            }
            ToggleShortcut => {
                if self.icon_target == ComponentId::Settings {
                    self.message = Some("Settings stays on Home so every theme can be edited.".into());
                } else if let Some(index) = layout.home_shortcuts.iter().position(|item| *item == self.icon_target) {
                    layout.home_shortcuts.remove(index);
                } else {
                    layout.home_shortcuts.push(self.icon_target);
                }
            }
            LibraryMode => layout.library_mode = match layout.library_mode {
                eglantine_gpui::theme::LibraryMode::List => eglantine_gpui::theme::LibraryMode::Grid,
                eglantine_gpui::theme::LibraryMode::Grid => eglantine_gpui::theme::LibraryMode::List,
            },
            SettingsSide => layout.settings_side = opposite(layout.settings_side),
            PauseSide => layout.pause_side = opposite(layout.pause_side),
            IconTarget => {
                let options = ThemePackage::default().theme.layout.home_shortcuts;
                self.icon_target = options.iter().position(|item| *item == self.icon_target)
                    .and_then(|index| options.get(index + 1)).copied().unwrap_or(options[0]);
            }
            _ => {
                self.edit = Some((action, String::new()));
                self.message = Some("Type a value, then press Enter. Esc cancels.".into());
            }
        }
        if self.message.is_none() && !self.draft_usable {
            self.check_preview();
        }
    }

    fn pick(&self, action: ThemeAction, window: &Window, cx: &Context<Shell>) {
        if action == ThemeAction::Export {
            let folder = std::env::var_os("USERPROFILE").map(std::path::PathBuf::from)
                .or_else(|| std::env::current_dir().ok()).unwrap_or_default();
            let name = self.editing_package().suggested_filename();
            let reply = cx.prompt_for_new_path(&folder, Some(&name));
            cx.spawn_in(window, async move |shell, cx| {
                let result = reply.await;
                let _ = cx.update(|window, cx| shell.update(cx, |shell, cx| {
                    window.focus(&shell.focus);
                    shell.appearance.message = match result {
                        Ok(Ok(Some(path))) => Some(match shell.appearance.editing_package().export(&path) {
                            Ok(()) => "Theme exported.".to_owned(),
                            Err(error) => error.to_string(),
                        }),
                        Ok(Err(error)) => Some(error.to_string()),
                        _ => None,
                    };
                    cx.notify();
                }));
            }).detach();
            return;
        }
        let reply = cx.prompt_for_paths(PathPromptOptions {
            files: true, directories: false, multiple: false, prompt: Some(action.label().into()),
        });
        cx.spawn_in(window, async move |shell, cx| {
            let result = reply.await;
            let _ = cx.update(|window, cx| shell.update(cx, |shell, cx| {
                window.focus(&shell.focus);
                if let Ok(Ok(Some(paths))) = result {
                    if let Some(path) = paths.first() {
                        shell.appearance.message = match shell.appearance.read_file(action, path) {
                            Ok(()) => {
                                shell.appearance.load_assets(cx);
                                Some("Preview ready. Apply to keep the changes.".into())
                            }
                            Err(error) => Some(error),
                        };
                    }
                } else if let Ok(Err(error)) = result {
                    shell.appearance.message = Some(error.to_string());
                }
                cx.notify();
            }));
        }).detach();
    }

    fn read_file(&mut self, action: ThemeAction, path: &Path) -> Result<(), String> {
        if action == ThemeAction::Import {
            self.draft = Some(ThemePackage::import(path).map_err(|error| error.to_string())?);
            self.draft_usable = true;
            return Ok(());
        }
        let extension = path.extension().and_then(|part| part.to_str()).unwrap_or_default().to_ascii_lowercase();
        let kind = match extension.as_str() {
            "png" => AssetKind::Png, "jpg" | "jpeg" => AssetKind::Jpeg, "webp" => AssetKind::Webp,
            "ttf" => AssetKind::Ttf, "otf" => AssetKind::Otf,
            _ => return Err("Choose a PNG, JPEG, WebP, TTF, or OTF file.".into()),
        };
        if matches!(kind, AssetKind::Ttf | AssetKind::Otf) != (action == ThemeAction::FontFile) {
            return Err("The selected file is the wrong type for this theme setting.".into());
        }
        const MAX_ASSET: u64 = MAX_THEME_ASSET_BYTES as u64;
        use std::io::Read;
        let file = std::fs::File::open(path).map_err(|error| error.to_string())?;
        let mut bytes = Vec::new();
        file.take(MAX_ASSET + 1).read_to_end(&mut bytes).map_err(|error| error.to_string())?;
        if bytes.len() as u64 > MAX_ASSET {
            return Err(format!("The asset is larger than {} MiB.", MAX_ASSET / (1024 * 1024)));
        }
        self.begin();
        let draft = self.draft.as_mut().expect("begin creates a draft");
        let id = match action {
            ThemeAction::BackgroundImage => "background".to_owned(),
            ThemeAction::FontFile => "font".to_owned(),
            ThemeAction::IconFile => format!("icon-{}", match self.icon_target {
                ComponentId::Library => "library", ComponentId::Controllers => "controllers",
                ComponentId::SavesMods => "saves-mods", ComponentId::Content => "content",
                ComponentId::Settings => "settings", ComponentId::Exit => "exit",
            }),
            _ => return Err("This action does not accept an asset.".into()),
        };
        draft.set_asset(&id, kind, bytes).map_err(|error| error.to_string())?;
        match action {
            ThemeAction::BackgroundImage => draft.theme.style.background_asset = Some(id),
            ThemeAction::FontFile => {
                if let Some(family) = draft.font_family(&id) {
                    draft.theme.style.font_family = family;
                }
                draft.theme.style.font_asset = Some(id);
            }
            ThemeAction::IconFile => { draft.theme.style.icons.insert(self.icon_target, id); }
            _ => unreachable!(),
        }
        Ok(())
    }

    fn load_assets(&mut self, cx: &Context<Shell>) {
        let package = self.package();
        let images = package.theme.assets.iter().filter_map(|(id, kind)| {
            let format = match kind {
                AssetKind::Png => ImageFormat::Png,
                AssetKind::Jpeg => ImageFormat::Jpeg,
                AssetKind::Webp => ImageFormat::Webp,
                _ => return None,
            };
            package.asset(id).map(|bytes| (id.clone(), Arc::new(Image::from_bytes(format, bytes.to_vec()))))
        }).collect();
        let font = package.theme.style.font_asset.as_ref().and_then(|id| package.asset(id)).map(|bytes| {
            let mut hasher = DefaultHasher::new();
            bytes.hash(&mut hasher);
            (hasher.finish(), bytes)
        });
        let font = font.filter(|(id, _)| !self.registered_fonts.contains(id))
            .map(|(id, bytes)| (id, bytes.to_vec()));
        self.images = images;
        if let Some((id, bytes)) = font {
            match cx.text_system().add_fonts(vec![Cow::Owned(bytes)]) {
                Ok(()) => { self.registered_fonts.insert(id); }
                Err(error) => self.message = Some(format!("Couldn't load the theme font: {error}")),
            }
        }
    }
}

fn hex(color: Color) -> String {
    format!("#{:06x}", color.0)
}

fn parse_color(text: &str) -> Result<Color, String> {
    let raw = text.strip_prefix('#').unwrap_or(text);
    if raw.len() == 6 && let Ok(value) = u32::from_str_radix(raw, 16) {
        Ok(Color(value))
    } else {
        Err("Enter a six-digit hex color, such as #ffc875.".into())
    }
}

fn side_label(side: PanelSide) -> &'static str {
    match side { PanelSide::Left => "Left", PanelSide::Right => "Right" }
}

fn opposite(side: PanelSide) -> PanelSide {
    match side { PanelSide::Left => PanelSide::Right, PanelSide::Right => PanelSide::Left }
}
