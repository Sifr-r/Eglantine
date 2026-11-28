// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use eglantine_library::{CONSOLES, ConsoleFamily, ConsoleId, LibraryLayout};
use gpui::{Pixels, Point, ScrollStrategy, point, px};
use std::path::{Path, PathBuf};

use super::{Overlay, Screen, Shell, Target, library};

pub(crate) const TABS: &[ConsoleId] = &[
    ConsoleId::GameBoy, ConsoleId::GameBoyColor, ConsoleId::GameBoyAdvance,
    ConsoleId::Ds, ConsoleId::Nintendo3Ds, ConsoleId::Switch, ConsoleId::Switch2,
];

#[derive(Default)]
pub(super) struct TabView {
    screen: Option<Screen>,
    path: Option<PathBuf>,
    query: String,
    alphabetical: bool,
    scroll: Point<Pixels>,
}

impl Shell {
    pub(crate) fn is_classic(console: ConsoleId) -> bool {
        matches!(console, ConsoleId::GameBoy | ConsoleId::GameBoyColor | ConsoleId::GameBoyAdvance)
    }

    pub(crate) fn console_layout(&self) -> Option<LibraryLayout> {
        self.console.map(|console| self.appearance.package().theme.console_layout(console))
            .filter(|layout| *layout != LibraryLayout::Existing)
    }

    pub(crate) fn native_playback(&self) -> bool {
        self.console.is_none_or(|console| matches!(console, ConsoleId::Switch | ConsoleId::Switch2))
    }

    pub(crate) fn can_play_selection(&self) -> bool {
        self.console.is_none_or(ConsoleId::supported) && self.library_indices.contains(&self.selected)
    }

    fn unclassified_native(entry: &library::LibraryEntry) -> bool {
        entry.console.is_none() || entry.console.is_some_and(|console| console.layout() == LibraryLayout::Existing)
    }

    pub(super) fn rebuild_library(&mut self, preferred: Option<&Path>) {
        let console = self.console;
        let query = if self.screen == Screen::Home { "" } else { &self.search };
        self.library_indices = library::browse_indices(self.entries(), query,
            self.screen != Screen::Home && self.alphabetical, |entry| {
                console.map_or_else(|| Self::unclassified_native(entry), |console| entry.console == Some(console))
            });
        let index = preferred.and_then(|path| self.library_indices.iter().find(|&&index| self.entries()[index].path == path).copied());
        self.selected = index.or_else(|| self.library_indices.first().copied()).unwrap_or_default();
    }

    pub(super) fn store_tab(&mut self) {
        let scroll = if self.console_layout().is_some() {
            self.console_scroll.0.borrow().base_handle.offset()
        } else {
            self.shelf_scroll.offset()
        };
        self.tab_views.insert(self.console, TabView {
            screen: Some(self.screen),
            path: self.library_selection_path(),
            query: self.search.clone(),
            alphabetical: self.alphabetical,
            scroll,
        });
    }

    pub(super) fn choose_console(&mut self, console: Option<ConsoleId>) {
        if self.console == console && self.overlay == Overlay::None {
            return;
        }
        self.store_tab();
        self.console = console;
        self.overlay = Overlay::None;
        self.searching = false;
        let saved = self.tab_views.remove(&console).unwrap_or_else(|| TabView {
            alphabetical: console.is_some_and(Self::is_classic), ..Default::default()
        });
        self.screen = saved.screen.unwrap_or(if self.native_playback() { Screen::Home } else { Screen::Library });
        self.search = saved.query;
        self.alphabetical = saved.alphabetical;
        self.pending_library_selection = saved.path;
        let selected = self.pending_library_selection.clone();
        self.rebuild_library(selected.as_deref());
        if self.library_indices.contains(&self.selected) || !self.entries().is_empty() {
            self.pending_library_selection = None;
        }
        self.target = console.map_or(Target::Catalog, Target::Console);
        self.shelf_scroll.set_offset(point(px(0.), px(0.)));
        self.console_scroll.0.borrow().base_handle.set_offset(point(px(0.), px(0.)));
        if self.console_layout().is_some() {
            self.console_scroll.0.borrow().base_handle.set_offset(saved.scroll);
        } else {
            self.shelf_scroll.set_offset(saved.scroll);
        }
    }

    pub(super) fn cycle_console(&mut self, step: i32) {
        let length = TABS.len() as i32;
        let index = self.console.and_then(|console| TABS.iter().position(|item| *item == console))
            .map_or(if step > 0 { -1 } else { 0 }, |index| index as i32);
        self.choose_console(Some(TABS[(index + step).rem_euclid(length) as usize]));
    }

    pub(crate) fn catalog_results(&self) -> impl Iterator<Item = ConsoleId> + '_ {
        let query = self.catalog_query.trim().to_lowercase();
        [ConsoleFamily::Home, ConsoleFamily::Handheld].into_iter().flat_map(|family| {
            CONSOLES.iter().copied().filter(move |console| console.family() == family)
        }).filter(move |console| console.name().to_lowercase().contains(&query))
    }

    pub(super) fn catalog_position(&self, target: ConsoleId) -> Option<usize> {
        let mut family = None;
        let mut grouping_rows = 0;
        for (index, console) in self.catalog_results().enumerate() {
            if family != Some(console.family()) {
                family = Some(console.family());
                grouping_rows += 1;
            }
            if console == target {
                return Some(index + grouping_rows);
            }
        }
        None
    }

    pub(super) fn rail_targets(&self) -> Vec<Target> {
        let mut tabs: Vec<_> = self.rail_consoles().into_iter().map(Target::Console).collect();
        tabs.push(Target::Catalog);
        tabs.push(Target::EmulationSettings);
        tabs.push(Target::Appearance);
        tabs
    }

    pub(crate) fn has_alphabetical_tools(&self) -> bool {
        self.console == Some(ConsoleId::GameBoyAdvance)
    }

    pub(crate) fn rail_consoles(&self) -> Vec<ConsoleId> {
        if self.compact_rail {
            return self.console.into_iter().collect();
        }
        let mut consoles = TABS.to_vec();
        if let Some(console) = self.console && !TABS.contains(&console) {
            consoles.push(console);
        }
        consoles
    }

    pub(crate) fn letter_index(&self) -> Vec<(char, usize)> {
        let mut letters = Vec::new();
        for &index in &self.library_indices {
            let letter = self.entries()[index].title.trim_start().chars().next()
                .filter(char::is_ascii_alphabetic).map_or('#', |letter| letter.to_ascii_uppercase());
            if !letters.iter().any(|(found, _)| *found == letter) {
                letters.push((letter, index));
            }
        }
        letters.sort_by_key(|(letter, _)| *letter);
        letters
    }

    pub(super) fn jump_to_letter(&mut self, letter: char) {
        if let Some((_, index)) = self.letter_index().into_iter().find(|(found, _)| *found == letter) {
            self.overlay = Overlay::None;
            self.target = Target::Game(index);
            self.set_library_selection(index);
            if let Some(position) = self.library_indices.iter().position(|&item| item == index) {
                let columns = if self.console_layout() == Some(LibraryLayout::Grid) { self.grid_columns } else { 1 };
                self.console_scroll.scroll_to_item_strict(position / columns, ScrollStrategy::Top);
            }
        }
    }
}
