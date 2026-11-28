// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::library::{LibraryEntry, replace};
use eglantine_library::ConsoleId;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

const CACHE_FILE: &str = "metadata-cache.json";
const COVER_EXTENSIONS: [&str; 4] = ["png", "jpg", "jpeg", "webp"];
const COVER_SUBFOLDERS: [&str; 4] = ["covers", "boxart", "art", "images"];

#[derive(Clone, Debug, Default, PartialEq, Eq, Deserialize, Serialize)]
#[serde(default)]
pub struct GameInfo {
    pub format: Option<String>,
    pub publisher: Option<String>,
    pub version: Option<String>,
    pub region: Option<String>,
    pub product_code: Option<String>,
    pub internal_title: Option<String>,
    pub hardware: Option<String>,
    pub file_size: Option<u64>,
    pub languages: Vec<String>,
}

impl GameInfo {
    pub fn format_file_size(bytes: u64) -> String {
        const KB: f64 = 1024.0;
        const MB: f64 = 1024.0 * KB;
        const GB: f64 = 1024.0 * MB;
        let value = bytes as f64;
        if value >= GB {
            format!("{:.2} GB", value / GB)
        } else if value >= MB {
            format!("{:.1} MB", value / MB)
        } else if value >= KB {
            format!("{:.1} KB", value / KB)
        } else {
            format!("{bytes} B")
        }
    }

    pub fn summary_line(&self, console: Option<ConsoleId>) -> Option<String> {
        let mut parts = Vec::new();
        if let Some(publisher) = non_empty(self.publisher.as_deref()) {
            parts.push(publisher.to_owned());
        } else if let Some(console) = console {
            parts.push(console.name().to_owned());
        }
        if let Some(version) = non_empty(self.version.as_deref()) {
            parts.push(format_version(version));
        }
        if let Some(region) = non_empty(self.region.as_deref()) {
            parts.push(region.to_owned());
        }
        if let Some(format) = non_empty(self.format.as_deref()) {
            parts.push(format.to_owned());
        }
        if let Some(size) = self.file_size.filter(|&size| size > 0) {
            parts.push(Self::format_file_size(size));
        }
        (!parts.is_empty()).then(|| parts.join(" · "))
    }

    pub fn detail_rows(
        &self,
        console: Option<ConsoleId>,
        program_id: Option<&str>,
        title: &str,
    ) -> Vec<(&'static str, String)> {
        let mut rows = Vec::new();
        if let Some(console) = console {
            rows.push(("System", console.name().to_owned()));
        }
        if let Some(publisher) = non_empty(self.publisher.as_deref()) {
            rows.push(("Publisher", publisher.to_owned()));
        }
        if let Some(version) = non_empty(self.version.as_deref()) {
            rows.push(("Version", version.to_owned()));
        }
        if let Some(region) = non_empty(self.region.as_deref()) {
            rows.push(("Region", region.to_owned()));
        }
        if let Some(format) = non_empty(self.format.as_deref()) {
            rows.push(("Format", format.to_owned()));
        }
        if let Some(size) = self.file_size.filter(|&size| size > 0) {
            rows.push(("File size", Self::format_file_size(size)));
        }
        if let Some(program_id) = non_empty(program_id) {
            rows.push(("Program ID", program_id.to_ascii_uppercase()));
        }
        if let Some(product_code) = non_empty(self.product_code.as_deref()) {
            rows.push(("Game code", product_code.to_owned()));
        }
        if let Some(internal_title) = non_empty(self.internal_title.as_deref())
            && !internal_title.eq_ignore_ascii_case(title.trim())
        {
            rows.push(("Header title", internal_title.to_owned()));
        }
        if let Some(hardware) = non_empty(self.hardware.as_deref()) {
            rows.push(("Hardware", hardware.to_owned()));
        }
        if !self.languages.is_empty() {
            rows.push(("Languages", self.languages.join(", ")));
        }
        rows
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Deserialize, Serialize)]
pub struct CachedGameMetadata {
    pub file_size: u64,
    pub modified_ms: u64,
    pub title: String,
    pub program_id: Option<String>,
    pub console: Option<String>,
    pub cover: Option<PathBuf>,
    pub info: GameInfo,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Deserialize, Serialize)]
#[serde(default)]
pub struct MetadataCache {
    pub entries: BTreeMap<PathBuf, CachedGameMetadata>,
}

