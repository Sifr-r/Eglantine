// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::host_tool::{self, Event};
use crate::metadata::{self, GameInfo, MetadataCache};
use eglantine_library::ConsoleId;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
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
    pub console: Option<ConsoleId>,
    pub info: GameInfo,
}

pub fn browse_indices(
    entries: &[LibraryEntry], query: &str, alphabetical: bool,
    mut includes: impl FnMut(&LibraryEntry) -> bool,
) -> Vec<usize> {
    let query = query.trim().to_lowercase();
    let mut indices: Vec<_> = entries.iter().enumerate().filter_map(|(index, entry)| {
        (includes(entry) && (query.is_empty() || entry.title.to_lowercase().contains(&query)))
            .then_some(index)
    }).collect();
    if alphabetical {
        indices.sort_by_cached_key(|&index| (entries[index].title.to_lowercase(), &entries[index].path));
    }
    indices
}

#[derive(Clone, Default, Deserialize, Serialize)]
struct Preferences {
    folders: BTreeMap<PathBuf, bool>,
    played: BTreeMap<PathBuf, u64>,
}

pub struct Library {
    file: PathBuf,
    preferences: Preferences,
    cache: MetadataCache,
    entries_by_folder: BTreeMap<PathBuf, FolderScan>,
    entries: Vec<LibraryEntry>,
}

pub struct LibraryScan {
    folders: BTreeMap<PathBuf, bool>,
    cache_dir: PathBuf,
    cached_metadata: MetadataCache,
    extract_metadata: bool,
    force_refresh: bool,
}

pub struct LibraryScanResult {
    folders: BTreeMap<PathBuf, FolderScan>,
    update_cache: bool,
    pub warning: Option<String>,
}

struct FolderScan {
    entries: Vec<LibraryEntry>,
    complete: bool,
}

impl LibraryScanResult {
    pub fn is_complete(&self) -> bool {
        self.folders.values().all(|folder| folder.complete)
    }
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
        let cache = MetadataCache::load(&dir).unwrap_or_default();
        Ok(Self { file, preferences, cache, entries_by_folder: BTreeMap::new(), entries: Vec::new() })
    }

    pub fn entries(&self) -> &[LibraryEntry] { &self.entries }

    pub fn add_folder(&mut self, folder: PathBuf) -> Result<(), String> {
        let directory = folder.canonicalize().map_err(|error| format!("Could not open {}: {error}", folder.display()))?;
        if !directory.is_dir() {
            return Err(format!("{} is not a directory", folder.display()));
        }
        let mut next = self.preferences.clone();
        next.folders.entry(directory).or_insert(true);
        Self::save(&self.file, &next)?;
        self.preferences = next;
        Ok(())
    }

    pub fn metadata_request(&self, extract_metadata: bool, force_refresh: bool) -> LibraryScan {
        LibraryScan {
            folders: self.preferences.folders.clone(),
            cache_dir: self.file.with_file_name("covers"),
            cached_metadata: self.cache.clone(),
            extract_metadata,
            force_refresh,
        }
    }

    pub fn apply_metadata(&mut self, metadata: LibraryScanResult) {
        let mut previous = std::mem::take(&mut self.entries_by_folder);
        self.entries_by_folder = metadata.folders.into_iter().map(|(folder, scan)| {
            let latest = if scan.complete {
                scan
            } else {
                previous.remove(&folder)
                    .filter(|prior| prior.complete || !prior.entries.is_empty())
                    .unwrap_or(scan)
            };
            (folder, latest)
        }).collect();
        let games: BTreeMap<_, _> = self.entries_by_folder.values()
            .flat_map(|folder| &folder.entries)
            .map(|entry| (&entry.path, entry)).collect();
        self.entries = games.into_values().cloned().collect();
        sort_entries(&mut self.entries, &self.preferences.played);
        if metadata.update_cache && !self.entries.is_empty() {
            for entry in &self.entries {
                self.cache.upsert(entry);
            }
            if let Some(dir) = self.file.parent() {
                let _ = self.cache.save(dir);
            }
        }
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

    fn save(file: &Path, preferences: &Preferences) -> Result<(), String> {
        let bytes = serde_json::to_vec_pretty(preferences).map_err(|error| format!("Could not encode library: {error}"))?;
        replace(file, &bytes)
    }
}

