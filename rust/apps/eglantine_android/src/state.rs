// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::borrow::Cow;
use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::path::PathBuf;

use eglantine_library::ConsoleId;

use crate::library::{GameEntry, LibraryStore, is_document_uri};
use crate::overlay::OverlayState;
use crate::settings::SettingsSnapshot;

const MAX_STATUS_DETAIL: usize = 88;
pub(super) const VISIBLE_GAME_ROWS: usize = 5;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) enum ViewMode {
    #[default]
    Library,
    Settings,
}

pub(super) struct App {
    suspended: bool,
    pub(super) document: Option<CString>,
    pub(super) activity: Activity,
    view: ViewMode,
    pub(super) library: LibraryStore,
    pub(super) console_selections: HashMap<Option<ConsoleId>, CString>,
    overlay: OverlayState,
    settings: SettingsSnapshot,
    driver_name: CString,
    storage_dir: Option<PathBuf>,
    has_prod_keys: bool,
    pub(super) scroll_offset: usize,
}

impl Default for App {
    fn default() -> Self {
        Self {
            suspended: false,
            document: None,
            activity: Activity::default(),
            view: ViewMode::Library,
            library: LibraryStore::default(),
            console_selections: HashMap::new(),
            overlay: OverlayState::default(),
            settings: SettingsSnapshot::default(),
            driver_name: c"System default".to_owned(),
            storage_dir: None,
            has_prod_keys: false,
            scroll_offset: 0,
        }
    }
}

impl App {
    pub(super) fn set_suspended(&mut self, suspended: bool) {
        self.suspended = suspended;
    }

    pub(super) fn set_storage_dir(&mut self, dir: &str) {
        if dir.is_empty() {
            return;
        }
        let path = PathBuf::from(dir);
        self.library = LibraryStore::load_from_dir(&path);
        self.overlay = OverlayState::load_from_dir(&path);
        self.storage_dir = Some(path);
        self.console_selections.clear();
        self.sync_visible_selection();
        self.clamp_scroll();
    }

    pub(super) fn set_has_prod_keys(&mut self, present: bool) {
        self.has_prod_keys = present;
    }

    pub(super) fn has_prod_keys(&self) -> bool {
        self.has_prod_keys
    }

    pub(super) fn games(&self) -> &[GameEntry] {
        self.library.games()
    }

    pub(super) fn selected_game_index(&self) -> Option<usize> {
        let selected_raw = self.library.selected_index()?;
        self.visible_game_indices()
            .into_iter()
            .position(|idx| idx == selected_raw)
    }

    pub(super) fn scroll_offset(&self) -> usize {
        self.scroll_offset
    }

    pub(super) fn overlay_mut(&mut self) -> &mut OverlayState {
        &mut self.overlay
    }

    pub(super) fn view(&self) -> ViewMode {
        self.view
    }

    pub(super) fn toggle_view(&mut self) {
        if !self.is_idle() {
            return;
        }
        self.view = match self.view {
            ViewMode::Library => ViewMode::Settings,
            ViewMode::Settings => ViewMode::Library,
        };
    }

    pub(super) fn settings(&self) -> SettingsSnapshot {
        self.settings
    }

    pub(super) fn settings_mut(&mut self) -> &mut SettingsSnapshot {
        &mut self.settings
    }

    pub(super) fn driver_name(&self) -> &CStr {
        &self.driver_name
    }

    pub(super) fn set_settings(&mut self, snapshot: SettingsSnapshot, driver_name: Option<&str>) {
        self.settings = snapshot.normalized();
        if let Some(name) = driver_name.map(str::trim).filter(|s| !s.is_empty())
            && let Ok(cstr) = CString::new(name)
        {
            self.driver_name = cstr;
        } else {
            self.driver_name = c"System default".to_owned();
        }
    }

    pub(super) fn begin_selection(&mut self) -> bool {
        self.begin_picker(PickerKind::GameFile)
    }

    pub(super) fn begin_folder_selection(&mut self) -> bool {
        self.begin_picker(PickerKind::GameFolder)
    }

    pub(super) fn begin_keys_selection(&mut self) -> bool {
        self.begin_picker(PickerKind::KeysFile)
    }

    pub(super) fn begin_driver_selection(&mut self) -> bool {
        self.begin_picker(PickerKind::DriverFile)
    }

