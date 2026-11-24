// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::library::replace;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, Deserialize, Ord, PartialEq, PartialOrd, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum EmulatorSetting {
    Renderer,
    Resolution,
    Vsync,
    GameDisplay,
    Volume,
    CpuAccuracy,
    UserProfile,
    HomeKey,
    HomeButton,
}

type Choice = (&'static str, &'static str);

struct SettingInfo {
    section: &'static str,
    key: &'static str,
    choices: &'static [Choice],
    default: usize,
}

const SETTINGS: [EmulatorSetting; 9] = [
    EmulatorSetting::Renderer, EmulatorSetting::Resolution, EmulatorSetting::Vsync,
    EmulatorSetting::GameDisplay, EmulatorSetting::Volume, EmulatorSetting::CpuAccuracy,
    EmulatorSetting::UserProfile, EmulatorSetting::HomeKey, EmulatorSetting::HomeButton,
];

impl EmulatorSetting {
    fn info(self) -> SettingInfo {
        match self {
            Self::Renderer => SettingInfo { section: "Renderer", key: "backend", default: 0,
                choices: &[("1", "Vulkan"), ("0", "OpenGL")] },
            Self::Resolution => SettingInfo { section: "Renderer", key: "resolution_setup", default: 3,
                choices: &[("0", "0.25×"), ("1", "0.5×"), ("2", "0.75×"), ("3", "1×"),
                    ("4", "1.25×"), ("5", "1.5×"), ("6", "2×"), ("7", "3×"), ("8", "4×"),
                    ("9", "5×"), ("10", "6×"), ("11", "7×"), ("12", "8×")] },
            Self::Vsync => SettingInfo { section: "Renderer", key: "use_vsync", default: 2,
                choices: &[("0", "Off"), ("1", "Mailbox"), ("2", "On"), ("3", "Adaptive")] },
            Self::GameDisplay => SettingInfo { section: "GPUI", key: "game_fullscreen", default: 0,
                choices: &[("false", "Windowed"), ("true", "Fullscreen")] },
            Self::Volume => SettingInfo { section: "Audio", key: "volume", default: 4,
                choices: &[("0", "0%"), ("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%"),
                    ("125", "125%"), ("150", "150%"), ("175", "175%"), ("200", "200%")] },
            Self::CpuAccuracy => SettingInfo { section: "Cpu", key: "cpu_accuracy", default: 0,
                choices: &[("0", "Auto"), ("1", "Accurate"), ("2", "Unsafe"), ("3", "Paranoid"), ("4", "Debugging")] },
            Self::UserProfile => SettingInfo { section: "GPUI", key: "user_profile", default: 0,
                choices: &[("0", "Profile 1"), ("1", "Profile 2"), ("2", "Profile 3"), ("3", "Profile 4"),
                    ("4", "Profile 5"), ("5", "Profile 6"), ("6", "Profile 7"), ("7", "Profile 8")] },
            Self::HomeKey => SettingInfo { section: "GPUI", key: "home_key", default: 0,
                choices: &[("F10", "F10"), ("F9", "F9"), ("F8", "F8"), ("Home", "Home"), ("Pause", "Pause")] },
            Self::HomeButton => SettingInfo { section: "GPUI", key: "home_button", default: 0,
                choices: &[("guide", "Home / Guide"), ("back", "Back / Select"), ("start", "Start / Plus"),
                    ("leftstick", "Left Stick Click"), ("rightstick", "Right Stick Click"), ("misc1", "Capture / Misc")] },
        }
    }
}

pub struct EmulatorConfig {
    path: PathBuf,
    ini: Ini,
}

type Overrides = BTreeMap<PathBuf, BTreeMap<EmulatorSetting, String>>;