impl MetadataCache {
    pub fn load(dir: &Path) -> Result<Self, String> {
        let file = dir.join(CACHE_FILE);
        match fs::read(&file) {
            Ok(bytes) => match serde_json::from_slice(&bytes) {
                Ok(cache) => Ok(cache),
                Err(_) => Ok(Self::default()),
            },
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(Self::default()),
            Err(error) => Err(format!("Could not read {}: {error}", file.display())),
        }
    }

    pub fn save(&self, dir: &Path) -> Result<(), String> {
        let bytes = serde_json::to_vec_pretty(self)
            .map_err(|error| format!("Could not encode metadata cache: {error}"))?;
        replace(&dir.join(CACHE_FILE), &bytes)
    }

    pub fn lookup_fresh(&self, path: &Path) -> Option<CachedGameMetadata> {
        let mut cached = self.entries.get(path)?.clone();
        let (size, modified_ms) = file_fingerprint(path)?;
        if cached.file_size != size || cached.modified_ms != modified_ms {
            return None;
        }
        if let Some(cover) = &cached.cover
            && !cover.is_file()
        {
            cached.cover = None;
        }
        Some(cached)
    }

    pub fn upsert(&mut self, entry: &LibraryEntry) {
        let Some((file_size, modified_ms)) = file_fingerprint(&entry.path) else {
            return;
        };
        self.entries.insert(
            entry.path.clone(),
            CachedGameMetadata {
                file_size,
                modified_ms,
                title: entry.title.clone(),
                program_id: entry.program_id.clone(),
                console: entry.console.map(|console| console.as_str().to_owned()),
                cover: entry.cover.clone(),
                info: entry.info.clone(),
            },
        );
    }
}

pub fn find_sidecar_cover(game: &Path) -> Option<PathBuf> {
    COVER_EXTENSIONS
        .into_iter()
        .map(|extension| game.with_extension(extension))
        .find(|path| path.is_file())
}

pub fn find_local_cover(
    cache: &Path,
    game: &Path,
    program_id: Option<&str>,
    product_code: Option<&str>,
) -> Option<PathBuf> {
    if let Some(sidecar) = find_sidecar_cover(game) {
        return Some(sidecar);
    }

    let mut stems = Vec::new();
    if let Some(stem) = game.file_stem().and_then(|value| value.to_str()) {
        push_unique(&mut stems, stem.to_owned());
    }
    let mut id_stems = Vec::new();
    if let Some(program_id) = non_empty(program_id) {
        push_unique(&mut id_stems, program_id.to_ascii_lowercase());
        push_unique(&mut id_stems, program_id.to_ascii_uppercase());
    }
    if let Some(product_code) = non_empty(product_code) {
        push_unique(&mut id_stems, product_code.to_owned());
        push_unique(&mut id_stems, product_code.to_ascii_uppercase());
    }
    for id in &id_stems {
        push_unique(&mut stems, id.clone());
    }

    if let Some(parent) = game.parent() {
        for folder in COVER_SUBFOLDERS {
            let dir = parent.join(folder);
            if !dir.is_dir() {
                continue;
            }
            if let Some(found) = first_matching_cover(&dir, &stems) {
                return Some(found);
            }
        }
    }

    if cache.is_dir()
        && let Some(found) = first_matching_cover(cache, &id_stems)
    {
        return Some(found);
    }
    None
}

