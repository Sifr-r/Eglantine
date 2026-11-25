// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::ffi::{CStr, CString};
use std::fs;
use std::io;
use std::path::Path;

const LIBRARY_FILE: &str = "android_library.tsv";
const LIBRARY_TEMP_FILE: &str = "android_library.tsv.tmp";

pub(super) struct GameEntry {
    pub(super) uri: CString,
    pub(super) title: CString,
    pub(super) program_id: u64,
}

#[derive(Default)]
pub(super) struct LibraryStore {
    folders: Vec<String>,
    games: Vec<GameEntry>,
    selected_index: Option<usize>,
}

impl LibraryStore {
    #[cfg(test)]
    pub(super) fn folders(&self) -> &[String] {
        &self.folders
    }

    pub(super) fn games(&self) -> &[GameEntry] {
        &self.games
    }

    pub(super) fn selected_index(&self) -> Option<usize> {
        self.selected_index
    }

    pub(super) fn selected_game(&self) -> Option<&GameEntry> {
        self.selected_index.and_then(|idx| self.games.get(idx))
    }

    pub(super) fn add_folder(&mut self, uri: &str) -> bool {
        if !is_document_uri(uri) || self.folders.iter().any(|existing| existing == uri) {
            return false;
        }
        self.folders.push(uri.to_owned());
        true
    }

    pub(super) fn upsert_games(&mut self, incoming: Vec<(String, String, u64)>) -> usize {
        let selected_uri = self
            .selected_game()
            .map(|entry| entry.uri.to_string_lossy().into_owned());
        let mut touched = 0usize;

        for (uri, raw_title, program_id) in incoming {
            if !is_document_uri(&uri) {
                continue;
            }
            let Ok(uri_cstr) = CString::new(uri.as_str()) else {
                continue;
            };
            let cleaned_title = sanitize_single_line(&raw_title);
            let title_text = if cleaned_title.is_empty() {
                "Untitled game".to_owned()
            } else {
                cleaned_title
            };
            let Ok(title_cstr) = CString::new(title_text) else {
                continue;
            };

            if let Some(existing) = self
                .games
                .iter_mut()
                .find(|entry| entry.uri.as_c_str() == uri_cstr.as_c_str())
            {
                existing.title = title_cstr;
                existing.program_id = program_id;
            } else {
                self.games.push(GameEntry {
                    uri: uri_cstr,
                    title: title_cstr,
                    program_id,
                });
            }
            touched += 1;
        }

        self.sort_and_restore_selection(selected_uri.as_deref());
        touched
    }

    pub(super) fn select(&mut self, index: usize) -> Option<&CStr> {
        if index < self.games.len() {
            self.selected_index = Some(index);
            Some(self.games[index].uri.as_c_str())
        } else {
            None
        }
    }

    pub(super) fn load_from_dir(dir: &Path) -> Self {
        let Ok(contents) = fs::read_to_string(dir.join(LIBRARY_FILE)) else {
            return Self::default();
        };
        let mut store = Self::default();
        let mut incoming = Vec::new();
        let mut selected_uri = None;

        for line in contents.lines() {
            let mut parts = line.split('\t');
            match parts.next() {
                Some("folder") => {
                    if let Some(uri) = parts.next()
                        && parts.next().is_none()
                    {
                        store.add_folder(uri);
                    }
                }
                Some("game") => {
                    if let (Some(hex_id), Some(title), Some(uri), None) =
                        (parts.next(), parts.next(), parts.next(), parts.next())
                        && let Ok(program_id) = u64::from_str_radix(hex_id, 16)
                    {
                        incoming.push((uri.to_owned(), title.to_owned(), program_id));
                    }
                }
                Some("selected") => {
                    if let Some(uri) = parts.next()
                        && parts.next().is_none()
                        && is_document_uri(uri)
                    {
                        selected_uri = Some(uri.to_owned());
                    }
                }
                _ => {}
            }
        }

        store.upsert_games(incoming);
        if let Some(uri) = selected_uri.as_deref() {
            store.sort_and_restore_selection(Some(uri));
        }
        store
    }

    pub(super) fn save_to_dir(&self, dir: &Path) -> io::Result<()> {
        fs::create_dir_all(dir)?;
        let mut out = String::new();
        for folder in &self.folders {
            out.push_str("folder\t");
            out.push_str(&sanitize_single_line(folder));
            out.push('\n');
        }
        for game in &self.games {
            out.push_str("game\t");
            out.push_str(&format!("{:016x}", game.program_id));
            out.push('\t');
            out.push_str(&sanitize_single_line(&game.title.to_string_lossy()));
            out.push('\t');
            out.push_str(&sanitize_single_line(&game.uri.to_string_lossy()));
            out.push('\n');
        }
        if let Some(selected) = self.selected_game() {
            out.push_str("selected\t");
            out.push_str(&sanitize_single_line(&selected.uri.to_string_lossy()));
            out.push('\n');
        }

        let temp_path = dir.join(LIBRARY_TEMP_FILE);
        let final_path = dir.join(LIBRARY_FILE);
        fs::write(&temp_path, out.as_bytes())?;
        fs::rename(temp_path, final_path)
    }

