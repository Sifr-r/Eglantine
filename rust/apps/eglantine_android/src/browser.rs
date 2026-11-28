// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::ffi::CStr;

use eglantine_library::ConsoleId;

use crate::library::GameEntry;
use crate::state::{Activity, App, status_with_detail};

impl App {
    pub(super) fn visible_game_indices(&self) -> Vec<usize> {
        let active = self.library.active_console;
        self.library
            .games()
            .iter()
            .enumerate()
            .filter_map(|(idx, game)| match active {
                None => Some(idx),
                Some(console) if game.console == Some(console) => Some(idx),
                Some(_) => None,
            })
            .collect()
    }

    pub(super) fn visible_game_count(&self) -> usize {
        let active = self.library.active_console;
        self.library
            .games()
            .iter()
            .filter(|game| active.is_none_or(|console| game.console == Some(console)))
            .count()
    }

    pub(super) fn visible_game_at(&self, visible_index: usize) -> Option<(usize, &GameEntry)> {
        let active = self.library.active_console;
        let games = self.library.games();
        games
            .iter()
            .enumerate()
            .filter(|(_, game)| active.is_none_or(|console| game.console == Some(console)))
            .nth(visible_index)
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn selected_title(&self) -> Option<&CStr> {
        let visible_idx = self.selected_game_index()?;
        let (_, game) = self.visible_game_at(visible_idx)?;
        Some(game.title.as_c_str())
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn classify_game(&mut self, uri: &str, console_id: &str) {
        let Some(console) = ConsoleId::parse(console_id) else {
            return;
        };
        let Some(game) = self.library.game_by_uri_mut(uri) else {
            return;
        };
        game.console = Some(console);
        self.sync_visible_selection();
        self.clamp_scroll();
        self.save_library();
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn select_console(&mut self, console_id: Option<&str>) -> bool {
        if !self.is_idle() {
            return false;
        }
        let target = match console_id {
            None => None,
            Some(id) => match ConsoleId::parse(id).filter(|c| c.supported()) {
                Some(c) => Some(c),
                None => return false,
            },
        };

        let previous = self.library.active_console;
        if let Some(visible_idx) = self.selected_game_index()
            && let Some((_, game)) = self.visible_game_at(visible_idx)
        {
            self.console_selections.insert(previous, game.uri.clone());
        }

        self.library.active_console = target;
        let visible = self.visible_game_indices();
        let chosen_raw_idx = self
            .console_selections
            .get(&target)
            .and_then(|saved_uri| {
                visible
                    .iter()
                    .copied()
                    .find(|&raw_idx| self.library.games()[raw_idx].uri == *saved_uri)
            })
            .or_else(|| visible.first().copied());

        if let Some(raw_idx) = chosen_raw_idx {
            if let Some(uri) = self.library.select(raw_idx).map(CStr::to_owned) {
                self.console_selections.insert(target, uri.clone());
                self.document = Some(uri);
            }
        } else {
            self.document = None;
        }

        self.clamp_scroll();
        self.save_library();
        true
    }

    pub(super) fn sync_visible_selection(&mut self) {
        let active = self.library.active_console;
        let visible = self.visible_game_indices();
        if let Some(selected_raw) = self.library.selected_index()
            && visible.contains(&selected_raw)
        {
            if let Some(game) = self.library.games().get(selected_raw) {
                self.console_selections.insert(active, game.uri.clone());
                if self.document.is_none() {
                    self.document = Some(game.uri.clone());
                }
            }
            return;
        }

        if let Some(&first_raw) = visible.first() {
            if let Some(uri) = self.library.select(first_raw).map(CStr::to_owned) {
                self.console_selections.insert(active, uri.clone());
                if self.document.is_none()
                    || active.is_some()
                {
                    self.document = Some(uri);
                }
            }
        } else if active.is_some() {
            self.document = None;
        }
    }

    pub(super) fn select_visible_game(&mut self, visible_index: usize) -> bool {
        if !self.is_idle() {
            return false;
        }
        let Some((raw_idx, _)) = self.visible_game_at(visible_index) else {
            return false;
        };
        let Some(uri) = self.library.select(raw_idx).map(CStr::to_owned) else {
            return false;
        };
        self.console_selections
            .insert(self.library.active_console, uri.clone());
        self.document = Some(uri);
        self.save_library();
        if let Some(game) = self.library.selected_game() {
            let title = game.title.to_string_lossy();
            self.activity = Activity::Message(status_with_detail("Selected: ", &title).into());
        }
        true
    }
}