    pub(super) fn selection_result(&mut self, result: SelectionResult<'_>) {
        if !matches!(self.activity, Activity::Picking(PickerKind::GameFile)) {
            return;
        }
        let message = match result {
            SelectionResult::Cancelled => Cow::Borrowed(c"Document selection cancelled."),
            SelectionResult::Selected(uri) => {
                if let Some(uri) = uri.filter(|uri| is_document_uri(uri)) {
                    self.document = CString::new(uri).ok();
                    status_with_detail("Selected: ", uri).into()
                } else {
                    Cow::Borrowed(c"The picker returned an invalid Android document URI.")
                }
            }
            SelectionResult::Failed(detail) => detail
                .map(str::trim)
                .filter(|detail| !detail.is_empty())
                .map_or(Cow::Borrowed(c"The Android document picker failed."), |detail| {
                    status_with_detail("Document picker failed: ", detail).into()
                }),
        };
        self.activity = Activity::Message(message);
    }

    pub(super) fn folder_scan_result(
        &mut self,
        tree_uri: Option<&str>,
        games: Vec<(String, String, u64)>,
        error: Option<&str>,
    ) {
        if !matches!(self.activity, Activity::Picking(PickerKind::GameFolder)) {
            return;
        }
        let message = if let Some(err) = error.map(str::trim).filter(|err| !err.is_empty()) {
            status_with_detail("Folder scan failed: ", err).into()
        } else if let Some(tree_uri) = tree_uri.filter(|uri| is_document_uri(uri)) {
            self.library.add_folder(tree_uri);
            let scanned = self.library.upsert_games(games);
            self.sync_visible_selection();
            self.clamp_scroll();
            self.save_library();
            let detail = format!(
                "{scanned} scanned ({} total in library).",
                self.library.games().len()
            );
            status_with_detail("Folder added: ", &detail).into()
        } else if tree_uri.is_some() {
            Cow::Borrowed(c"The picker returned an invalid Android folder URI.")
        } else {
            Cow::Borrowed(c"Folder selection cancelled.")
        };
        self.activity = Activity::Message(message);
    }

    pub(super) fn keys_install_result(&mut self, error: Option<&str>, has_keys: bool) {
        if !matches!(self.activity, Activity::Picking(PickerKind::KeysFile)) {
            return;
        }
        self.has_prod_keys = has_keys;
        let message = if let Some(err) = error.map(str::trim).filter(|err| !err.is_empty()) {
            status_with_detail("Keys install: ", err).into()
        } else if has_keys {
            Cow::Borrowed(c"Keys installed (prod.keys ready).")
        } else {
            Cow::Borrowed(c"Keys file installed.")
        };
        self.activity = Activity::Message(message);
    }

    pub(super) fn driver_install_result(&mut self, error: Option<&str>, driver_name: Option<&str>) {
        if !matches!(self.activity, Activity::Picking(PickerKind::DriverFile)) {
            return;
        }
        let message = if let Some(err) = error.map(str::trim).filter(|err| !err.is_empty()) {
            status_with_detail("Driver install: ", err).into()
        } else {
            if let Some(name) = driver_name.map(str::trim).filter(|s| !s.is_empty())
                && let Ok(cstr) = CString::new(name)
            {
                self.driver_name = cstr;
            }
            Cow::Borrowed(c"Custom GPU driver installed.")
        };
        self.activity = Activity::Message(message);
    }

    pub(super) fn clear_driver(&mut self) {
        if !self.is_idle() {
            return;
        }
        self.driver_name = c"System default".to_owned();
        self.activity = Activity::Message(Cow::Borrowed(c"Reset GPU driver to System default."));
    }

    pub(super) fn select_game_index(&mut self, index: usize) -> bool {
        self.select_visible_game(index)
    }

    pub(super) fn scroll_games(&mut self, delta: isize) {
        if !self.is_idle() {
            return;
        }
        let max_offset = self.visible_game_count().saturating_sub(VISIBLE_GAME_ROWS);
        let next = if delta < 0 {
            self.scroll_offset.saturating_sub(delta.unsigned_abs())
        } else {
            self.scroll_offset.saturating_add(delta as usize)
        };
        self.scroll_offset = next.min(max_offset);
    }

    pub(super) fn can_launch(&self) -> bool {
        self.is_idle() && self.document.is_some()
    }

    pub(super) fn begin_launch(&mut self) -> Option<&CStr> {
        if !self.can_launch() {
            return None;
        }
        self.activity = Activity::Launching;
        self.document.as_deref()
    }

    pub(super) fn launch_result(&mut self, error: Option<&str>) {
        if matches!(self.activity, Activity::Launching) {
            self.activity = error.map_or(Activity::Running, |error| {
                Activity::Message(status_with_detail("Launch failed: ", error).into())
            });
        }
    }

