// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{Cursor, Read, Seek, SeekFrom, Write};
use std::path::Path;

use image::{ImageFormat, ImageReader, Limits};
use tempfile::NamedTempFile;
use zip::write::SimpleFileOptions;

use super::{AssetKind, MAX_ASSETS, THEME_EXTENSION, Theme, ThemeError, invalid, valid_id, valid_label};

const ACTIVE_PACKAGE: &str = "active.eglantine-theme";
const MANIFEST: &str = "theme.json";
const MAX_MANIFEST_BYTES: usize = 64 * 1024;
pub const MAX_THEME_ASSET_BYTES: usize = 8 * 1024 * 1024;
const MAX_TOTAL_ASSET_BYTES: usize = 32 * 1024 * 1024;
const MAX_PACKAGE_BYTES: u64 = 40 * 1024 * 1024;
const MAX_IMAGE_DIMENSION: u32 = 4096;

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ThemePackage {
    pub theme: Theme,
    assets: BTreeMap<String, Vec<u8>>,
}

impl ThemePackage {
    pub fn asset(&self, id: &str) -> Option<&[u8]> {
        self.assets.get(id).map(Vec::as_slice)
    }

    pub fn font_family(&self, id: &str) -> Option<String> {
        let face = ttf_parser::Face::parse(self.asset(id)?, 0).ok()?;
        [ttf_parser::name_id::TYPOGRAPHIC_FAMILY, ttf_parser::name_id::FAMILY]
            .into_iter()
            .find_map(|name_id| {
                face.names().into_iter().filter(|name| name.name_id == name_id)
                    .filter_map(|name| name.to_string())
                    .find(|name| valid_label(name))
            })
    }

    pub fn set_asset(&mut self, id: &str, kind: AssetKind, bytes: Vec<u8>) -> Result<(), ThemeError> {
        if !valid_id(id) {
            return Err(invalid("The theme asset ID is invalid"));
        }
        if self.assets.len() >= MAX_ASSETS && !self.assets.contains_key(id) {
            return Err(invalid("A theme can contain up to 24 assets"));
        }
        let remaining: usize = self.assets.iter()
            .filter(|(old_id, _)| old_id.as_str() != id)
            .map(|(_, old)| old.len())
            .sum();
        if bytes.len() > MAX_TOTAL_ASSET_BYTES - remaining {
            return Err(invalid("Theme assets exceed the package size limit"));
        }
        validate_asset(id, kind, &bytes)?;
        self.assets.insert(id.to_owned(), bytes);
        self.theme.assets.insert(id.to_owned(), kind);
        Ok(())
    }

    pub fn remove_asset(&mut self, id: &str) {
        self.assets.remove(id);
        self.theme.assets.remove(id);
        if self.theme.style.background_asset.as_deref() == Some(id) {
            self.theme.style.background_asset = None;
        }
        if self.theme.style.font_asset.as_deref() == Some(id) {
            self.theme.style.font_asset = None;
        }
        self.theme.style.icons.retain(|_, asset| asset != id);
    }

    pub fn validate(&self) -> Result<(), ThemeError> {
        self.theme.validate()?;
        if self.assets.len() != self.theme.assets.len() {
            return Err(invalid("Theme asset list and data do not match"));
        }
        let mut total = 0;
        for (id, kind) in &self.theme.assets {
            let bytes = self.asset(id).ok_or_else(|| invalid(format!("Missing theme asset {id:?}")))?;
            total += bytes.len();
            if total > MAX_TOTAL_ASSET_BYTES {
                return Err(invalid("Theme assets exceed the package size limit"));
            }
            validate_asset(id, *kind, bytes)?;
        }
        Ok(())
    }

    pub fn import(path: &Path) -> Result<Self, ThemeError> {
        let file = File::open(path)?;
        if file.metadata()?.len() > MAX_PACKAGE_BYTES {
            return Err(invalid("Theme archive exceeds the package size limit"));
        }
        Self::read_package(file)
    }