impl EmulatorConfig {
    pub fn load(dir: &Path) -> Result<Self, String> {
        let path = dir.join("emulator.ini");
        let ini = match read_optional(&path)? {
            Some(ini) => ini,
            None => {
                let mut imported = Ini::default();
                if let Some(qt) = read_optional(&dir.with_file_name("qt-config.ini"))? {
                    for setting in SETTINGS {
                        let info = setting.info();
                        if info.section == "GPUI" { continue; }
                        if let Some(value) = qt.setting(&info).filter(|value| value.parse::<u8>().is_ok()) {
                            imported.set_setting(&info, value);
                        }
                    }
                }
                replace(&path, imported.as_string().as_bytes())?;
                imported
            }
        };
        Ok(Self { path, ini })
    }

    pub fn value(&self, setting: EmulatorSetting) -> &'static str {
        self.value_index(setting).map_or("Custom", |index| setting.info().choices[index].1)
    }

    pub fn cycle(&mut self, setting: EmulatorSetting, direction: i32) -> Result<(), String> {
        if direction == 0 { return Ok(()); }
        // SDL may have written settings since construction. Preserve every unedited value.
        let mut ini = read_optional(&self.path)?.unwrap_or_default();
        let info = setting.info();
        let index = value_index(&ini, &info).unwrap_or(info.default) as i32;
        let next = (index + direction.signum()).rem_euclid(info.choices.len() as i32) as usize;
        ini.set_setting(&info, info.choices[next].0);
        replace(&self.path, ini.as_string().as_bytes())?;
        self.ini = ini;
        Ok(())
    }

    pub fn path(&self) -> &Path { &self.path }

    pub fn game_fullscreen(&self) -> bool {
        self.value_index(EmulatorSetting::GameDisplay) == Some(1)
    }

    pub fn home_key(&self) -> &'static str {
        self.code_value(EmulatorSetting::HomeKey)
    }

    pub fn home_button(&self) -> &'static str {
        self.code_value(EmulatorSetting::HomeButton)
    }

    pub fn user_profile(&self) -> u8 {
        self.code_value(EmulatorSetting::UserProfile).parse().unwrap_or(0)
    }

    fn code_value(&self, setting: EmulatorSetting) -> &'static str {
        let info = setting.info();
        self.value_index(setting)
            .and_then(|index| info.choices.get(index))
            .map_or(info.choices[info.default].0, |(code, _)| *code)
    }

    pub fn game_value(&self, game: &Path, setting: EmulatorSetting) -> Result<Option<&'static str>, String> {
        Ok(self.read_overrides()?.get(game).and_then(|settings| settings.get(&setting)).map(|value| {
            setting.info().choices.iter().find(|(code, _)| code == value).map_or("Custom", |(_, label)| *label)
        }))
    }

    pub fn cycle_game(&self, game: &Path, setting: EmulatorSetting, direction: i32) -> Result<(), String> {
        if direction == 0 {
            return Ok(());
        }
        let mut overrides = self.read_overrides()?;
        let settings = overrides.entry(game.to_owned()).or_default();
        let info = setting.info();
        let current = settings.get(&setting).and_then(|value| info.choices.iter().position(|(code, _)| code == value))
            .or_else(|| self.value_index(setting)).unwrap_or(info.default);
        let selected = (current as i32 + direction.signum()).rem_euclid(info.choices.len() as i32) as usize;
        settings.insert(setting, info.choices[selected].0.into());
        self.save_overrides(&overrides)
    }

    pub fn clear_game(&self, game: &Path, setting: EmulatorSetting) -> Result<(), String> {
        let mut overrides = self.read_overrides()?;
        if let Some(settings) = overrides.get_mut(game) {
            settings.remove(&setting);
            if settings.is_empty() {
                overrides.remove(game);
            }
            self.save_overrides(&overrides)?;
        }
        Ok(())
    }

    pub fn session_file(&self, game: &Path) -> Result<(bool, Option<tempfile::TempPath>), String> {
        let overrides = self.read_overrides()?;
        let Some(settings) = overrides.get(game) else { return Ok((self.game_fullscreen(), None)); };
        let mut ini = read_optional(&self.path)?.unwrap_or_default();
        for (setting, value) in settings {
            ini.set_setting(&setting.info(), value);
        }
        let fullscreen = value_index(&ini, &EmulatorSetting::GameDisplay.info()) == Some(1);
        let dir = self.path.parent().ok_or("Config path has no parent directory")?;
        let write_session = || -> io::Result<tempfile::TempPath> {
            let mut file = tempfile::Builder::new().prefix("game-").suffix(".ini").tempfile_in(dir)?;
            file.write_all(ini.as_string().as_bytes())?;
            file.as_file().sync_all()?;
            Ok(file.into_temp_path())
        };
        let file = write_session().map_err(|error| format!("Could not prepare settings for {}: {error}", game.display()))?;
        Ok((fullscreen, Some(file)))
    }

    fn override_path(&self) -> PathBuf { self.path.with_file_name("game-overrides.json") }

    fn read_overrides(&self) -> Result<Overrides, String> {
        let file = self.override_path();
        let overrides: Overrides = match fs::read(&file) {
            Ok(bytes) => serde_json::from_slice(&bytes).map_err(|error| format!("Invalid {}: {error}", file.display()))?,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(BTreeMap::new()),
            Err(error) => return Err(format!("Could not read {}: {error}", file.display())),
        };
        for settings in overrides.values() {
            for (setting, value) in settings {
                if !setting.info().choices.iter().any(|(code, _)| code == value) {
                    return Err(format!("Invalid per-game setting value for {setting:?}"));
                }
            }
        }
        Ok(overrides)
    }

    fn save_overrides(&self, overrides: &Overrides) -> Result<(), String> {
        let content = serde_json::to_vec_pretty(overrides).map_err(|error| format!("Could not encode game settings: {error}"))?;
        replace(&self.override_path(), &content)
    }

    fn value_index(&self, setting: EmulatorSetting) -> Option<usize> {
        value_index(&self.ini, &setting.info())
    }
}

