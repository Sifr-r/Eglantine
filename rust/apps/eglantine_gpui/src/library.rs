// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::host_tool::{self, Event};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::OsStr;
use std::fs;
use std::hash::{DefaultHasher, Hash, Hasher};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

#[derive(Clone, Debug)]
pub struct LibraryEntry {
    pub title: String,
    pub path: PathBuf,
    pub cover: Option<PathBuf>,
    pub program_id: Option<String>,
}

#[derive(Clone, Default, Deserialize, Serialize)]
struct Preferences {
    folders: BTreeMap<PathBuf, bool>,
    played: BTreeMap<PathBuf, u64>,
}

pub struct Library {
    file: PathBuf,
    preferences: Preferences,
    entries: Vec<LibraryEntry>,
}

pub struct LibraryScan {
    folders: BTreeMap<PathBuf, bool>,
    cache: PathBuf,
}

pub fn emulator_data_dir() -> PathBuf {
    let portable = if cfg!(windows) {
        std::env::current_exe().ok().map(|path| path.with_file_name("user"))
    } else {
        std::env::current_dir().ok().map(|path| path.join("user"))
    };
    if let Some(path) = portable.filter(|path| path.is_dir()) {
        return path;
    }
    if cfg!(windows) {
        env_path("APPDATA").unwrap_or_else(|| PathBuf::from(".")).join("eglantine")
    } else {
        env_path("XDG_DATA_HOME").or_else(|| env_path("HOME").map(|path| path.join(".local/share")))
            .unwrap_or_else(|| PathBuf::from(".")).join("eglantine")
    }
}

pub fn data_dir() -> PathBuf {
    let data = emulator_data_dir();
    if cfg!(windows) || data.ends_with("user") {
        return data.join("config/gpui");
    }
    env_path("XDG_CONFIG_HOME").or_else(|| env_path("HOME").map(|path| path.join(".config")))
        .map_or_else(|| data.join("config/gpui"), |path| path.join("eglantine/gpui"))
}

fn env_path(variable: &str) -> Option<PathBuf> {
    std::env::var_os(variable).filter(|value| !value.is_empty()).map(PathBuf::from)
}

pub(crate) fn replace(path: &Path, content: &[u8]) -> Result<(), String> {
    let parent = path.parent().ok_or("Config path has no parent directory")?;
    let save = || -> io::Result<()> {
        fs::create_dir_all(parent)?;
        let mut file = tempfile::NamedTempFile::new_in(parent)?;
        file.write_all(content)?;
        file.as_file().sync_all()?;
        file.persist(path).map_err(|error| error.error)?;
        Ok(())
    };
    save().map_err(|error| format!("Could not save {}: {error}", path.display()))
}

impl Library {
    pub fn open(dir: PathBuf) -> Result<Self, String> {
        let file = dir.join("library.json");
        let preferences = match fs::read(&file) {
            Ok(bytes) => serde_json::from_slice(&bytes).map_err(|error| format!("Invalid {}: {error}", file.display()))?,
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                let mut preferences = Preferences::default();
                let qt = dir.with_file_name("qt-config.ini");
                match fs::read_to_string(&qt) {
                    Ok(ini) => preferences.folders = qt_folders(&ini),
                    Err(error) if error.kind() == io::ErrorKind::NotFound => {},
                    Err(error) => return Err(format!("Could not read {}: {error}", qt.display())),
                }
                Self::save(&file, &preferences)?;
                preferences
            }
            Err(error) => return Err(format!("Could not read {}: {error}", file.display())),
        };
        let mut library = Self { file, preferences, entries: Vec::new() };
        library.refresh()?;
        Ok(library)
    }

    pub fn entries(&self) -> &[LibraryEntry] { &self.entries }

    pub fn add_folder(&mut self, folder: PathBuf) -> Result<(), String> {
        let directory = folder.canonicalize().map_err(|error| format!("Could not open {}: {error}", folder.display()))?;
        if !directory.is_dir() {
            return Err(format!("{} is not a directory", folder.display()));
        }
        let mut next = self.preferences.clone();
        next.folders.entry(directory).or_insert(true);
        self.update(next)
    }

    pub fn refresh(&mut self) -> Result<(), String> {
        self.entries = scan(&self.preferences)?;
        Ok(())
    }

    pub fn metadata_request(&self) -> LibraryScan {
        LibraryScan {
            folders: self.preferences.folders.clone(),
            cache: self.file.with_file_name("covers"),
        }
    }

    pub fn apply_metadata(&mut self, metadata: Vec<LibraryEntry>) {
        for entry in metadata {
            if let Some(existing) = self.entries.iter_mut().find(|existing| existing.path == entry.path) {
                existing.title = entry.title;
                existing.program_id = entry.program_id;
                if existing.cover.is_none() {
                    existing.cover = entry.cover;
                }
            } else {
                self.entries.push(entry);
            }
        }
        sort_entries(&mut self.entries, &self.preferences.played);
    }

    pub fn note_played(&mut self, path: &Path) -> Result<(), String> {
        let mut next = self.preferences.clone();
        let timestamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis();
        next.played.insert(path.to_owned(), timestamp.min(u128::from(u64::MAX)) as u64);
        Self::save(&self.file, &next)?;
        self.preferences = next;
        sort_entries(&mut self.entries, &self.preferences.played);
        Ok(())
    }

    fn update(&mut self, next: Preferences) -> Result<(), String> {
        let entries = scan(&next)?;
        Self::save(&self.file, &next)?;
        self.preferences = next;
        self.entries = entries;
        Ok(())
    }

    fn save(file: &Path, preferences: &Preferences) -> Result<(), String> {
        let bytes = serde_json::to_vec_pretty(preferences).map_err(|error| format!("Could not encode library: {error}"))?;
        replace(file, &bytes)
    }
}