impl LibraryScan {
    pub fn read(self) -> LibraryScanResult {
        let mut folders = BTreeMap::new();
        let mut discovery_warning = None;
        let mut cover_warning = None;
        for (directory, deep) in self.folders {
            let missing = match fs::metadata(&directory) {
                Ok(metadata) => !metadata.is_dir(),
                Err(error) => error.kind() == io::ErrorKind::NotFound,
            };
            if missing {
                folders.insert(directory, FolderScan { entries: Vec::new(), complete: true });
                continue;
            }
            let mut entries = Vec::new();
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
                    Event::Game {
                        path,
                        title,
                        format,
                        program_id,
                        icon_hex,
                        system,
                        publisher,
                        version,
                        region,
                        product_code,
                        internal_title,
                        hardware,
                        file_size,
                        languages,
                    } => {
                        let path = path.canonicalize().unwrap_or(path);
                        let console = system.as_deref().and_then(ConsoleId::parse);
                        let (cover, info) = if !self.extract_metadata && !self.force_refresh {
                            if let Some(cached) = self.cached_metadata.lookup_fresh(&path) {
                                (cached.cover, cached.info)
                            } else {
                                (
                                    metadata::find_sidecar_cover(&path),
                                    GameInfo { format, ..Default::default() },
                                )
                            }
                        } else {
                            let info = GameInfo {
                                format,
                                publisher,
                                version,
                                region,
                                product_code,
                                internal_title,
                                hardware,
                                file_size,
                                languages,
                            };
                            let cover = match find_cover(
                                &self.cache_dir,
                                &path,
                                program_id.as_deref(),
                                info.product_code.as_deref(),
                                icon_hex.as_deref(),
                            ) {
                                Ok(cover) => cover,
                                Err(error) => {
                                    cover_warning.get_or_insert(error);
                                    None
                                }
                            };
                            (cover, info)
                        };
                        entries.push(LibraryEntry { title, path, cover, program_id, console, info });
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
            let complete = error.is_none();
            if let Some(error) = error {
                discovery_warning.get_or_insert_with(|| format!("{}: {error}", directory.display()));
            }
            folders.insert(directory, FolderScan { entries, complete });
        }
        LibraryScanResult {
            folders,
            update_cache: self.extract_metadata || self.force_refresh,
            warning: discovery_warning.or(cover_warning),
        }
    }
}

fn find_cover(
    cache: &Path,
    game: &Path,
    program_id: Option<&str>,
    product_code: Option<&str>,
    icon: Option<&str>,
) -> Result<Option<PathBuf>, String> {
    if let Some(local) = metadata::find_local_cover(cache, game, program_id, product_code) {
        return Ok(Some(local));
    }
    icon.map(|data| cache_cover(cache, game, data)).transpose()
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

fn sort_entries(entries: &mut [LibraryEntry], played: &BTreeMap<PathBuf, u64>) {
    entries.sort_by_cached_key(|entry| (std::cmp::Reverse(played.get(&entry.path).copied()), entry.title.to_lowercase(), entry.path.clone()));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alphabetical_library_orders_titles_without_changing_recent_order() {
        let entries: Vec<_> = ["Zebra Circuit", "moss valley", "Amber Relay"].into_iter()
            .map(|title| LibraryEntry {
                title: title.into(), path: PathBuf::from(title), cover: None, program_id: None, console: None,
                info: GameInfo::default(),
            }).collect();
        assert_eq!(browse_indices(&entries, "", true, |_| true), [2, 1, 0]);
        assert_eq!(browse_indices(&entries, "VALLEY", true, |_| true), [1]);
        assert_eq!(browse_indices(&entries, "", false, |entry| entry.title != "moss valley"), [0, 2]);
    }

    #[test]
    fn filters_and_sorts_a_large_mixed_console_collection() {
        let entries: Vec<_> = (0..1000).rev().map(|number| {
            let console = if number % 2 == 0 { ConsoleId::GameBoyAdvance } else { ConsoleId::GameBoyColor };
            LibraryEntry {
                title: format!("Relay {number:04}"), path: PathBuf::from(format!("{number}.rom")),
                cover: None, program_id: None, console: Some(console), info: GameInfo::default(),
            }
        }).collect();
        let is_advance = |entry: &LibraryEntry| entry.console == Some(ConsoleId::GameBoyAdvance);
        let games = browse_indices(&entries, "", true, is_advance);
        assert_eq!(games.len(), 500);
        assert_eq!(entries[games[0]].path, PathBuf::from("0.rom"));
        assert_eq!(entries[*games.last().unwrap()].path, PathBuf::from("998.rom"));
        let filter = browse_indices(&entries, "RELay 009", true, is_advance);
        assert_eq!(filter.iter().map(|&index| entries[index].title.as_str()).collect::<Vec<_>>(),
            ["Relay 0090", "Relay 0092", "Relay 0094", "Relay 0096", "Relay 0098"]);
    }

    #[test]
    fn opening_and_adding_folders_only_loads_preferences() {
        let scratch = tempfile::tempdir().unwrap();
        let games = scratch.path().join("games");
        fs::create_dir(&games).unwrap();
        fs::write(games.join("unvalidated.nsp"), []).unwrap();
        let dir = scratch.path().join("gpui");
        let mut library = Library::open(dir.clone()).unwrap();
        library.add_folder(games.clone()).unwrap();
        assert!(library.entries().is_empty());
        let reopened = Library::open(dir).unwrap();
        assert!(reopened.entries().is_empty());
        assert_eq!(
            reopened.metadata_request(true, false).folders,
            BTreeMap::from([(games.canonicalize().unwrap(), true)]),
        );
    }

    #[test]
    fn host_snapshots_replace_entries_and_restore_played_order() {
        let scratch = tempfile::tempdir().unwrap();
        let dir = scratch.path().join("gpui");
        let mut library = Library::open(dir.clone()).unwrap();
        let folder = scratch.path().join("games");
        let snapshot = |entries| LibraryScanResult {
            folders: BTreeMap::from([(folder.clone(), FolderScan { entries, complete: true })]),
            update_cache: true,
            warning: None,
        };
        let entry = |title: &str| LibraryEntry {
            title: title.into(),
            path: folder.join(format!("{title}.nro")),
            cover: None,
            program_id: None,
            console: None,
            info: GameInfo::default(),
        };
        let alpha = entry("Alpha");
        let beta = entry("Beta");
        library.apply_metadata(snapshot(vec![beta.clone(), alpha.clone()]));
        assert_eq!(library.entries()[0].path, alpha.path);
        library.note_played(&beta.path).unwrap();
        assert_eq!(library.entries()[0].path, beta.path);

        let mut reopened = Library::open(dir).unwrap();
        reopened.apply_metadata(snapshot(vec![alpha.clone(), beta]));
        assert_eq!(reopened.entries()[0].title, "Beta");
        reopened.apply_metadata(snapshot(vec![alpha]));
        assert_eq!(reopened.entries().len(), 1);
        assert_eq!(reopened.entries()[0].title, "Alpha");
        reopened.apply_metadata(snapshot(Vec::new()));
        assert!(reopened.entries().is_empty());
    }

    #[test]
    fn incomplete_discovery_retains_only_the_affected_folder() {
        let scratch = tempfile::tempdir().unwrap();
        let mut library = Library::open(scratch.path().join("gpui")).unwrap();
        let alpha = scratch.path().join("alpha");
        let beta = scratch.path().join("beta");
        let entry = |folder: &Path, title: &str| LibraryEntry {
            title: title.into(),
            path: folder.join(format!("{title}.nro")),
            cover: None,
            program_id: None,
            console: None,
            info: GameInfo::default(),
        };

        let first = LibraryScanResult {
            folders: BTreeMap::from([
                (alpha.clone(), FolderScan { entries: vec![entry(&alpha, "Alpha")], complete: true }),
                (beta.clone(), FolderScan { entries: vec![entry(&beta, "Beta")], complete: true }),
            ]),
            update_cache: true,
            warning: None,
        };
        library.apply_metadata(first);
        assert_eq!(library.entries().len(), 2);

        let interrupted = LibraryScanResult {
            folders: BTreeMap::from([
                (alpha, FolderScan { entries: Vec::new(), complete: true }),
                (beta.clone(), FolderScan { entries: vec![entry(&beta, "Partial")], complete: false }),
            ]),
            update_cache: false,
            warning: Some("The emulator did not finish the scan".into()),
        };
        assert!(!interrupted.is_complete());
        library.apply_metadata(interrupted);
        assert_eq!(library.entries().len(), 1);
        assert_eq!(library.entries()[0].title, "Beta");

        let recovered = LibraryScanResult {
            folders: BTreeMap::from([
                (beta.clone(), FolderScan { entries: vec![entry(&beta, "New")], complete: true }),
            ]),
            update_cache: true,
            warning: Some("Invalid embedded cover".into()),
        };
        assert!(recovered.is_complete());
        library.apply_metadata(recovered);
        assert_eq!(library.entries().len(), 1);
        assert_eq!(library.entries()[0].title, "New");
    }

    #[test]
    fn sidecar_cover_takes_priority_over_embedded_icon() {
        let scratch = tempfile::tempdir().unwrap();
        let cache = scratch.path().join("covers");
        let game = scratch.path().join("game.nro");
        let sidecar = game.with_extension("png");
        fs::write(&sidecar, []).unwrap();
        assert_eq!(find_cover(&cache, &game, None, None, Some("invalid")).unwrap(), Some(sidecar.clone()));
        assert!(!cache.exists());

        fs::remove_file(sidecar).unwrap();
        let mut icon = io::Cursor::new(Vec::new());
        image::DynamicImage::new_rgba8(1, 1)
            .write_to(&mut icon, image::ImageFormat::Png)
            .unwrap();
        let bytes = icon.into_inner();
        let hex: String = bytes.iter().map(|byte| format!("{byte:02x}")).collect();
        let cached = find_cover(&cache, &game, None, None, Some(&hex)).unwrap().unwrap();
        assert_eq!(cached.parent(), Some(cache.as_path()));
        assert_eq!(fs::read(cached).unwrap(), bytes);
    }

    #[test]
    fn metadata_request_persists_flags_and_cache() {
        let scratch = tempfile::tempdir().unwrap();
        let dir = scratch.path().join("gpui");
        let games = scratch.path().join("games");
        fs::create_dir(&games).unwrap();
        let rom = games.join("Example.gba");
        fs::write(&rom, b"synthetic-rom").unwrap();

        let mut library = Library::open(dir.clone()).unwrap();
        let entry = LibraryEntry {
            title: "Example".into(),
            path: rom.clone(),
            cover: None,
            program_id: None,
            console: Some(ConsoleId::GameBoyAdvance),
            info: GameInfo {
                format: Some("GBA".into()),
                publisher: Some("Nintendo".into()),
                ..Default::default()
            },
        };
        library.apply_metadata(LibraryScanResult {
            folders: BTreeMap::from([(games, FolderScan { entries: vec![entry], complete: true })]),
            update_cache: true,
            warning: None,
        });

        let reopened = Library::open(dir).unwrap();
        let scan_off = reopened.metadata_request(false, false);
        assert!(!scan_off.extract_metadata);
        assert!(!scan_off.force_refresh);
        assert_eq!(
            scan_off.cached_metadata.lookup_fresh(&rom).unwrap().info.publisher.as_deref(),
            Some("Nintendo")
        );

        let scan_force = reopened.metadata_request(false, true);
        assert!(!scan_force.extract_metadata);
        assert!(scan_force.force_refresh);
    }

    #[test]
    fn migrates_absolute_qt_paths_with_scan_depth() {
        let sample = tempfile::tempdir().unwrap();
        let value = format!("[Paths]\ngamedirs\\1\\path={}\ngamedirs\\1\\deep_scan=true\ngamedirs\\2\\path=UserNAND\n", sample.path().display());
        assert_eq!(qt_folders(&value), BTreeMap::from([(sample.path().into(), true)]));
    }
}