fn value_index(ini: &Ini, info: &SettingInfo) -> Option<usize> {
    match ini.setting(info) {
        Some(value) => info.choices.iter().position(|(code, _)| *code == value),
        None => Some(info.default),
    }
}

fn read_optional(path: &Path) -> Result<Option<Ini>, String> {
    match fs::read_to_string(path) {
        Ok(content) => Ok(Some(Ini(content.trim_start_matches('\u{feff}').lines().map(str::to_owned).collect()))),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(format!("Could not read {}: {error}", path.display())),
    }
}

#[derive(Default)]
struct Ini(Vec<String>);

impl Ini {
    fn setting(&self, info: &SettingInfo) -> Option<&str> {
        let default = self.get(info.section, &format!("{}\\default", info.key));
        if info.section != "GPUI" && !default.is_some_and(|value| matches!(value, "false" | "0")) {
            return None;
        }
        self.get(info.section, info.key)
    }

    fn set_setting(&mut self, info: &SettingInfo, value: &str) {
        self.set(info.section, info.key, value);
        if info.section != "GPUI" {
            self.set(info.section, &format!("{}\\default", info.key), "false");
        }
    }

    fn get(&self, section: &str, key: &str) -> Option<&str> {
        let range = self.range(section)?;
        self.0[range].iter().filter_map(|line| line.split_once('=')).find_map(|(name, value)| {
            name.trim().eq_ignore_ascii_case(key).then(|| value.trim().trim_matches('"'))
        })
    }

    fn set(&mut self, section: &str, key: &str, value: &str) {
        let line = format!("{key}={value}");
        if let Some(range) = self.range(section) {
            let offset = self.0[range.clone()].iter().position(|line| {
                line.split_once('=').is_some_and(|(name, _)| name.trim().eq_ignore_ascii_case(key))
            });
            if let Some(offset) = offset { self.0[range.start + offset] = line; }
            else { self.0.insert(range.end, line); }
        } else {
            if !self.0.is_empty() { self.0.push(String::new()); }
            self.0.extend([format!("[{section}]"), line]);
        }
    }

    fn range(&self, section: &str) -> Option<std::ops::Range<usize>> {
        let start = self.0.iter().position(|line| {
            line.trim().strip_prefix('[').and_then(|line| line.strip_suffix(']'))
                .is_some_and(|name| name.eq_ignore_ascii_case(section))
        })? + 1;
        let end = self.0[start..].iter().position(|line| line.trim().starts_with('['))
            .map_or(self.0.len(), |end| start + end);
        Some(start..end)
    }