impl LibraryScan {
    pub fn read(self) -> (Vec<LibraryEntry>, Option<String>) {
        let mut entries = BTreeMap::new();
        let mut failures = Vec::new();
        for (directory, deep) in self.folders {
            if !directory.is_dir() {
                continue;
            }
            let mut args = vec![OsStr::new("--gpui-library"), directory.as_os_str()];
            if !deep {
                args.push(OsStr::new("--shallow"));
            }
            let mut done = false;
            let mut scan_errors = 0;
            let result = host_tool::run(&args, Duration::from_secs(120), |event| {
                match event {
                    Event::Ready => {}
                    Event::LibraryDone { scan_errors: errors } => {
                        done = true;
                        scan_errors = errors;
                    }
                    Event::Game { path, title, program_id, icon_hex } => {
                        let path = path.canonicalize().unwrap_or(path);
                        let cover = icon_hex.as_deref().and_then(|icon| {
                            match cache_cover(&self.cache, &path, icon) {
                                Ok(path) => Some(path),
                                Err(error) => {
                                    failures.push(error);
                                    None
                                }
                            }
                        });
                        entries.insert(path.clone(), LibraryEntry { title, path, cover, program_id });
                    }
                    _ => return Err("The emulator returned an unexpected library event".into()),
                }
                Ok(())
            });
            let error = match result {
                Err(error) => Some(error),
                Ok(()) if !done => Some("The emulator did not finish the scan".into()),
                Ok(()) if scan_errors > 0 => Some(format!("Couldn't read {scan_errors} entries")),
                Ok(()) => None,
            };
            if let Some(error) = error {
                failures.push(format!("{}: {error}", directory.display()));
            }
        }
        let error = failures.into_iter().next();
        (entries.into_values().collect(), error)
    }
}

fn cache_cover(cache: &Path, game: &Path, hex: &str) -> Result<PathBuf, String> {
    let invalid = || format!("Invalid embedded cover for {}", game.display());
    if hex.len() > 512 * 1024 || !hex.len().is_multiple_of(2) || !hex.is_ascii() {
        return Err(invalid());
    }
    let bytes = (0..hex.len()).step_by(2).map(|index| u8::from_str_radix(&hex[index..index + 2], 16))
        .collect::<Result<Vec<_>, _>>().map_err(|_| invalid())?;
    let ext = match image::guess_format(&bytes) {
        Ok(image::ImageFormat::Jpeg) => "jpg",
        Ok(image::ImageFormat::Png) => "png",
        Ok(image::ImageFormat::WebP) => "webp",
        _ => return Err(invalid()),
    };
    let mut reader = image::ImageReader::new(io::Cursor::new(&bytes));
    reader.set_format(image::ImageFormat::from_extension(ext).ok_or_else(invalid)?);
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(1024);
    limits.max_image_height = Some(1024);
    limits.max_alloc = Some(8 * 1024 * 1024);
    reader.limits(limits);
    reader.decode().map_err(|_| invalid())?;
    let mut hash = DefaultHasher::new();
    game.hash(&mut hash);
    bytes.hash(&mut hash);
    let path = cache.join(format!("{:016x}.{ext}", hash.finish()));
    if !path.is_file() {
        replace(&path, &bytes)?;
    }
    Ok(path)
}