    pub fn export(&self, path: &Path) -> Result<(), ThemeError> {
        self.validate()?;
        let dir = path.parent().filter(|path| !path.as_os_str().is_empty()).unwrap_or(Path::new("."));
        let mut file = NamedTempFile::new_in(dir)?;
        self.write_package(file.as_file_mut())?;
        file.as_file_mut().sync_all()?;
        file.persist(path).map_err(|error| error.error)?;
        Ok(())
    }

    pub fn load_active(dir: &Path, safe_theme: bool) -> Result<Self, ThemeError> {
        if safe_theme {
            return Ok(Self::default());
        }
        match Self::import(&dir.join(ACTIVE_PACKAGE)) {
            Err(ThemeError::Io(error)) if error.kind() == std::io::ErrorKind::NotFound => {
                Ok(Self::default())
            }
            result => result,
        }
    }

    pub fn save_active(&self, dir: &Path) -> Result<(), ThemeError> {
        fs::create_dir_all(dir)?;
        self.export(&dir.join(ACTIVE_PACKAGE))
    }

    pub fn restore_default(dir: &Path) -> Result<(), ThemeError> {
        Self::default().save_active(dir)
    }

    pub fn suggested_filename(&self) -> String {
        format!("{}.{THEME_EXTENSION}", self.theme.id)
    }

    pub(super) fn read_package<R: Read + Seek>(mut reader: R) -> Result<Self, ThemeError> {
        let (entry_count, directory_start) = archive_entries(&mut reader)?;
        let mut archive = zip::ZipArchive::new(reader)?;
        if archive.len() != entry_count || archive.central_directory_start() != directory_start {
            return Err(invalid("Theme archive has duplicate or inconsistent entries"));
        }

        let manifest = read_entry(archive.by_name(MANIFEST)?, MAX_MANIFEST_BYTES)?;
        let theme: Theme = serde_json::from_slice(&manifest)?;
        theme.validate()?;
        let asset_dir = archive.index_for_name("assets/").is_some();
        if asset_dir {
            let dir = archive.by_name("assets/")?;
            if !dir.is_dir() || dir.size() != 0 || is_symlink(dir.unix_mode()) {
                return Err(invalid("Theme archive has an invalid assets folder"));
            }
        }
        if archive.len() != theme.assets.len() + 1 + usize::from(asset_dir) {
            return Err(invalid("Every file in the theme archive must be declared"));
        }

        let mut total = 0;
        let mut assets = BTreeMap::new();
        for (id, kind) in &theme.assets {
            let name = asset_path(id, *kind);
            let entry = archive.by_name(&name)
                .map_err(|error| invalid(format!("Could not find theme asset {id:?}: {error}")))?;
            let remaining = MAX_TOTAL_ASSET_BYTES - total;
            let bytes = read_entry(entry, MAX_THEME_ASSET_BYTES.min(remaining))?;
            validate_asset(id, *kind, &bytes)?;
            total += bytes.len();
            assets.insert(id.clone(), bytes);
        }
        Ok(Self { theme, assets })
    }

    fn write_package<W: Write + Seek>(&self, writer: W) -> Result<(), ThemeError> {
        let manifest = serde_json::to_vec_pretty(&self.theme)?;
        if manifest.len() > MAX_MANIFEST_BYTES {
            return Err(invalid("Theme manifest is too large"));
        }
        let mut archive = zip::ZipWriter::new(writer);
        let options = SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
        archive.start_file(MANIFEST, options)?;
        archive.write_all(&manifest)?;
        for (id, kind) in &self.theme.assets {
            archive.start_file(asset_path(id, *kind), options)?;
            archive.write_all(&self.assets[id])?;
        }
        archive.finish()?;
        Ok(())
    }
}

fn asset_path(id: &str, kind: AssetKind) -> String {
    format!("assets/{id}.{}", kind.extension())
}