    fn as_string(&self) -> String {
        let mut text = self.0.join("\n");
        text.push('\n');
        text
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn imports_shared_values_once_and_preserves_both_config_files() {
        let scratch = tempfile::tempdir().unwrap();
        let qt = scratch.path().join("qt-config.ini");
        let source = "[Renderer]\nbackend=0\nbackend\\default=false\nuse_vsync=0\nuse_vsync\\default=true\n[Controls]\nplayer_0_a=qt-only\n";
        fs::write(&qt, source).unwrap();
        let dir = scratch.path().join("gpui");
        let mut config = EmulatorConfig::load(&dir).unwrap();
        assert_eq!(config.value(EmulatorSetting::Renderer), "OpenGL");
        assert_eq!(config.value(EmulatorSetting::Vsync), "On");
        let mut updated = fs::read_to_string(config.path()).unwrap();
        assert!(!updated.contains("qt-only"));
        updated.push_str("\n[Unknown]\nvalue=preserved\n");
        fs::write(config.path(), updated).unwrap();
        config.cycle(EmulatorSetting::Renderer, 1).unwrap();
        assert_eq!(EmulatorConfig::load(&dir).unwrap().value(EmulatorSetting::Renderer), "Vulkan");
        assert!(fs::read_to_string(config.path()).unwrap().contains("value=preserved"));
        assert_eq!(fs::read_to_string(&qt).unwrap(), source);
    }

    #[test]
    fn title_override_only_changes_the_owned_session_configuration() {
        let scratch = tempfile::tempdir().unwrap();
        let config = EmulatorConfig::load(scratch.path()).unwrap();
        let game = Path::new("synthetic.nro");
        assert!(config.session_file(game).unwrap().1.is_none());
        config.cycle_game(game, EmulatorSetting::GameDisplay, 1).unwrap();
        config.cycle_game(game, EmulatorSetting::Vsync, -1).unwrap();
        let (fullscreen, file) = config.session_file(game).unwrap();
        let file = file.unwrap();
        assert!(fullscreen);
        assert_eq!(config.game_value(game, EmulatorSetting::Vsync).unwrap(), Some("Mailbox"));
        assert_eq!(config.value(EmulatorSetting::Vsync), "On");
        let session = read_optional(&file).unwrap().unwrap();
        assert_eq!(session.setting(&EmulatorSetting::Vsync.info()), Some("1"));
        config.clear_game(game, EmulatorSetting::Vsync).unwrap();
        assert_eq!(config.game_value(game, EmulatorSetting::Vsync).unwrap(), None);
    }

    #[test]
    fn home_fallback_settings_persist_and_keep_defaults() {
        let scratch = tempfile::tempdir().unwrap();
        let mut config = EmulatorConfig::load(scratch.path()).unwrap();
        assert_eq!(config.home_key(), "F10");
        assert_eq!(config.home_button(), "guide");
        config.cycle(EmulatorSetting::HomeKey, 1).unwrap();
        config.cycle(EmulatorSetting::HomeButton, 1).unwrap();
        let reloaded = EmulatorConfig::load(scratch.path()).unwrap();
        assert_eq!(reloaded.home_key(), "F9");
        assert_eq!(reloaded.home_button(), "back");
        assert_eq!(reloaded.value(EmulatorSetting::HomeButton), "Back / Select");
    }

    #[test]
    fn user_profile_defaults_to_zero_and_persists() {
        let scratch = tempfile::tempdir().unwrap();
        let mut config = EmulatorConfig::load(scratch.path()).unwrap();
        assert_eq!(config.user_profile(), 0);
        config.cycle(EmulatorSetting::UserProfile, 2).unwrap();
        let reloaded = EmulatorConfig::load(scratch.path()).unwrap();
        assert_eq!(reloaded.user_profile(), 1);
        assert_eq!(reloaded.value(EmulatorSetting::UserProfile), "Profile 2");
    }
}
