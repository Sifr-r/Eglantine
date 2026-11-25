// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::{path::PathBuf, time::Duration};

use eglantine_gpui::{
    content::ContentOperation,
    emulator_config::{EmulatorConfig, EmulatorSetting},
    library::{self, Library},
    session::SessionClient,
    theme::{ComponentId, HomeMode, LibraryMode, Placement, ThemePackage},
};
use gpui::{Context, FocusHandle, KeyDownEvent, Task, Window};

#[path = "shell_effects.rs"]
mod effects;
use effects::Picker;

use crate::{
    appearance::{Appearance, ThemeAction},
    controller::{Controller, Input},
    navigation::{Direction, move_focus},
    preferences::Preferences,
};

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Target {
    Fullscreen,
    Game(usize),
    Shortcut(ComponentId),
    AddFolder,
    OpenGame,
    GameOptions,
    PlaySelected,
    Refresh,
    Search,
    Category(Category),
    Setting(EmulatorSetting),
    Theme(ThemeAction),
    OpenNand,
    OpenMods,
    OpenGameMods,
    OpenLogs,
    InstallContent,
    InstallFirmware,
    VerifyContent,
    More,
    Back,
    Resume,
    Restart,
    Stop,
    Approve,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Category {
    Emulation,
    Graphics,
    Audio,
    Controllers,
    Paths,
    Appearance,
}

impl Category {
    pub const ALL: [Self; 6] = [
        Self::Emulation, Self::Graphics, Self::Audio, Self::Controllers, Self::Paths, Self::Appearance,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Self::Emulation => "Emulation",
            Self::Graphics => "Graphics",
            Self::Audio => "Audio",
            Self::Controllers => "Controllers",
            Self::Paths => "Paths",
            Self::Appearance => "Appearance",
        }
    }

    pub fn values(self) -> &'static [EmulatorSetting] {
        use EmulatorSetting::*;
        match self {
            Self::Emulation => &[CpuAccuracy, UserProfile],
            Self::Graphics => &[Renderer, Resolution, Vsync, GameDisplay],
            Self::Audio => &[Volume],
            Self::Controllers => &[HomeKey, HomeButton],
            _ => &[],
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Screen {
    Home,
    Library,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Overlay {
    None,
    Settings,
    SavesMods,
    Content,
    More,
    Pause,
    Confirm(Target),
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Session {
    Idle,
    Starting,
    Running,
    Pausing,
    Paused,
    Resuming,
    Closing,
}

pub struct Shell {
    pub(crate) dir: PathBuf,
    pub(crate) focus: FocusHandle,
    pub(crate) controller: Controller,
    pub(crate) target: Target,
    pub(crate) return_target: Target,
    pub(crate) selected: usize,
    pub(crate) screen: Screen,
    pub(crate) overlay: Overlay,
    pub(crate) category: Category,
    pub(crate) game_options: Option<PathBuf>,
    pub(crate) session: Session,
    pub(crate) session_path: Option<PathBuf>,
    pub(crate) library: Option<Library>,
    pub(crate) settings: Option<EmulatorConfig>,
    pub(crate) appearance: Appearance,
    pub(crate) search: String,
    pub(crate) searching: bool,
    pub(crate) message: Option<String>,
    pub(crate) home_released: bool,
    pub(crate) controller_name: Option<String>,
    pub(crate) grid_columns: usize,
    pub(crate) content_busy: bool,
    metadata_request: u64,
    user_profile_override: Option<u8>,
    pub(crate) shelf_scroll: gpui::ScrollHandle,
    pub(crate) view_scroll: gpui::ScrollHandle,
    client: SessionClient,
    _polling: Task<()>,
}

pub struct ShellConfig {
    pub dir: PathBuf,
    pub preferences: Preferences,
    pub message: Option<String>,
    pub safe_theme: bool,
    pub initial_game: Option<PathBuf>,
    pub user_profile_override: Option<u8>,
}

impl Shell {
    pub fn new(window: &mut Window, cx: &mut Context<Self>, config: ShellConfig) -> Self {
        let ShellConfig {
            dir,
            preferences,
            mut message,
            safe_theme,
            initial_game,
            user_profile_override,
        } = config;
        let library = Library::open(dir.clone()).map_err(|error| message = Some(error)).ok();
        let settings = EmulatorConfig::load(&dir).map_err(|error| message = Some(error)).ok();
        let package = ThemePackage::load_active(&dir, safe_theme)
            .map_err(|error| message = Some(format!("Couldn't open the selected theme: {error}")))
            .unwrap_or_default();
        let appearance = Appearance::new(package, cx);
        let focus = cx.focus_handle();
        window.focus(&focus);
        let shell = cx.weak_entity();
        window.on_window_should_close(cx, move |window, cx| {
            shell.update(cx, |shell, cx| {
                if shell.content_busy {
                    shell.message = Some("Wait for the content operation to finish before closing Eglantine.".into());
                    cx.notify();
                    return false;
                }
                shell.save_preferences(window);
                true
            }).unwrap_or(true)
        });
        let polling = cx.spawn_in(window, async |entity, cx| {
            loop {
                cx.background_executor().timer(Duration::from_millis(24)).await;
                if cx.update(|window, cx| entity.update(cx, |shell, cx| shell.poll(window, cx)))
                    .ok().and_then(Result::ok).is_none()
                {
                    break;
                }
            }
        });
        let controller = Controller::new();
        let controller_name = controller.name();
        let entries = library.as_ref().map_or(&[][..], Library::entries);
        let selected = preferences.selected.and_then(|path| entries.iter().position(|entry| entry.path == path))
            .unwrap_or_default();
        let target = if !entries.is_empty() {
            Target::Game(selected)
        } else {
            Target::AddFolder
        };
        let mut shell = Self {
            dir,
            focus,
            controller,
            target,
            return_target: target,
            selected,
            screen: Screen::Home,
            overlay: Overlay::None,
            category: Category::Graphics,
            game_options: None,
            session: Session::Idle,
            session_path: None,
            library,
            settings,
            appearance,
            search: String::new(),
            searching: false,
            message,
            home_released: true,
            controller_name,
            grid_columns: 4,
            content_busy: false,
            metadata_request: 0,
            user_profile_override,
            shelf_scroll: Default::default(),
            view_scroll: Default::default(),
            client: SessionClient::default(),
            _polling: polling,
        };
        shell.scroll_to_game(selected);
        shell.refresh_metadata(window, cx);
        if let Some(game) = initial_game {
            shell.launch(game);
        }
        shell
    }

    pub(crate) fn selected_program_id(&self) -> Option<&str> {
        self.entries().get(self.selected).and_then(|entry| entry.program_id.as_deref())
    }

    pub(crate) fn entries(&self) -> &[library::LibraryEntry] {
        self.library.as_ref().map_or(&[], Library::entries)
    }

    pub(crate) fn visible_entries(&self) -> Vec<usize> {
        let query = self.search.to_lowercase();
        self.entries().iter().enumerate().filter_map(|(index, entry)| {
            (self.screen == Screen::Home || entry.title.to_lowercase().contains(&query)).then_some(index)
        }).collect()
    }

    pub(crate) fn hidden_shortcuts(&self) -> impl Iterator<Item = ComponentId> + '_ {
        const SHORTCUTS: [ComponentId; 6] = [
            ComponentId::Library, ComponentId::Controllers, ComponentId::SavesMods,
            ComponentId::Content, ComponentId::Settings, ComponentId::Exit,
        ];
        let available = &self.appearance.package().theme.layout.home_shortcuts;
        SHORTCUTS.into_iter().filter(|component| !available.contains(component))
    }

    pub(crate) fn categories(&self) -> &'static [Category] {
        if self.game_options.is_some() { &Category::ALL[..3] } else { &Category::ALL }
    }

    fn settings_targets(&self) -> Vec<Target> {
        match self.category {
            Category::Appearance => ThemeAction::ALL.into_iter().map(Target::Theme).collect(),
            Category::Paths => vec![Target::AddFolder, Target::OpenNand, Target::OpenMods, Target::OpenLogs],
            _ => self.category.values().iter().copied().map(Target::Setting).collect(),
        }
    }

    fn focus_rows(&self) -> Vec<Vec<Target>> {
        match self.overlay {
            Overlay::Settings => {
                let mut rows: Vec<_> = self.settings_targets().into_iter().map(|target| vec![target]).collect();
                rows.push(vec![Target::Back]);
                return rows;
            }
            Overlay::SavesMods => {
                let mut rows = vec![vec![Target::OpenMods]];
                if self.selected_program_id().is_some() {
                    rows.push(vec![Target::OpenGameMods]);
                }
                rows.push(vec![Target::OpenNand]);
                rows.push(vec![Target::Back]);
                return rows;
            }
            Overlay::Content => return vec![
                vec![Target::InstallContent], vec![Target::InstallFirmware], vec![Target::VerifyContent],
                vec![Target::OpenGame], vec![Target::OpenNand], vec![Target::Back],
            ],
            Overlay::More => return self.hidden_shortcuts().map(|component| vec![Target::Shortcut(component)])
                .chain([vec![Target::Back]]).collect(),
            Overlay::Pause => return vec![vec![Target::Resume], vec![Target::Restart], vec![Target::Stop]],
            Overlay::Confirm(_) => return vec![vec![Target::Back, Target::Approve]],
            Overlay::None => {}
        }
        let games: Vec<_> = self.visible_entries().into_iter().map(Target::Game).collect();
        let theme = &self.appearance.package().theme;
        match self.screen {
            Screen::Home => {
                let mut shortcuts: Vec<_> = theme.layout.home_shortcuts.iter().copied()
                    .map(Target::Shortcut).collect();
                if self.hidden_shortcuts().next().is_some() {
                    shortcuts.push(Target::More);
                }
                let games = if games.is_empty() { vec![Target::AddFolder] } else { games };
                let mut rows = vec![vec![Target::Fullscreen]];
                let columns = if theme.layout.home_mode == HomeMode::Shelf {
                    games.len()
                } else {
                    self.grid_columns
                };
                if theme.layout.home_navigation == Placement::Below {
                    rows.extend(games.chunks(columns).map(<[_]>::to_vec));
                    rows.push(shortcuts);
                } else {
                    for (index, chunk) in games.chunks(columns).enumerate() {
                        let mut row = chunk.to_vec();
                        if let Some(&shortcut) = shortcuts.get(index) {
                            if theme.layout.home_navigation == Placement::Left {
                                row.insert(0, shortcut);
                            } else {
                                row.push(shortcut);
                            }
                        }
                        rows.push(row);
                    }
                    rows.extend(shortcuts[games.len().div_ceil(columns).min(shortcuts.len())..].iter().map(|item| vec![*item]));
                }
                rows
            }
            Screen::Library => {
                let width = if theme.layout.library_mode == LibraryMode::List { 1 } else { self.grid_columns };
                let mut rows = vec![vec![Target::Back, Target::Search, Target::AddFolder, Target::OpenGame, Target::Refresh]];
                rows.extend(games.chunks(width).map(<[_]>::to_vec));
                if !games.is_empty() {
                    rows.push(vec![Target::PlaySelected, Target::GameOptions]);
                }
                rows
            }
        }
    }

    pub(crate) fn on_key(&mut self, event: &KeyDownEvent, window: &mut Window, cx: &mut Context<Self>) {
        let key = event.keystroke.key.as_str();
        if self.appearance.key(event, cx) {
            cx.notify();
            return;
        }
        if self.searching {
            match key {
                "enter" | "escape" => self.searching = false,
                "backspace" => { self.search.pop(); }
                _ => {
                    if let Some(character) = &event.keystroke.key_char
                        && self.search.len() < 4096
                        && !character.chars().any(char::is_control)
                    {
                        self.search.push_str(character);
                    }
                }
            }
            let visible = self.visible_entries();
            if !visible.contains(&self.selected) && let Some(index) = visible.first() {
                self.selected = *index;
            }
            cx.notify();
            return;
        }
        let home_key = self.settings.as_ref().map_or("F10", EmulatorConfig::home_key);
        let action = match key {
            "left" => Input::Move(Direction::Left),
            "right" => Input::Move(Direction::Right),
            "up" => Input::Move(Direction::Up),
            "down" => Input::Move(Direction::Down),
            "enter" | "space" if !event.is_held => Input::Accept,
            "escape" | "backspace" if !event.is_held => Input::Back,
            "+" | "=" if !event.is_held => Input::Options,
            _ if key.eq_ignore_ascii_case(home_key) && !event.is_held => Input::Home,
            "f11" if !event.is_held => {
                window.toggle_fullscreen();
                cx.notify();
                return;
            }
            _ => return,
        };
        self.handle_input(action, window, cx);
    }

    fn handle_input(&mut self, input: Input, window: &mut Window, cx: &mut Context<Self>) {
        if matches!(input, Input::Home) {
            if self.session == Session::Running {
                self.request_pause();
                cx.notify();
            }
            return;
        }
        if !matches!(self.session, Session::Idle | Session::Paused) || !self.home_released {
            return;
        }
        if self.appearance.is_editing() || self.searching {
            if matches!(input, Input::Back) {
                self.appearance.cancel_edit();
                self.searching = false;
                cx.notify();
            }
            return;
        }
        match input {
            Input::Move(direction) => {
                if self.overlay == Overlay::Settings {
                    self.move_settings_focus(direction);
                } else if let Some(next) = move_focus(&self.focus_rows(), self.target, direction) {
                    self.target = next;
                    if let Target::Game(index) = next {
                        self.selected = index;
                        self.scroll_to_game(index);
                    }
                }
            }
            Input::Accept => self.activate(self.target, window, cx),
            Input::Back if self.overlay == Overlay::Pause => self.activate(Target::Resume, window, cx),
            Input::Back => self.back(cx),
            Input::Options => {
                if let (Some(game), Some(settings), Target::Setting(setting)) =
                    (&self.game_options, &mut self.settings, self.target)
                {
                    if let Err(error) = settings.clear_game(game, setting) {
                        self.message = Some(error);
                    }
                } else {
                    self.open_game_options();
                }
            }
            Input::Home => unreachable!(),
        }
        cx.notify();
    }

    fn scroll_to_game(&self, index: usize) {
        let layout = &self.appearance.package().theme.layout;
        if self.screen == Screen::Home && layout.home_mode == HomeMode::Shelf {
            self.shelf_scroll.scroll_to_item(index);
        } else if let Some(row_index) = self.visible_entries().iter().position(|entry| *entry == index) {
            let columns = if self.screen == Screen::Library && layout.library_mode == LibraryMode::List {
                1
            } else {
                self.grid_columns
            };
            self.shelf_scroll.scroll_to_item(row_index / columns);
        }
    }

    fn move_settings_focus(&mut self, direction: Direction) {
        if let Target::Category(category) = self.target {
            if matches!(direction, Direction::Right) {
                self.select_category(category);
                return;
            }
            let rows: Vec<_> = self.categories().iter().copied()
                .map(|category| vec![Target::Category(category)]).collect();
            if let Some(target) = move_focus(&rows, self.target, direction) {
                self.target = target;
            }
            return;
        }
        if let Target::Theme(action) = self.target
            && matches!(direction, Direction::Left | Direction::Right)
            && self.appearance.nudge(action, if direction == Direction::Left { -1 } else { 1 })
        {
            return;
        }
        if matches!(direction, Direction::Left) {
            self.target = Target::Category(self.category);
            return;
        }
        if let Target::Setting(setting) = self.target && matches!(direction, Direction::Right) {
            self.cycle_setting(setting, 1);
            return;
        }
        if let Some(target) = move_focus(&self.focus_rows(), self.target, direction) {
            self.target = target;
            if let Some(index) = self.settings_targets().iter().position(|item| *item == target) {
                self.view_scroll.scroll_to_item(index);
            }
        }
    }

    pub(crate) fn select_game(
        &mut self, index: usize, open: bool, options: bool, window: &mut Window, cx: &mut Context<Self>,
    ) {
        if self.session != Session::Idle || self.overlay != Overlay::None {
            return;
        }
        self.selected = index;
        self.target = Target::Game(index);
        if options {
            self.open_game_options();
        } else if open {
            self.activate(Target::Game(index), window, cx);
        }
        cx.notify();
    }

    pub(crate) fn activate(&mut self, target: Target, window: &mut Window, cx: &mut Context<Self>) {
        if !matches!(self.session, Session::Idle | Session::Paused) || !self.home_released {
            return;
        }
        if target != Target::Search {
            self.searching = false;
        }
        self.target = target;
        if self.overlay == Overlay::More && matches!(target, Target::Shortcut(_)) {
            self.target = Target::More;
            self.overlay = Overlay::None;
        }
        self.message = None;
        match target {
            Target::Fullscreen => window.toggle_fullscreen(),
            Target::Game(index) => {
                self.selected = index;
                if let Some(entry) = self.entries().get(index) {
                    self.launch(entry.path.clone());
                }
            }
            Target::Shortcut(ComponentId::Exit) => {
                if self.content_busy {
                    self.message = Some("Wait for the content operation to finish before closing Eglantine.".into());
                    cx.notify();
                    return;
                }
                self.save_preferences(window);
                cx.quit();
            }
            Target::Shortcut(ComponentId::Library) => {
                self.screen = Screen::Library;
                self.target = Target::Back;
                self.shelf_scroll.set_offset(gpui::point(gpui::px(0.0), gpui::px(0.0)));
                self.scroll_to_game(self.selected);
            }
            Target::Shortcut(ComponentId::Settings) => self.open_settings(Category::Graphics),
            Target::Shortcut(ComponentId::Controllers) => self.open_settings(Category::Controllers),
            Target::Shortcut(ComponentId::SavesMods) => self.open_overlay(Overlay::SavesMods, Target::OpenMods),
            Target::Shortcut(ComponentId::Content) => self.open_overlay(Overlay::Content, Target::InstallContent),
            Target::AddFolder => self.pick(Picker::Folder, window, cx),
            Target::OpenGame => self.pick(Picker::Game, window, cx),
            Target::GameOptions => self.open_game_options(),
            Target::PlaySelected => {
                if let Some(entry) = self.entries().get(self.selected) {
                    self.launch(entry.path.clone());
                }
            }
            Target::Refresh => {
                if let Some(library) = &mut self.library && let Err(error) = library.refresh() {
                    self.message = Some(error);
                }
                self.refresh_metadata(window, cx);
            }
            Target::Search => self.searching = true,
            Target::Category(category) => self.select_category(category),
            Target::Setting(setting) => self.cycle_setting(setting, 1),
            Target::Theme(action) => self.appearance.activate(action, &self.dir, window, cx),
            Target::OpenNand => self.open_directory("nand", cx),
            Target::OpenMods => self.open_directory("load", cx),
            Target::OpenGameMods => {
                if let Some(program_id) = self.selected_program_id().map(str::to_owned) {
                    self.open_directory(&format!("load/{program_id}"), cx);
                }
            }
            Target::OpenLogs => self.open_directory("log", cx),
            Target::InstallContent => self.pick(Picker::Content, window, cx),
            Target::InstallFirmware => self.pick(Picker::Firmware, window, cx),
            Target::VerifyContent => self.run_content(ContentOperation::VerifyContent, None, window, cx),
            Target::More => {
                let component = self.hidden_shortcuts().next();
                if let Some(component) = component {
                    self.open_overlay(Overlay::More, Target::Shortcut(component));
                }
            }
            Target::Back => self.back(cx),
            Target::Resume if self.session == Session::Paused => {
                match self.client.resume() {
                    Ok(()) => self.session = Session::Resuming,
                    Err(error) => self.message = Some(error),
                }
            }
            Target::Restart | Target::Stop if self.session == Session::Paused => {
                self.overlay = Overlay::Confirm(target);
                self.target = Target::Back;
            }
            Target::Approve => {
                if let Overlay::Confirm(action) = self.overlay {
                    let result = if action == Target::Restart { self.client.restart() } else { self.client.stop() };
                    match result {
                        Ok(()) => self.session = Session::Closing,
                        Err(error) => self.message = Some(error),
                    }
                }
            }
            Target::Resume | Target::Stop | Target::Restart => {}
        }
        cx.notify();
    }

    fn save_preferences(&self, window: &Window) {
        let size = window.window_bounds().get_bounds().size;
        let mut preferences = Preferences {
            width: f32::from(size.width).round() as u16,
            height: f32::from(size.height).round() as u16,
            fullscreen: window.is_fullscreen(),
            selected: self.entries().get(self.selected).map(|entry| entry.path.clone()),
        };
        preferences.clamp_window();
        if let Err(error) = preferences.save(&self.dir) {
            eprintln!("{error}");
        }
    }

    fn back(&mut self, cx: &mut Context<Self>) {
        match self.overlay {
            Overlay::Pause => {},
            Overlay::Confirm(_) => {
                self.overlay = Overlay::Pause;
                self.target = Target::Resume;
            }
            Overlay::None if self.screen == Screen::Library => {
                self.screen = Screen::Home;
                self.target = if self.hidden_shortcuts().any(|item| item == ComponentId::Library) {
                    Target::More
                } else {
                    Target::Shortcut(ComponentId::Library)
                };
                self.shelf_scroll.set_offset(gpui::point(gpui::px(0.0), gpui::px(0.0)));
                self.search.clear();
                self.searching = false;
                self.scroll_to_game(self.selected);
            }
            Overlay::None => {},
            _ => {
                self.appearance.discard(cx);
                self.overlay = Overlay::None;
                self.game_options = None;
                self.target = self.return_target;
            }
        }
    }

    fn open_directory(&mut self, name: &str, cx: &mut Context<Self>) {
        let path = library::emulator_data_dir().join(name);
        if path.is_dir() {
            cx.reveal_path(&path);
        } else {
            self.message = Some(format!("The {name} folder does not exist yet."));
        }
    }

    fn open_settings(&mut self, category: Category) {
        self.open_overlay(Overlay::Settings, Target::Category(category));
        self.select_category(category);
    }

    fn open_game_options(&mut self) {
        if self.overlay == Overlay::None
            && self.visible_entries().contains(&self.selected)
            && let Some(entry) = self.entries().get(self.selected)
        {
            self.game_options = Some(entry.path.clone());
            self.open_settings(Category::Graphics);
        }
    }

    fn select_category(&mut self, category: Category) {
        self.category = category;
        if category == Category::Appearance {
            self.appearance.begin();
        }
        self.target = self.settings_targets().first().copied().unwrap_or(Target::Category(category));
        self.view_scroll.set_offset(gpui::point(gpui::px(0.0), gpui::px(0.0)));
    }

    fn open_overlay(&mut self, overlay: Overlay, target: Target) {
        self.return_target = self.target;
        self.overlay = overlay;
        self.target = target;
        self.view_scroll.set_offset(gpui::point(gpui::px(0.0), gpui::px(0.0)));
    }

    fn cycle_setting(&mut self, setting: EmulatorSetting, direction: i32) {
        if let Some(settings) = &mut self.settings {
            let result = match &self.game_options {
                Some(path) => settings.cycle_game(path, setting, direction),
                None => settings.cycle(setting, direction),
            };
            if let Err(error) = result {
                self.message = Some(error);
            }
        }
    }


}