// Bound central-directory allocations before the ZIP reader loads entries or deduplicates names.
// The package limits fit in ZIP32, including archives with the optional ZIP comment.
fn archive_entries<R: Read + Seek>(reader: &mut R) -> Result<(usize, u64), ThemeError> {
    const END_RECORD_BYTES: usize = 22;
    const MAX_COMMENT_BYTES: usize = u16::MAX as usize;
    let size = reader.seek(SeekFrom::End(0))?;
    if !(END_RECORD_BYTES as u64..=MAX_PACKAGE_BYTES).contains(&size) {
        return Err(invalid("Theme archive has an invalid size"));
    }
    let tail_size = size.min((END_RECORD_BYTES + MAX_COMMENT_BYTES) as u64) as usize;
    let tail_start = size - tail_size as u64;
    reader.seek(SeekFrom::Start(tail_start))?;
    let mut tail = vec![0; tail_size];
    reader.read_exact(&mut tail)?;

    let record_start = (0..=tail_size - END_RECORD_BYTES).rev().find(|&offset| {
        tail[offset..].starts_with(b"PK\x05\x06")
            && usize::from(u16::from_le_bytes([tail[offset + 20], tail[offset + 21]]))
                == tail_size - offset - END_RECORD_BYTES
    });
    let offset = record_start.ok_or_else(|| invalid("Missing theme archive directory"))?;
    let record = &tail[offset..offset + END_RECORD_BYTES];
    let read_u16 = |offset| u16::from_le_bytes([record[offset], record[offset + 1]]);
    let read_u32 = |offset| {
        u32::from_le_bytes([record[offset], record[offset + 1], record[offset + 2], record[offset + 3]])
    };
    let entries = usize::from(read_u16(10));
    let start = u64::from(read_u32(16));
    let end = start + u64::from(read_u32(12));
    if read_u16(4) != 0 || read_u16(6) != 0 || usize::from(read_u16(8)) != entries
        || !(1..=MAX_ASSETS + 2).contains(&entries) || end != tail_start + offset as u64
    {
        return Err(invalid("Theme archives require a bounded, single-disk ZIP directory"));
    }
    Ok((entries, start))
}

fn is_symlink(mode: Option<u32>) -> bool {
    mode.is_some_and(|mode| mode & 0o170000 == 0o120000)
}

fn read_entry<R: Read>(entry: zip::read::ZipFile<'_, R>, max_size: usize) -> Result<Vec<u8>, ThemeError> {
    if entry.enclosed_name().is_none() || !entry.is_file() || is_symlink(entry.unix_mode()) {
        return Err(invalid("Theme archives must contain regular relative files"));
    }
    let size = entry.size();
    if size > max_size as u64 {
        return Err(invalid(format!("Theme entry {:?} exceeds its size limit", entry.name())));
    }
    let mut bytes = Vec::with_capacity(size as usize);
    entry.take(max_size as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 != size {
        return Err(invalid("Theme archive entry has an invalid size"));
    }
    Ok(bytes)
}

fn validate_asset(id: &str, kind: AssetKind, bytes: &[u8]) -> Result<(), ThemeError> {
    if bytes.is_empty() || bytes.len() > MAX_THEME_ASSET_BYTES {
        return Err(invalid(format!("Theme asset {id:?} has an invalid size")));
    }
    let format = match kind {
        AssetKind::Png => ImageFormat::Png,
        AssetKind::Jpeg => ImageFormat::Jpeg,
        AssetKind::Webp => ImageFormat::WebP,
        AssetKind::Ttf => return validate_font(id, bytes, [0, 1, 0, 0], *b"true"),
        AssetKind::Otf => return validate_font(id, bytes, *b"OTTO", *b"OTTO"),
    };
    let mut reader = ImageReader::with_format(Cursor::new(bytes), format);
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAX_IMAGE_DIMENSION);
    limits.max_image_height = Some(MAX_IMAGE_DIMENSION);
    limits.max_alloc = Some(128 * 1024 * 1024);
    reader.limits(limits);
    reader.decode().map_err(|error| invalid(format!("Invalid theme image {id:?}: {error}")))?;
    Ok(())
}

fn validate_font(id: &str, bytes: &[u8], magic: [u8; 4], alternate_magic: [u8; 4]) -> Result<(), ThemeError> {
    if !(bytes.starts_with(&magic) || bytes.starts_with(&alternate_magic))
        || ttf_parser::Face::parse(bytes, 0).is_err()
    {
        return Err(invalid(format!("Theme font {id:?} cannot be loaded")));
    }
    Ok(())
}