fn qt_folders(ini: &str) -> BTreeMap<PathBuf, bool> {
    let mut folders = BTreeMap::new();
    let mut paths = BTreeMap::<&str, (&str, bool)>::new();
    let mut section = "";
    for line in ini.lines() {
        let line = line.trim().trim_start_matches('\u{feff}');
        if let Some(name) = line.strip_prefix('[').and_then(|line| line.strip_suffix(']')) {
            section = name;
            continue;
        }
        if section != "Paths" { continue; }
        if let Some((key, value)) = line.split_once('=') {
            let Some((index, field)) = key.trim().strip_prefix("gamedirs\\").and_then(|key| key.split_once('\\')) else { continue };
            if index.parse::<usize>().is_err() { continue; }
            let entry = paths.entry(index).or_default();
            match field {
                "path" => entry.0 = value.trim().trim_matches('"'),
                "deep_scan" => entry.1 = matches!(value.trim(), "true" | "1"),
                _ => {},
            }
        }
    }
    for (path, deep) in paths.into_values() {
        let path = PathBuf::from(path);
        if path.is_absolute() { folders.insert(path, deep); }
    }
    folders
}

fn scan(preferences: &Preferences) -> Result<Vec<LibraryEntry>, String> {
    let mut pending: Vec<_> = preferences.folders.iter().map(|(path, deep)| (path.clone(), *deep)).collect();
    let mut seen = BTreeSet::new();
    let mut games = BTreeSet::new();
    while let Some((directory, deep)) = pending.pop() {
        let files = match fs::read_dir(&directory) {
            Ok(files) => files,
            Err(error) if matches!(error.kind(), io::ErrorKind::NotFound | io::ErrorKind::PermissionDenied) => continue,
            Err(error) => return Err(format!("Could not scan {}: {error}", directory.display())),
        };
        let directory = directory.canonicalize().map_err(|error| format!("Could not read {}: {error}", directory.display()))?;
        if !seen.insert((directory.clone(), deep)) { continue; }
        if directory.join("main").is_file() && directory.join("main.npdm").is_file() {
            games.insert(directory.join("main"));
            continue;
        }
        for entry in files {
            let entry = entry.map_err(|error| format!("Could not scan {}: {error}", directory.display()))?;
            let file_type = entry.file_type().map_err(|error| format!("Could not read {}: {error}", entry.path().display()))?;
            let path = directory.join(entry.file_name());
            if file_type.is_dir() && deep {
                if !entry.file_name().eq_ignore_ascii_case("romfs") { pending.push((path, true)); }
            } else if file_type.is_file() && supported(&path) {
                games.insert(path);
            }
        }
    }
    let mut entries: Vec<_> = games.into_iter().map(|path| {
        let title_path = if path.file_name().is_some_and(|name| name == "main") { path.parent().unwrap_or(&path) } else { &path };
        let title = title_path.file_stem().unwrap_or(title_path.as_os_str()).to_string_lossy().into_owned();
        let cover = ["png", "jpg", "jpeg", "webp"].into_iter().map(|ext| path.with_extension(ext)).find(|path| path.is_file());
        LibraryEntry { title, path, cover, program_id: None }
    }).collect();
    sort_entries(&mut entries, &preferences.played);
    Ok(entries)
}

fn supported(path: &Path) -> bool {
    path.extension().and_then(|ext| ext.to_str()).is_some_and(|ext| {
        ["nro", "nso", "nca", "xci", "xcz", "nsp", "nsz", "nup", "kip"].iter().any(|candidate| ext.eq_ignore_ascii_case(candidate))
    })
}

fn sort_entries(entries: &mut [LibraryEntry], played: &BTreeMap<PathBuf, u64>) {
    entries.sort_by_cached_key(|entry| (std::cmp::Reverse(played.get(&entry.path).copied()), entry.title.to_lowercase(), entry.path.clone()));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn library_recurses_deduplicates_and_remembers_played_order() {
        let scratch = tempfile::tempdir().unwrap();
        let games = scratch.path().join("games");
        let nested = games.join("nested");
        fs::create_dir_all(&nested).unwrap();
        fs::write(games.join("A.nsp"), []).unwrap();
        fs::write(nested.join("風.NUP"), []).unwrap();
        fs::write(games.join("ignored.txt"), []).unwrap();
        let dir = scratch.path().join("gpui");
        let mut library = Library::open(dir.clone()).unwrap();
        library.add_folder(games).unwrap();
        library.add_folder(nested).unwrap();
        assert_eq!(library.entries().len(), 2);
        let played = library.entries()[1].path.clone();
        library.note_played(&played).unwrap();
        assert_eq!(Library::open(dir).unwrap().entries()[0].path, played);
    }

    #[test]
    fn migrates_absolute_qt_paths_with_scan_depth() {
        let sample = tempfile::tempdir().unwrap();
        let value = format!("[Paths]\ngamedirs\\1\\path={}\ngamedirs\\1\\deep_scan=true\ngamedirs\\2\\path=UserNAND\n", sample.path().display());
        assert_eq!(qt_folders(&value), BTreeMap::from([(sample.path().into(), true)]));
    }
}