    fn sort_and_restore_selection(&mut self, selected_uri: Option<&str>) {
        self.games.sort_by(|a, b| {
            a.title
                .to_string_lossy()
                .to_ascii_lowercase()
                .cmp(&b.title.to_string_lossy().to_ascii_lowercase())
                .then_with(|| a.uri.as_c_str().cmp(b.uri.as_c_str()))
        });
        if let Some(uri) = selected_uri
            && let Some(idx) = self
                .games
                .iter()
                .position(|entry| entry.uri.to_string_lossy() == uri)
        {
            self.selected_index = Some(idx);
        } else if self.games.is_empty() {
            self.selected_index = None;
        } else {
            self.selected_index = Some(0);
        }
    }
}

pub(super) fn is_document_uri(uri: &str) -> bool {
    uri.strip_prefix("content://").is_some_and(|rest| {
        let authority_end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
        authority_end != 0 && !rest.chars().any(|c| c.is_control() || c.is_whitespace())
    })
}

fn sanitize_single_line(text: &str) -> String {
    text.chars()
        .map(|ch| match ch {
            '\t' | '\r' | '\n' => ' ',
            c if c.is_control() => ' ',
            c => c,
        })
        .collect::<String>()
        .trim()
        .to_owned()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn unique_temp_dir(label: &str) -> std::path::PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("eglantine_android_{label}_{}_{}", std::process::id(), nanos))
    }

    #[test]
    fn folders_reject_invalid_uris_and_deduplicate() {
        let mut store = LibraryStore::default();
        assert!(!store.add_folder("file:///sdcard/roms"));
        assert!(store.add_folder("content://com.android.externalstorage.documents/tree/primary%3AGames"));
        assert!(!store.add_folder("content://com.android.externalstorage.documents/tree/primary%3AGames"));
        assert_eq!(store.folders().len(), 1);
    }

    #[test]
    fn upsert_sorts_deduplicates_and_preserves_selection() {
        let mut store = LibraryStore::default();
        let added = store.upsert_games(vec![
            ("content://p/doc/b".to_owned(), "Zelda".to_owned(), 2),
            ("content://p/doc/a".to_owned(), "Animal Crossing".to_owned(), 1),
            ("bad://ignored".to_owned(), "Ignored".to_owned(), 3),
        ]);
        assert_eq!(added, 2);
        assert_eq!(store.games()[0].title.as_c_str(), c"Animal Crossing");
        assert_eq!(store.games()[1].title.as_c_str(), c"Zelda");
        assert_eq!(store.selected_index(), Some(0));

        assert_eq!(store.select(1), Some(c"content://p/doc/b"));
        store.upsert_games(vec![
            ("content://p/doc/b".to_owned(), "Zelda Updated".to_owned(), 2),
            ("content://p/doc/c".to_owned(), "Metroid".to_owned(), 4),
        ]);
        assert_eq!(store.games().len(), 3);
        assert_eq!(store.selected_game().unwrap().title.as_c_str(), c"Zelda Updated");
    }

    #[test]
    fn persistence_round_trips_and_ignores_corrupt_lines() {
        let dir = unique_temp_dir("lib_roundtrip");
        let mut store = LibraryStore::default();
        store.add_folder("content://provider/tree/games");
        store.upsert_games(vec![
            ("content://provider/doc/1".to_owned(), "Alpha\tGame".to_owned(), 0x0100_0000_0001_0000),
            ("content://provider/doc/2".to_owned(), "Beta".to_owned(), 0x0100_0000_0002_0000),
        ]);
        store.select(1);
        store.save_to_dir(&dir).unwrap();

        let tsv_path = dir.join(LIBRARY_FILE);
        let mut raw = fs::read_to_string(&tsv_path).unwrap();
        raw.push_str("corrupt\tline\n");
        raw.push_str("game\tnot_hex\tBad\tcontent://provider/doc/bad\n");
        fs::write(&tsv_path, raw).unwrap();

        let reloaded = LibraryStore::load_from_dir(&dir);
        assert_eq!(reloaded.folders(), &["content://provider/tree/games"]);
        assert_eq!(reloaded.games().len(), 2);
        assert_eq!(reloaded.games()[0].title.as_c_str(), c"Alpha Game");
        assert_eq!(reloaded.selected_game().unwrap().uri.as_c_str(), c"content://provider/doc/2");

        let _ = fs::remove_dir_all(&dir);
    }
}
