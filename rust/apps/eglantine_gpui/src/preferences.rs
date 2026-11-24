// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::{
    fs, io::{self, Write}, path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};

const FILE_NAME: &str = "shell.json";

#[derive(Debug, PartialEq, Eq, Deserialize, Serialize)]
#[serde(default)]
pub struct Preferences {
    pub width: u16,
    pub height: u16,
    pub fullscreen: bool,
    pub selected: Option<PathBuf>,
}

impl Default for Preferences {
    fn default() -> Self {
        Self { width: 1280, height: 760, fullscreen: false, selected: None }
    }
}

impl Preferences {
    pub fn load(dir: &Path) -> Result<Self, String> {
        let path = dir.join(FILE_NAME);
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(Self::default()),
            Err(error) => return Err(format!("Couldn't read window preferences: {error}")),
        };
        let mut preferences: Self = serde_json::from_slice(&bytes)
            .map_err(|error| format!("Couldn't parse window preferences: {error}"))?;
        preferences.clamp_window();
        Ok(preferences)
    }

    pub fn save(&self, dir: &Path) -> Result<(), String> {
        self.write_atomic(dir).map_err(|error| format!("Couldn't save window preferences: {error}"))
    }

    fn write_atomic(&self, dir: &Path) -> io::Result<()> {
        fs::create_dir_all(dir)?;
        let mut file = tempfile::NamedTempFile::new_in(dir)?;
        serde_json::to_writer(file.as_file_mut(), self)?;
        file.as_file_mut().write_all(b"\n")?;
        file.as_file().sync_all()?;
        file.persist(dir.join(FILE_NAME)).map_err(|error| error.error)?;
        Ok(())
    }

    pub fn clamp_window(&mut self) {
        let defaults = Self::default();
        if !(630..=8192).contains(&self.width) {
            self.width = defaults.width;
        }
        if !(450..=8192).contains(&self.height) {
            self.height = defaults.height;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stores_window_bounds_and_selected_title() {
        let dir = tempfile::tempdir().unwrap();
        let mut preferences = Preferences::default();
        preferences.width = 1440;
        preferences.height = 810;
        preferences.fullscreen = true;
        preferences.selected = Some(PathBuf::from("Library/Métro.nsp"));
        preferences.save(dir.path()).unwrap();
        let saved = Preferences::load(dir.path()).unwrap();
        assert_eq!(saved, preferences);
    }

    #[test]
    fn falls_back_from_bad_dimensions_and_keeps_valid_preferences() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(
            dir.path().join(FILE_NAME),
            r#"{"width":1,"height":9001,"fullscreen":true}"#,
        ).unwrap();
        let saved = Preferences::load(dir.path()).unwrap();
        assert_eq!((saved.width, saved.height), (1280, 760));
        assert!(saved.fullscreen);
    }
}