    pub(super) fn session_ended(&mut self, error: Option<&str>) {
        if matches!(self.activity, Activity::Running) {
            let message = error.map_or(Cow::Borrowed(c"Emulation ended."), |error| {
                status_with_detail("Emulation ended: ", error).into()
            });
            self.activity = Activity::Message(message);
        }
    }

    pub(super) fn status(&self) -> &CStr {
        match &self.activity {
            Activity::Ready => c"Select a game file or add a library folder.",
            Activity::Picking(PickerKind::GameFile) => c"Waiting for the Android document picker...",
            Activity::Picking(PickerKind::GameFolder) => c"Waiting for the Android folder picker...",
            Activity::Picking(PickerKind::KeysFile) => c"Waiting for the Android keys picker...",
            Activity::Picking(PickerKind::DriverFile) => c"Waiting for the GPU driver picker...",
            Activity::Launching => c"Starting emulation...",
            Activity::Running => c"Emulation is running.",
            Activity::Message(message) => message,
        }
    }

    fn begin_picker(&mut self, kind: PickerKind) -> bool {
        if !self.is_idle() {
            return false;
        }
        self.activity = Activity::Picking(kind);
        true
    }

    pub(super) fn is_idle(&self) -> bool {
        !self.suspended && matches!(self.activity, Activity::Ready | Activity::Message(_))
    }

    pub(super) fn clamp_scroll(&mut self) {
        let max_offset = self.visible_game_count().saturating_sub(VISIBLE_GAME_ROWS);
        self.scroll_offset = self.scroll_offset.min(max_offset);
    }

    pub(super) fn save_library(&self) {
        if let Some(dir) = &self.storage_dir {
            let _ = self.library.save_to_dir(dir);
        }
    }
}

#[derive(Clone, Copy)]
pub(super) enum PickerKind {
    GameFile,
    GameFolder,
    KeysFile,
    DriverFile,
}