fn first_matching_cover(dir: &Path, stems: &[String]) -> Option<PathBuf> {
    for stem in stems {
        for ext in COVER_EXTENSIONS {
            let candidate = dir.join(format!("{stem}.{ext}"));
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    None
}

fn file_fingerprint(path: &Path) -> Option<(u64, u64)> {
    let metadata = fs::metadata(path).ok()?;
    let modified = metadata.modified().ok()?.duration_since(UNIX_EPOCH).ok()?.as_millis();
    Some((metadata.len(), modified.min(u128::from(u64::MAX)) as u64))
}

fn non_empty(value: Option<&str>) -> Option<&str> {
    value.map(str::trim).filter(|text| !text.is_empty())
}

fn format_version(version: &str) -> String {
    if version.starts_with(['v', 'V']) {
        version.to_owned()
    } else {
        format!("v{version}")
    }
}

fn push_unique(list: &mut Vec<String>, item: String) {
    if !list.contains(&item) {
        list.push(item);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn game_info_formats_sizes_summaries_and_detail_rows() {
        assert_eq!(GameInfo::format_file_size(512), "512 B");
        assert_eq!(GameInfo::format_file_size(32 * 1024), "32.0 KB");
        assert_eq!(GameInfo::format_file_size(16 * 1024 * 1024), "16.0 MB");

        let info = GameInfo {
            format: Some("GBA".into()),
            publisher: Some("Nintendo".into()),
            version: Some("1.0".into()),
            region: Some("USA".into()),
            product_code: Some("AGBE".into()),
            internal_title: Some("EXAMPLE".into()),
            hardware: None,
            file_size: Some(32 * 1024),
            languages: vec!["English".into(), "Japanese".into()],
        };
        assert_eq!(
            info.summary_line(Some(ConsoleId::GameBoyAdvance)).as_deref(),
            Some("Nintendo · v1.0 · USA · GBA · 32.0 KB")
        );
        let rows = info.detail_rows(Some(ConsoleId::GameBoyAdvance), None, "Example");
        assert!(!rows.iter().any(|(label, _)| *label == "Header title"));
        let rows_with_header = info.detail_rows(Some(ConsoleId::GameBoyAdvance), None, "Different Title");
        assert!(rows_with_header.iter().any(|(label, value)| *label == "Header title" && value == "EXAMPLE"));
        assert!(rows_with_header.iter().any(|(label, value)| *label == "Languages" && value == "English, Japanese"));
    }

    #[test]
    fn metadata_cache_round_trips_and_invalidates_on_change() {
        let scratch = tempfile::tempdir().unwrap();
        let game = scratch.path().join("game.gba");
        fs::write(&game, b"rom-v1").unwrap();
        let cover = scratch.path().join("game.png");
        fs::write(&cover, b"png").unwrap();

        let entry = LibraryEntry {
            title: "Game".into(),
            path: game.clone(),
            cover: Some(cover.clone()),
            program_id: None,
            console: Some(ConsoleId::GameBoyAdvance),
            info: GameInfo {
                format: Some("GBA".into()),
                publisher: Some("Nintendo".into()),
                file_size: Some(6),
                ..Default::default()
            },
        };

        let mut cache = MetadataCache::default();
        cache.upsert(&entry);
        cache.save(scratch.path()).unwrap();

        let loaded = MetadataCache::load(scratch.path()).unwrap();
        let fresh = loaded.lookup_fresh(&game).unwrap();
        assert_eq!(fresh.info.publisher.as_deref(), Some("Nintendo"));
        assert_eq!(fresh.cover.as_deref(), Some(cover.as_path()));

        fs::remove_file(&cover).unwrap();
        let missing_cover = loaded.lookup_fresh(&game).unwrap();
        assert_eq!(missing_cover.cover, None);

        fs::write(&game, b"rom-v2-longer").unwrap();
        assert_eq!(loaded.lookup_fresh(&game), None);
    }

    #[test]
    fn find_local_cover_follows_priority_order() {
        let scratch = tempfile::tempdir().unwrap();
        let games = scratch.path().join("games");
        let cache = scratch.path().join("covers");
        fs::create_dir_all(games.join("covers")).unwrap();
        fs::create_dir_all(&cache).unwrap();

        let game = games.join("Metroid.nsp");
        fs::write(&game, []).unwrap();
        let cache_cover = cache.join("0100123400000000.png");
        fs::write(&cache_cover, []).unwrap();
        assert_eq!(
            find_local_cover(&cache, &game, Some("0100123400000000"), None),
            Some(cache_cover)
        );

        let folder_id_cover = games.join("covers/0100123400000000.jpg");
        fs::write(&folder_id_cover, []).unwrap();
        assert_eq!(
            find_local_cover(&cache, &game, Some("0100123400000000"), None),
            Some(folder_id_cover)
        );

        let folder_stem_cover = games.join("covers/Metroid.webp");
        fs::write(&folder_stem_cover, []).unwrap();
        assert_eq!(
            find_local_cover(&cache, &game, Some("0100123400000000"), None),
            Some(folder_stem_cover)
        );

        let sidecar = games.join("Metroid.png");
        fs::write(&sidecar, []).unwrap();
        assert_eq!(
            find_local_cover(&cache, &game, Some("0100123400000000"), None),
            Some(sidecar)
        );
    }
}