#[derive(Default)]
pub(super) enum Activity {
    #[default]
    Ready,
    Picking(PickerKind),
    Launching,
    Running,
    Message(Cow<'static, CStr>),
}

pub(super) enum SelectionResult<'a> {
    Cancelled,
    Selected(Option<&'a str>),
    Failed(Option<&'a str>),
}

pub(super) fn status_with_detail(prefix: &str, detail: &str) -> CString {
    let mut message = String::from(prefix);
    let mut chars = detail.chars();
    message.extend(chars.by_ref().take(MAX_STATUS_DETAIL).map(|c| match c {
        ' '..='~' => c,
        _ if c.is_whitespace() => ' ',
        _ => '?',
    }));
    if chars.next().is_some() {
        message.push_str("...");
    }
    CString::new(message).expect("display text contains only printable ASCII")
}

#[cfg(test)]
mod tests {
    use super::{App, SelectionResult};

    #[test]
    fn console_switching_filters_titles_and_restores_selection() {
        let mut app = App::default();
        assert!(app.begin_folder_selection());
        app.folder_scan_result(
            Some("content://p/tree/library"),
            vec![
                ("content://p/doc/advance-a".into(), "Amber".into(), 0),
                ("content://p/doc/advance-b".into(), "Bramble".into(), 0),
                ("content://p/doc/switch".into(), "Native".into(), 1),
            ],
            None,
        );
        app.classify_game("content://p/doc/advance-a", "game-boy-advance");
        app.classify_game("content://p/doc/advance-b", "game-boy-advance");
        app.classify_game("content://p/doc/switch", "switch-2");
        assert!(app.select_console(Some("game-boy-advance")));
        assert_eq!(app.visible_game_count(), 2);
        assert!(app.select_game_index(1));
        assert!(app.select_console(Some("switch-2")));
        assert_eq!(app.visible_game_count(), 1);
        assert!(app.select_console(Some("game-boy-advance")));
        assert_eq!(app.selected_game_index(), Some(1));
        assert_eq!(app.selected_title(), Some(c"Bramble"));
        assert!(app.select_console(Some("game-boy-color")));
        assert!(!app.can_launch());
    }

    #[test]
    fn a_pending_picker_cannot_be_opened_twice() {
        let mut app = App::default();
        assert!(app.begin_selection());
        assert!(!app.begin_selection());
        assert!(!app.begin_folder_selection());
        assert!(!app.begin_keys_selection());
        app.selection_result(SelectionResult::Cancelled);
        assert_eq!(app.status(), c"Document selection cancelled.");
        assert!(app.begin_selection());
    }

    #[test]
    fn a_picker_callback_during_backgrounding_survives_resume() {
        let mut app = App::default();
        assert!(app.begin_selection());
        app.set_suspended(true);
        let uri = "content://provider/document/game.nsp";
        app.selection_result(SelectionResult::Selected(Some(uri)));
        assert!(!app.begin_selection());
        app.set_suspended(false);
        assert_eq!(app.status(), c"Selected: content://provider/document/game.nsp");
        assert!(app.begin_selection());
    }

    #[test]
    fn callback_without_pending_request_cannot_replace_status() {
        let mut app = App::default();
        app.selection_result(SelectionResult::Selected(Some("content://provider/other")));
        assert_eq!(app.status(), c"Select a game file or add a library folder.");
    }

    #[test]
    fn invalid_document_uris_end_the_selection_with_an_error() {
        let mut app = App::default();
        for uri in [None, Some("file:///a"), Some("content:///a"), Some("content://p/\na")] {
            assert!(app.begin_selection());
            app.selection_result(SelectionResult::Selected(uri));
            assert_eq!(app.status(), c"The picker returned an invalid Android document URI.");
        }
    }

    #[test]
    fn picker_failures_are_readable_and_allow_retry() {
        let mut app = App::default();
        assert!(app.begin_selection());
        app.selection_result(SelectionResult::Failed(Some("Picker\nis unavailable")));
        assert_eq!(app.status(), c"Document picker failed: Picker is unavailable");
        assert!(app.begin_selection());
        app.selection_result(SelectionResult::Failed(None));
        assert_eq!(app.status(), c"The Android document picker failed.");
    }

    #[test]
    fn launch_requires_selection_and_retains_the_document_on_failure() {
        let mut app = App::default();
        let uri = "content://provider/document/game.nsp";
        assert!(!app.can_launch());
        assert!(app.begin_selection());
        app.selection_result(SelectionResult::Selected(Some(uri)));
        assert!(app.can_launch());
        assert_eq!(app.begin_launch(), Some(c"content://provider/document/game.nsp"));
        assert!(!app.can_launch());
        assert!(!app.begin_selection());
        app.launch_result(Some("Missing firmware"));
        assert_eq!(app.status(), c"Launch failed: Missing firmware");
        assert!(app.can_launch());
    }

    #[test]
    fn running_session_blocks_picker_until_it_stops() {
        let mut app = App::default();
        assert!(app.begin_selection());
        app.selection_result(SelectionResult::Selected(Some("content://provider/game")));
        assert!(app.begin_launch().is_some());
        app.launch_result(None);
        app.set_suspended(true);
        app.set_suspended(false);
        assert!(!app.can_launch());
        assert!(!app.begin_selection());
        app.session_ended(None);
        assert_eq!(app.status(), c"Emulation ended.");
        assert!(app.can_launch());
    }

    #[test]
    fn folder_scan_populates_library_and_allows_selecting_game() {
        let mut app = App::default();
        assert!(app.begin_folder_selection());
        app.folder_scan_result(
            Some("content://provider/tree/games"),
            vec![
                ("content://provider/doc/b".to_owned(), "Zelda".to_owned(), 2),
                ("content://provider/doc/a".to_owned(), "Mario".to_owned(), 1),
            ],
            None,
        );
        assert_eq!(app.games().len(), 2);
        assert_eq!(app.selected_game_index(), Some(0));
        assert!(app.can_launch());

        assert!(app.select_game_index(1));
        assert_eq!(app.status(), c"Selected: Zelda");
        assert_eq!(app.begin_launch(), Some(c"content://provider/doc/b"));
    }

    #[test]
    fn keys_install_updates_prod_keys_flag_and_status() {
        let mut app = App::default();
        assert!(!app.has_prod_keys());
        assert!(app.begin_keys_selection());
        app.keys_install_result(None, true);
        assert!(app.has_prod_keys());
        assert_eq!(app.status(), c"Keys installed (prod.keys ready).");
    }

    #[test]
    fn view_toggle_and_driver_install_update_state() {
        let mut app = App::default();
        assert_eq!(app.view(), super::ViewMode::Library);
        app.toggle_view();
        assert_eq!(app.view(), super::ViewMode::Settings);

        assert!(app.begin_driver_selection());
        app.driver_install_result(None, Some("turnip_v25.so"));
        assert_eq!(app.driver_name(), c"turnip_v25.so");
        assert_eq!(app.status(), c"Custom GPU driver installed.");

        app.clear_driver();
        assert_eq!(app.driver_name(), c"System default");
        assert_eq!(app.status(), c"Reset GPU driver to System default.");
    }
}
