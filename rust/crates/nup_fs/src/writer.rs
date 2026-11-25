// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::collections::{BTreeMap, HashMap};
use std::fs::{self, File, OpenOptions};
use std::io::{BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::AtomicBool;

use crate::format::{
    align_up, is_cancelled, put_u16_le, put_u32_le, put_u64_le, read_u64_le, sha256, Sha256,
    ZstdCodec, CODEC_RAW, CODEC_ZSTD, DEFAULT_CHUNK_BYTES, DEFAULT_RAW_THRESHOLD, DIR_RECORD_SIZE,
    EXTENT_ALIGNMENT, FILE_RECORD_SIZE, HEADER_MAGIC, MAX_CHUNK_FILES, NO_DIR,
    PACKAGE_FLAG_STANDALONE_UPDATE, PACKAGE_RECORD_SIZE, PROGRESS_STEP, READ_BUFFER_SIZE,
    SUPERBLOCK_SIZE, TOC_HEADER_SIZE, TOC_MAGIC,
};
pub use crate::journal::{Plan, PlanEntry, PlanPackage};
use crate::journal::{
    extent_to_json, journal_path_for, parse_journal_file, part_path_for, plan_to_json,
    JournalExtent,
};
use crate::metadata::{
    find_directory_ci, probe_package_dir, serialize_localized_titles, serialize_tlvs,
    set_or_remove_tlv, set_tlv, MetaType, Metadata, PackageLoaderInfo,
};
use crate::reader::PackageKind;

const PARALLEL_MAX_FILE_SIZE: u64 = 32 * 1024 * 1024;
const PARALLEL_BATCH_BYTES: u64 = 64 * 1024 * 1024;
const FILE_TAG_BIT: u64 = 1u64 << 32;

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PackResult {
    Success = 0,
    Cancelled = 1,
    InvalidSource = 2,
    IoError = 3,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PackProgress {
    pub bytes_done: u64,
    pub bytes_total: u64,
    pub files_done: u64,
    pub files_total: u64,
}

#[derive(Clone, Debug)]
pub struct PackOptions {
    pub delete_sources: bool,
    pub zstd_level: i32,
    pub chunk_bytes: u64,
    pub raw_threshold: u64,
    pub nacp_language_index: i32,
    pub metadata: Metadata,
}

impl Default for PackOptions {
    fn default() -> Self {
        Self {
            delete_sources: false,
            zstd_level: 3,
            chunk_bytes: DEFAULT_CHUNK_BYTES,
            raw_threshold: DEFAULT_RAW_THRESHOLD,
            nacp_language_index: 1,
            metadata: Metadata::default(),
        }
    }
}

fn eq_ascii_ci(a: &str, b: &str) -> bool {
    a.eq_ignore_ascii_case(b)
}

fn has_bootable_exefs_dir(exefs: &Path) -> bool {
    exefs.join("main").is_file() && exefs.join("main.npdm").is_file()
}

fn parse_decimal_u64(name: &str) -> Option<u64> {
    if name.is_empty() || !name.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    name.parse::<u64>().ok()
}

fn parse_title_id_hex16(name: &str) -> Option<u64> {
    if name.len() != 16 || !name.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    u64::from_str_radix(name, 16).ok()
}

fn parse_npdm_title_id(npdm: &[u8]) -> Option<u64> {
    let aci_offset = npdm.windows(4).position(|w| w == b"ACI0")?;
    if aci_offset + 0x18 > npdm.len() {
        return None;
    }
    Some(read_u64_le(npdm, aci_offset + 0x10))
}

/// Checks whether `dir` is a valid bootable loose Switch dump (matches `HasBootableExeFS`).
pub fn is_loose_dump(dir: &Path) -> bool {
    if !dir.is_dir() {
        return false;
    }
    let mut best_exefs: Option<PathBuf> = None;
    let mut best_version: Option<u64> = None;
    if let Some(updates) = find_directory_ci(dir, "updates") {
        if let Ok(entries) = fs::read_dir(&updates) {
            for entry in entries.flatten() {
                let p = entry.path();
                if !p.is_dir() {
                    continue;
                }
                let Some(ver) = entry.file_name().to_str().and_then(parse_decimal_u64) else {
                    continue;
                };
                if let Some(candidate) = find_directory_ci(&p, "exefs") {
                    if has_bootable_exefs_dir(&candidate)
                        && best_version.is_none_or(|b| ver > b)
                    {
                        best_version = Some(ver);
                        best_exefs = Some(candidate);
                    }
                }
            }
        }
    }

    let base = find_directory_ci(dir, "base");
    if best_exefs.is_none() {
        if let Some(ref b) = base {
            if let Some(exefs) = find_directory_ci(b, "exefs") {
                best_exefs = Some(exefs);
            }
        }
    }
    if best_exefs.is_none() {
        best_exefs = find_directory_ci(dir, "exefs");
    }

    let Some(exefs) = best_exefs else {
        return false;
    };
    if !has_bootable_exefs_dir(&exefs) {
        return false;
    }
    let has_pkg_romfs = exefs
        .parent()
        .is_some_and(|pkg| find_directory_ci(pkg, "romfs").is_some());
    let has_base_romfs = base
        .as_ref()
        .is_some_and(|b| find_directory_ci(b, "romfs").is_some());
    has_pkg_romfs || has_base_romfs
}

/// Resolves the loose dump root from either `.../exefs` or `.../exefs/main`.
pub fn dump_root_from_main(main_path: &Path) -> Option<PathBuf> {
    let exefs = if main_path.is_dir() {
        main_path.to_path_buf()
    } else {
        main_path.parent()?.to_path_buf()
    };
    let exefs_name = exefs.file_name()?.to_str()?;
    if !eq_ascii_ci(exefs_name, "exefs") {
        return None;
    }
    let pkg_dir = exefs.parent()?;
    let pkg_name = pkg_dir.file_name().and_then(|s| s.to_str()).unwrap_or("");
    let candidate = if eq_ascii_ci(pkg_name, "base") {
        pkg_dir.parent()?.to_path_buf()
    } else if parse_decimal_u64(pkg_name).is_some()
        && pkg_dir
            .parent()
            .and_then(|p| p.file_name())
            .and_then(|n| n.to_str())
            .is_some_and(|n| eq_ascii_ci(n, "updates"))
    {
        pkg_dir.parent()?.parent()?.to_path_buf()
    } else {
        pkg_dir.to_path_buf()
    };

    is_loose_dump(&candidate).then_some(candidate)
}

fn normalized_dir_without_trailing_sep(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for comp in path.components() {
        match comp {
            Component::CurDir => {}
            c => out.push(c.as_os_str()),
        }
    }
    if out.as_os_str().is_empty() {
        path.to_path_buf()
    } else {
        out
    }
}

/// Returns `<dump_root>.nup` cleanly even when `dump_root` has a trailing slash.
pub fn default_archive_path(dump_root: &Path) -> PathBuf {
    let norm = normalized_dir_without_trailing_sep(dump_root);
    if let (Some(parent), Some(name)) = (norm.parent(), norm.file_name()) {
        let mut file_name = name.to_os_string();
        file_name.push(".nup");
        if parent.as_os_str().is_empty() {
            PathBuf::from(file_name)
        } else {
            parent.join(file_name)
        }
    } else {
        norm.join("archive.nup")
    }
}

/// Returns `<archive_parent>/<archive_stem>` for extracting a `.nup` file.
pub fn default_extract_path(archive: &Path) -> PathBuf {
    let stem = archive
        .file_stem()
        .unwrap_or_else(|| std::ffi::OsStr::new("extracted"));
    match archive.parent() {
        Some(parent) if !parent.as_os_str().is_empty() => parent.join(stem),
        _ => PathBuf::from(stem),
    }
}

fn normalize_cmp_path(path: &Path) -> PathBuf {
    let abs = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .map(|cwd| cwd.join(path))
            .unwrap_or_else(|_| path.to_path_buf())
    };
    let mut out = PathBuf::new();
    for comp in abs.components() {
        match comp {
            Component::CurDir => {}
            Component::ParentDir => {
                out.pop();
            }
            c => out.push(c.as_os_str()),
        }
    }
    #[cfg(windows)]
    {
        PathBuf::from(out.to_string_lossy().to_ascii_lowercase())
    }
    #[cfg(not(windows))]
    {
        out
    }
}

fn dir_has_files(dir: &Path) -> bool {
    if !dir.is_dir() {
        return false;
    }
    let Ok(entries) = fs::read_dir(dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let p = entry.path();
        if p.is_file() {
            return true;
        }
        if p.is_dir() && dir_has_files(&p) {
            return true;
        }
    }
    false
}

fn has_package_content(dir: &Path) -> bool {
    find_directory_ci(dir, "exefs").is_some_and(|e| dir_has_files(&e))
        || find_directory_ci(dir, "romfs").is_some_and(|r| dir_has_files(&r))
}

fn to_generic_relative_utf8(path: &Path, dump_root: &Path) -> Result<String, String> {
    let rel = path.strip_prefix(dump_root).unwrap_or(path);
    let mut parts = Vec::new();
    for comp in rel.components() {
        let s = comp
            .as_os_str()
            .to_str()
            .ok_or_else(|| format!("non-UTF-8 path: {}", path.display()))?;
        parts.push(s);
    }
    Ok(parts.join("/"))
}

struct Discovery<'a> {
    dump_root: &'a Path,
    excluded: Vec<PathBuf>,
    options: &'a PackOptions,
    plan: Plan,
    package_tlvs: Vec<Vec<(u16, Vec<u8>)>>,
    loaders: Vec<(usize, PackageLoaderInfo)>,
}

impl<'a> Discovery<'a> {
    fn new(dump_root: &'a Path, excluded: Vec<PathBuf>, options: &'a PackOptions) -> Self {
        Self {
            dump_root,
            excluded,
            options,
            plan: Plan {
                packages: Vec::new(),
                entries: Vec::new(),
            },
            package_tlvs: Vec::new(),
            loaders: Vec::new(),
        }
    }

    fn is_excluded(&self, path: &Path) -> bool {
        let norm = normalize_cmp_path(path);
        self.excluded.iter().any(|e| e == &norm)
    }

    fn add_file(&mut self, package: u32, path: &Path, archive_path: &str) -> Result<(), String> {
        let meta = fs::symlink_metadata(path)
            .map_err(|e| format!("failed to stat {}: {e}", path.display()))?;
        if meta.file_type().is_symlink() {
            return Err(format!("symlinks are not supported: {}", path.display()));
        }
        if !meta.is_file() {
            return Err(format!("unsupported file type: {}", path.display()));
        }
        if path.file_name().and_then(|s| s.to_str()) == Some(".listing") || self.is_excluded(path) {
            return Ok(());
        }
        let source = to_generic_relative_utf8(path, self.dump_root)?;
        self.plan.entries.push(PlanEntry {
            package,
            path: archive_path.to_string(),
            source,
            directory: false,
        });
        Ok(())
    }

    fn collect_tree(
        &mut self,
        package: u32,
        root: &Path,
        prefix: &str,
    ) -> Result<(), String> {
        self.plan.entries.push(PlanEntry {
            package,
            path: prefix.to_string(),
            source: to_generic_relative_utf8(root, self.dump_root)?,
            directory: true,
        });
        self.collect_tree_inner(package, root, prefix)
    }

    fn collect_tree_inner(
        &mut self,
        package: u32,
        dir: &Path,
        prefix: &str,
    ) -> Result<(), String> {
        let entries =
            fs::read_dir(dir).map_err(|e| format!("cannot list {}: {e}", dir.display()))?;
        for entry_res in entries {
            let entry = entry_res.map_err(|e| format!("directory error: {e}"))?;
            let p = entry.path();
            let meta = fs::symlink_metadata(&p)
                .map_err(|e| format!("failed to stat {}: {e}", p.display()))?;
            let name = entry
                .file_name()
                .to_str()
                .ok_or_else(|| format!("non-UTF-8 path: {}", p.display()))?
                .to_owned();
            let arch_path = format!("{prefix}/{name}");
            if !meta.file_type().is_symlink() && meta.is_dir() {
                self.plan.entries.push(PlanEntry {
                    package,
                    path: arch_path.clone(),
                    source: to_generic_relative_utf8(&p, self.dump_root)?,
                    directory: true,
                });
                self.collect_tree_inner(package, &p, &arch_path)?;
            } else {
                self.add_file(package, &p, &arch_path)?;
            }
        }
        Ok(())
    }

    fn add_package(
        &mut self,
        kind: PackageKind,
        dir: &Path,
        version: u32,
        skip: &[&str],
        flags: u16,
        fallback_id: u64,
    ) -> Result<u64, String> {
        let index = self.plan.packages.len();
        let pkg_idx_u32 = u32::try_from(index).map_err(|_| "too many packages".to_string())?;
        let mut package = PlanPackage {
            title_id: fallback_id,
            version,
            kind: kind as u8,
            flags,
            meta: Vec::new(),
        };
        self.package_tlvs.push(Vec::new());

        let exefs = find_directory_ci(dir, "exefs");
        let romfs = find_directory_ci(dir, "romfs");
        let mut npdm_path = exefs
            .as_ref()
            .map(|e| e.join("main.npdm"))
            .unwrap_or_default();
        if !npdm_path.is_file() {
            npdm_path = dir.join("main.npdm");
        }
        if npdm_path.is_file() {
            let npdm =
                fs::read(&npdm_path).map_err(|e| format!("cannot read {}: {e}", npdm_path.display()))?;
            if let Some(id) = parse_npdm_title_id(&npdm) {
                if kind != PackageKind::Addon && id != 0 {
                    package.title_id = id;
                }
            }
            self.package_tlvs[index].push((MetaType::Npdm as u16, npdm));
        }

        if kind != PackageKind::Addon {
            if let Some(loader_info) = probe_package_dir(dir, self.options.nacp_language_index) {
                if !loader_info.display_version.is_empty() {
                    self.package_tlvs[index].push((
                        MetaType::DisplayVersion as u16,
                        loader_info.display_version.as_bytes().to_vec(),
                    ));
                }
                self.loaders.push((index, loader_info));
            }
        }

        let tid_out = package.title_id;
        self.plan.packages.push(package);

        if let Some(ref e) = exefs {
            if dir_has_files(e) {
                self.collect_tree(pkg_idx_u32, e, "exefs")?;
            }
        }
        if let Some(ref r) = romfs {
            if dir_has_files(r) {
                self.collect_tree(pkg_idx_u32, r, "romfs")?;
            }
        }

        if let Ok(entries) = fs::read_dir(dir) {
            for entry_res in entries {
                let entry = entry_res.map_err(|e| format!("directory error: {e}"))?;
                let p = entry.path();
                if exefs.as_ref() == Some(&p) || romfs.as_ref() == Some(&p) {
                    continue;
                }
                let name = entry
                    .file_name()
                    .to_str()
                    .ok_or_else(|| format!("non-UTF-8 path: {}", p.display()))?
                    .to_owned();
                let lower = name.to_ascii_lowercase();
                if skip.iter().any(|&s| s == lower) {
                    continue;
                }
                let meta = fs::symlink_metadata(&p)
                    .map_err(|e| format!("failed to stat {}: {e}", p.display()))?;
                if !meta.file_type().is_symlink() && meta.is_dir() {
                    self.collect_tree(pkg_idx_u32, &p, &name)?;
                } else {
                    self.add_file(pkg_idx_u32, &p, &name)?;
                }
            }
        }

        Ok(tid_out)
    }

    fn add_application_metadata(&mut self) {
        let target = self
            .plan
            .packages
            .iter()
            .position(|p| p.kind == PackageKind::Base as u8)
            .unwrap_or(0);
        self.loaders
            .sort_by_key(|(idx, _)| usize::from(*idx != target));

        let tlvs = &mut self.package_tlvs[target];
        let mut have_control = false;
        let mut have_icon = false;
        for (_, info) in &self.loaders {
            if !have_control {
                if let Some(ref nacp) = info.nacp {
                    have_control = true;
                    tlvs.push((MetaType::Nacp as u16, nacp.raw_bytes.clone()));
                    if !nacp.application_name.is_empty() {
                        tlvs.push((
                            MetaType::TitleName as u16,
                            nacp.application_name.as_bytes().to_vec(),
                        ));
                    }
                    if !nacp.developer_name.is_empty() {
                        tlvs.push((
                            MetaType::Publisher as u16,
                            nacp.developer_name.as_bytes().to_vec(),
                        ));
                    }
                }
            }
            if !have_icon && !info.icon.is_empty() {
                have_icon = true;
                tlvs.push((MetaType::Icon as u16, info.icon.clone()));
            }
        }

        let overrides = &self.options.metadata;
        if !overrides.title_name.is_empty() {
            set_tlv(
                tlvs,
                MetaType::TitleName as u16,
                overrides.title_name.as_bytes().to_vec(),
            );
        }
        if !overrides.publisher.is_empty() {
            set_tlv(
                tlvs,
                MetaType::Publisher as u16,
                overrides.publisher.as_bytes().to_vec(),
            );
        }
        if !overrides.icon.is_empty() {
            set_tlv(tlvs, MetaType::Icon as u16, overrides.icon.clone());
        }
        if !overrides.localized_titles.is_empty() {
            let encoded = serialize_localized_titles(&overrides.localized_titles);
            set_or_remove_tlv(tlvs, MetaType::LocalizedTitles as u16, Some(&encoded));
        }
        if !overrides.display_version.is_empty() {
            set_tlv(
                tlvs,
                MetaType::DisplayVersion as u16,
                overrides.display_version.as_bytes().to_vec(),
            );
            let mut newest: Option<usize> = None;
            for (i, pkg) in self.plan.packages.iter().enumerate() {
                if pkg.kind == PackageKind::Update as u8
                    && newest.is_none_or(|b| pkg.version >= self.plan.packages[b].version)
                {
                    newest = Some(i);
                }
            }
            if let Some(n) = newest {
                if n != target {
                    set_tlv(
                        &mut self.package_tlvs[n],
                        MetaType::DisplayVersion as u16,
                        overrides.display_version.as_bytes().to_vec(),
                    );
                }
            }
        }
    }

    fn run(mut self) -> Result<Plan, (PackResult, String)> {
        let base = find_directory_ci(self.dump_root, "base");
        let updates = find_directory_ci(self.dump_root, "updates");
        let dlc = find_directory_ci(self.dump_root, "dlc");

        let mut base_title_id: Option<u64> = None;
        if let Some(ref b) = base {
            if has_package_content(b) {
                let tid = self
                    .add_package(PackageKind::Base, b, 0, &[], 0, 0)
                    .map_err(|e| (PackResult::InvalidSource, e))?;
                base_title_id = Some(tid);
            }
        } else if updates.is_none() && has_package_content(self.dump_root) {
            let tid = self
                .add_package(
                    PackageKind::Base,
                    self.dump_root,
                    0,
                    &["base", "updates", "dlc"],
                    0,
                    0,
                )
                .map_err(|e| (PackResult::InvalidSource, e))?;
            base_title_id = Some(tid);
        }

        if let Some(ref udir) = updates {
            let mut versions: Vec<(u32, PathBuf)> = Vec::new();
            if let Ok(entries) = fs::read_dir(udir) {
                for entry in entries.flatten() {
                    let p = entry.path();
                    if !p.is_dir() {
                        continue;
                    }
                    let Some(ver_u64) = entry.file_name().to_str().and_then(parse_decimal_u64) else {
                        continue;
                    };
                    if has_package_content(&p) {
                        let ver_u32 = u32::try_from(ver_u64).map_err(|_| {
                            (
                                PackResult::InvalidSource,
                                format!("update version out of range: {}", p.display()),
                            )
                        })?;
                        versions.push((ver_u32, p));
                    }
                }
            }
            versions.sort_by(|a, b| a.0.cmp(&b.0).then_with(|| a.1.cmp(&b.1)));
            let update_flags = if base_title_id.is_some() {
                0
            } else {
                PACKAGE_FLAG_STANDALONE_UPDATE as u16
            };
            let fallback = base_title_id.unwrap_or(0);
            for (ver, p) in versions {
                self.add_package(PackageKind::Update, &p, ver, &[], update_flags, fallback)
                    .map_err(|e| (PackResult::InvalidSource, e))?;
            }
        }

        if let Some(ref ddir) = dlc {
            let mut addons: Vec<(String, PathBuf, u64)> = Vec::new();
            if let Ok(tids) = fs::read_dir(ddir) {
                for tid_entry in tids.flatten() {
                    let tid_path = tid_entry.path();
                    if !tid_path.is_dir() {
                        continue;
                    }
                    let Some(tid_name) = tid_entry.file_name().to_str().map(str::to_owned) else {
                        continue;
                    };
                    let Some(tid_val) = parse_title_id_hex16(&tid_name) else {
                        continue;
                    };
                    if let Ok(slots) = fs::read_dir(&tid_path) {
                        for slot in slots.flatten() {
                            let spath = slot.path();
                            if spath.is_dir() && has_package_content(&spath) {
                                if let Some(sname) = slot.file_name().to_str() {
                                    addons.push((format!("{tid_name}/{sname}"), spath, tid_val));
                                }
                            }
                        }
                    }
                }
            }
            addons.sort_by(|a, b| a.0.cmp(&b.0));
            for (_, path, tid) in addons {
                self.add_package(PackageKind::Addon, &path, 0, &[], 0, tid)
                    .map_err(|e| (PackResult::InvalidSource, e))?;
            }
        }

        if self.plan.packages.is_empty() {
            return Err((
                PackResult::InvalidSource,
                "no packages found in the dump".to_string(),
            ));
        }

        self.add_application_metadata();
        let source_name = normalized_dir_without_trailing_sep(self.dump_root)
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("")
            .as_bytes()
            .to_vec();
        for i in 0..self.plan.packages.len() {
            let mut tlvs = std::mem::take(&mut self.package_tlvs[i]);
            tlvs.push((MetaType::SourceDump as u16, source_name.clone()));
            self.plan.packages[i].meta = serialize_tlvs(tlvs);
        }

        Ok(self.plan)
    }
}

#[derive(Default)]
struct TreeDir {
    name: String,
    children: BTreeMap<String, TreeChild>,
}

#[derive(Default)]
struct TreeChild {
    dir: Option<Box<TreeDir>>,
    file_entry: usize,
}

impl TreeDir {
    fn descend(&mut self, component: &str) -> &mut TreeDir {
        let entry = self
            .children
            .entry(component.to_string())
            .or_default();
        if entry.dir.is_none() {
            entry.dir = Some(Box::new(TreeDir {
                name: component.to_string(),
                children: BTreeMap::new(),
            }));
        }
        entry.dir.as_deref_mut().expect("dir just created")
    }
}

struct LayoutPackage {
    title_id: u64,
    version: u32,
    kind: u8,
    flags: u8,
    root_dir: u32,
    exefs_dir: u32,
    romfs_dir: u32,
    meta: Vec<u8>,
}

struct LayoutDir {
    name: String,
    first_child: u32,
    child_count: u32,
}

struct LayoutFile {
    name: String,
    source_path: PathBuf,
    size: u64,
}

struct Layout {
    packages: Vec<LayoutPackage>,
    dirs: Vec<LayoutDir>,
    files: Vec<LayoutFile>,
    children_u64: Vec<u64>,
    string_pool: Vec<u8>,
    pool_index: HashMap<String, u32>,
    toc_size: u64,
    blob_offset: u64,
}

impl Layout {
    fn intern(&mut self, name: &str) -> u32 {
        if let Some(&idx) = self.pool_index.get(name) {
            return idx;
        }
        let idx = self.string_pool.len() as u32;
        self.string_pool.extend_from_slice(name.as_bytes());
        self.pool_index.insert(name.to_string(), idx);
        idx
    }
}

fn assign_dir(
    dir: &mut TreeDir,
    layout: &mut Layout,
    plan_entries: &[PlanEntry],
    dump_root: &Path,
) -> Result<u32, String> {
    let id = u32::try_from(layout.dirs.len()).map_err(|_| "directory count overflow".to_string())?;
    let first_child =
        u32::try_from(layout.children_u64.len()).map_err(|_| "children overflow".to_string())?;
    let child_count =
        u32::try_from(dir.children.len()).map_err(|_| "child count overflow".to_string())?;
    layout.dirs.push(LayoutDir {
        name: std::mem::take(&mut dir.name),
        first_child,
        child_count,
    });

    let slot_base = layout.children_u64.len();
    layout
        .children_u64
        .resize(slot_base + dir.children.len(), 0);

    // Match C++ AssignDir: assign all direct file children in slot order first,
    // then recurse into directory children in sorted order.
    let mut slot = slot_base;
    for (name, child) in &dir.children {
        if child.dir.is_none() {
            let file_idx = layout.files.len() as u64;
            layout.children_u64[slot] = FILE_TAG_BIT | file_idx;
            let src_rel = &plan_entries[child.file_entry].source;
            let src_path = dump_root.join(src_rel);
            let size = fs::metadata(&src_path).map(|m| m.len()).unwrap_or(0);
            layout.files.push(LayoutFile {
                name: name.clone(),
                source_path: src_path,
                size,
            });
        }
        slot += 1;
    }

    slot = slot_base;
    for child in dir.children.values_mut() {
        if let Some(ref mut sub) = child.dir {
            layout.children_u64[slot] = assign_dir(sub, layout, plan_entries, dump_root)? as u64;
        }
        slot += 1;
    }

    Ok(id)
}

fn build_layout(plan: &Plan, dump_root: &Path) -> Result<Layout, String> {
    if plan.packages.is_empty() {
        return Err("plan has no packages".to_string());
    }
    let mut roots: Vec<TreeDir> = (0..plan.packages.len()).map(|_| TreeDir::default()).collect();

    for (entry_idx, entry) in plan.entries.iter().enumerate() {
        let pkg_idx = entry.package as usize;
        if pkg_idx >= roots.len() {
            return Err("plan entry references out-of-range package".to_string());
        }
        let mut dir = &mut roots[pkg_idx];
        let mut start = 0usize;
        loop {
            match entry.path[start..].find('/') {
                None => {
                    let component = &entry.path[start..];
                    if entry.directory {
                        let _ = dir.descend(component);
                    } else {
                        let child = dir.children.entry(component.to_string()).or_default();
                        if child.dir.is_some() {
                            return Err(format!(
                                "a file and a folder share the name {}",
                                entry.path
                            ));
                        }
                        child.file_entry = entry_idx;
                    }
                    break;
                }
                Some(rel_slash) => {
                    let component = &entry.path[start..start + rel_slash];
                    dir = dir.descend(component);
                    start += rel_slash + 1;
                }
            }
        }
    }

    let mut layout = Layout {
        packages: Vec::with_capacity(plan.packages.len()),
        dirs: Vec::new(),
        files: Vec::new(),
        children_u64: Vec::new(),
        string_pool: Vec::new(),
        pool_index: HashMap::new(),
        toc_size: 0,
        blob_offset: 0,
    };

    for (i, root) in roots.iter_mut().enumerate() {
        let root_dir = assign_dir(root, &mut layout, &plan.entries, dump_root)?;
        let p = &plan.packages[i];
        layout.packages.push(LayoutPackage {
            title_id: p.title_id,
            version: p.version,
            kind: p.kind,
            flags: p.flags as u8,
            root_dir,
            exefs_dir: NO_DIR,
            romfs_dir: NO_DIR,
            meta: p.meta.clone(),
        });
    }

    // Propagate file counts upward so empty exefs/romfs directories stay NO_DIR.
    let mut file_counts = vec![0u64; layout.dirs.len()];
    for i in (0..layout.dirs.len()).rev() {
        let first = layout.dirs[i].first_child as usize;
        let count = layout.dirs[i].child_count as usize;
        let mut total = 0u64;
        for &child in &layout.children_u64[first..first + count] {
            if (child & FILE_TAG_BIT) != 0 {
                total += 1;
            } else {
                total += file_counts[child as usize];
            }
        }
        file_counts[i] = total;
    }

    for pkg in &mut layout.packages {
        let first = layout.dirs[pkg.root_dir as usize].first_child as usize;
        let count = layout.dirs[pkg.root_dir as usize].child_count as usize;
        for &child in &layout.children_u64[first..first + count] {
            if (child & FILE_TAG_BIT) != 0 {
                continue;
            }
            let id = child as u32;
            let name = &layout.dirs[id as usize].name;
            if name == "exefs" && file_counts[id as usize] > 0 {
                pkg.exefs_dir = id;
            } else if name == "romfs" && file_counts[id as usize] > 0 {
                pkg.romfs_dir = id;
            }
        }
    }

    if (layout.dirs.len() + layout.files.len()) as u64 >= NO_DIR as u64 {
        return Err("the dump has too many files".to_string());
    }

    let dir_names: Vec<String> = layout.dirs.iter().map(|d| d.name.clone()).collect();
    for name in &dir_names {
        layout.intern(name);
    }
    let file_names: Vec<String> = layout.files.iter().map(|f| f.name.clone()).collect();
    for name in &file_names {
        layout.intern(name);
    }
    if u32::try_from(layout.string_pool.len()).is_err() {
        return Err("the dump's file names are too long".to_string());
    }

    let unpadded = (TOC_HEADER_SIZE
        + layout.packages.len() * PACKAGE_RECORD_SIZE
        + layout.dirs.len() * DIR_RECORD_SIZE
        + layout.files.len() * FILE_RECORD_SIZE
        + layout.children_u64.len() * 4
        + layout.string_pool.len()) as u64;
    layout.toc_size = align_up(unpadded, EXTENT_ALIGNMENT);
    layout.blob_offset = align_up((SUPERBLOCK_SIZE as u64) + layout.toc_size, EXTENT_ALIGNMENT);
    Ok(layout)
}

#[derive(Clone)]
struct ExtentRecord {
    codec: u8,
    offset: u64,
    size: u64,
    stored_size: u64,
    sha256: [u8; 32],
}

struct PreparedChunkFile {
    file_index: usize,
    size: u64,
    sha256: [u8; 32],
    codec: u8,
    payload: Vec<u8>,
}

fn check_cancelled(
    cancel: Option<&AtomicBool>,
    cancel_poll: Option<&dyn Fn() -> bool>,
) -> Result<(), (PackResult, String)> {
    if is_cancelled(cancel, cancel_poll) {
        return Err((PackResult::Cancelled, "cancelled".to_string()));
    }
    Ok(())
}

fn prepare_small_file(
    file_index: usize,
    source: &Path,
    zstd: &dyn ZstdCodec,
    zstd_level: i32,
    raw_threshold: u64,
) -> Result<PreparedChunkFile, (PackResult, String)> {
    let data = fs::read(source).map_err(|e| {
        (
            PackResult::IoError,
            format!("cannot read {}: {e}", source.display()),
        )
    })?;
    let size = data.len() as u64;
    let digest = sha256(&data);
    if size > 0 && size <= raw_threshold && zstd_level > 0 {
        if let Ok(compressed) = zstd.compress(&data, zstd_level) {
            if compressed.len() < data.len() {
                return Ok(PreparedChunkFile {
                    file_index,
                    size,
                    sha256: digest,
                    codec: CODEC_ZSTD,
                    payload: compressed,
                });
            }
        }
    }
    Ok(PreparedChunkFile {
        file_index,
        size,
        sha256: digest,
        codec: CODEC_RAW,
        payload: data,
    })
}

fn write_zero_pad(file: &mut File, count: u64) -> Result<(), (PackResult, String)> {
    if count == 0 {
        return Ok(());
    }
    let zeros = [0u8; 4096];
    let mut rem = count;
    while rem > 0 {
        let take = (rem as usize).min(zeros.len());
        file.write_all(&zeros[..take])
            .map_err(|e| (PackResult::IoError, format!("padding write failed: {e}")))?;
        rem -= take as u64;
    }
    Ok(())
}

fn stream_large_file(
    source: &Path,
    part_file: &mut File,
    write_offset: u64,
    cancel: Option<&AtomicBool>,
    cancel_poll: Option<&dyn Fn() -> bool>,
    base_progress: &PackProgress,
    progress_cb: &mut Option<&mut dyn FnMut(&PackProgress)>,
) -> Result<(u64, [u8; 32]), (PackResult, String)> {
    let mut src = File::open(source).map_err(|e| {
        (
            PackResult::IoError,
            format!("cannot open {}: {e}", source.display()),
        )
    })?;
    part_file
        .seek(SeekFrom::Start(write_offset))
        .map_err(|e| (PackResult::IoError, format!("seek failed: {e}")))?;

    let mut buf = vec![0u8; READ_BUFFER_SIZE];
    let mut hasher = Sha256::new();
    let mut total = 0u64;
    let mut last_report = 0u64;
    loop {
        check_cancelled(cancel, cancel_poll)?;
        let n = src.read(&mut buf).map_err(|e| {
            (
                PackResult::IoError,
                format!("read error on {}: {e}", source.display()),
            )
        })?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        part_file
            .write_all(&buf[..n])
            .map_err(|e| (PackResult::IoError, format!("write error: {e}")))?;
        total += n as u64;
        if total - last_report >= PROGRESS_STEP {
            last_report = total;
            if let Some(cb) = progress_cb.as_deref_mut() {
                let bytes_done = base_progress.bytes_done.saturating_add(total);
                let live_progress = PackProgress {
                    bytes_done,
                    bytes_total: base_progress.bytes_total.max(bytes_done),
                    files_done: base_progress.files_done,
                    files_total: base_progress.files_total,
                };
                cb(&live_progress);
            }
        }
    }
    Ok((total, hasher.finalize()))
}

fn build_toc_bytes(
    layout: &Layout,
    extents: &[ExtentRecord],
    meta_addrs: &[(u64, u32)],
) -> Vec<u8> {
    let mut toc = vec![0u8; layout.toc_size as usize];
    let package_count = layout.packages.len() as u32;
    let dir_count = layout.dirs.len() as u64;
    let file_count = layout.files.len() as u64;

    put_u32_le(&mut toc, 0x00, TOC_MAGIC);
    put_u32_le(&mut toc, 0x04, package_count);
    put_u64_le(&mut toc, 0x08, dir_count);
    put_u64_le(&mut toc, 0x10, file_count);

    let mut pos = TOC_HEADER_SIZE;
    for (i, pkg) in layout.packages.iter().enumerate() {
        put_u64_le(&mut toc, pos, pkg.title_id);
        put_u32_le(&mut toc, pos + 0x08, pkg.root_dir);
        put_u32_le(&mut toc, pos + 0x0C, pkg.exefs_dir);
        put_u32_le(&mut toc, pos + 0x10, pkg.romfs_dir);
        put_u32_le(&mut toc, pos + 0x14, pkg.version);
        toc[pos + 0x18] = pkg.kind;
        toc[pos + 0x19] = pkg.flags;
        let (meta_off, meta_sz) = meta_addrs[i];
        put_u64_le(&mut toc, pos + 0x20, meta_off);
        put_u32_le(&mut toc, pos + 0x28, meta_sz);
        pos += PACKAGE_RECORD_SIZE;
    }

    for d in &layout.dirs {
        let pool_off = layout.pool_index[&d.name];
        put_u32_le(&mut toc, pos, pool_off);
        put_u32_le(&mut toc, pos + 0x04, d.name.len() as u32);
        put_u32_le(&mut toc, pos + 0x08, d.first_child);
        put_u32_le(&mut toc, pos + 0x0C, d.child_count);
        pos += DIR_RECORD_SIZE;
    }

    for (i, ext) in extents.iter().enumerate() {
        let f = &layout.files[i];
        let pool_off = layout.pool_index[&f.name];
        put_u32_le(&mut toc, pos, pool_off);
        put_u32_le(&mut toc, pos + 0x04, f.name.len() as u32);
        put_u64_le(&mut toc, pos + 0x08, ext.size);
        if ext.size != 0 {
            put_u64_le(&mut toc, pos + 0x10, ext.offset);
            put_u64_le(&mut toc, pos + 0x18, ext.stored_size);
            toc[pos + 0x40] = ext.codec;
        }
        toc[pos + 0x20..pos + 0x40].copy_from_slice(&ext.sha256);
        pos += FILE_RECORD_SIZE;
    }

    let dir_count_u32 = dir_count as u32;
    for &child in &layout.children_u64 {
        let id = if (child & FILE_TAG_BIT) != 0 {
            dir_count_u32 + (child as u32)
        } else {
            child as u32
        };
        put_u32_le(&mut toc, pos, id);
        pos += 4;
    }

    let pool_offset = pos as u64;
    let pool_size = layout.string_pool.len() as u64;
    put_u64_le(&mut toc, 0x18, pool_offset);
    put_u64_le(&mut toc, 0x20, pool_size);
    toc[pos..pos + layout.string_pool.len()].copy_from_slice(&layout.string_pool);
    toc
}

fn remove_empty_tree(root: &Path) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };
    for entry in entries.flatten() {
        let p = entry.path();
        if p.is_dir() {
            remove_empty_tree(&p);
        }
    }
    let _ = fs::remove_dir(root);
}

fn remove_empty_parents(mut path: &Path, dump_root: &Path) {
    while !path.as_os_str().is_empty() && path != dump_root {
        if fs::remove_dir(path).is_err() {
            break;
        }
        let Some(parent) = path.parent() else {
            break;
        };
        path = parent;
    }
}

enum PackStart {
    Resumed(Plan, Vec<JournalExtent>),
    Fresh(Plan, Layout),
}

type PackWritePhaseOutput<'cb> = (Plan, PackProgress, Option<&'cb mut dyn FnMut(&PackProgress)>);

#[allow(clippy::too_many_arguments)]
fn pack_dump_write_phase<'cb>(
    dump_root: &Path,
    archive: &Path,
    part_path: &Path,
    journal_path: &Path,
    options: &PackOptions,
    zstd: &dyn ZstdCodec,
    cancel: Option<&AtomicBool>,
    cancel_poll: Option<&dyn Fn() -> bool>,
    mut progress_cb: Option<&'cb mut dyn FnMut(&PackProgress)>,
    start: PackStart,
) -> Result<PackWritePhaseOutput<'cb>, (PackResult, String)> {
    let (plan, layout, mut part_file, mut journal_writer, mut completed, mut dedup_map, mut cursor) =
        match start {
            PackStart::Resumed(saved_plan, saved_extents) => {
                let layout = build_layout(&saved_plan, dump_root)
                    .map_err(|e| (PackResult::InvalidSource, format!("corrupt journal plan: {e}")))?;
                let file = OpenOptions::new()
                    .read(true)
                    .write(true)
                    .open(part_path)
                    .map_err(|e| {
                        (
                            PackResult::IoError,
                            format!("cannot reopen {}: {e}", part_path.display()),
                        )
                    })?;
                let part_len = file
                    .metadata()
                    .map_err(|e| (PackResult::IoError, e.to_string()))?
                    .len();

                let mut completed: Vec<Option<ExtentRecord>> = vec![None; layout.files.len()];
                let mut dedup_map: HashMap<(u64, [u8; 32]), (u8, u64, u64)> = HashMap::new();
                let mut max_end = layout.blob_offset;

                for ext in saved_extents {
                    if ext.file_index >= completed.len() {
                        continue;
                    }
                    if ext.stored_size > 0 && ext.offset + ext.stored_size > part_len {
                        continue;
                    }
                    if ext.stored_size > 0 {
                        max_end = max_end.max(ext.offset + ext.stored_size);
                    }
                    dedup_map.insert(
                        (ext.size, ext.sha256),
                        (ext.codec, ext.offset, ext.stored_size),
                    );
                    completed[ext.file_index] = Some(ExtentRecord {
                        codec: ext.codec,
                        offset: ext.offset,
                        size: ext.size,
                        stored_size: ext.stored_size,
                        sha256: ext.sha256,
                    });
                }

                if part_len < layout.blob_offset {
                    file.set_len(layout.blob_offset)
                        .map_err(|e| (PackResult::IoError, e.to_string()))?;
                    max_end = layout.blob_offset;
                }
                let jfile = OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(journal_path)
                    .map_err(|e| (PackResult::IoError, format!("cannot open journal: {e}")))?;

                (
                    saved_plan,
                    layout,
                    file,
                    Some(BufWriter::new(jfile)),
                    completed,
                    dedup_map,
                    max_end,
                )
            }
            PackStart::Fresh(plan, layout) => {
                let _ = fs::remove_file(part_path);
                let _ = fs::remove_file(journal_path);

                let mut file = OpenOptions::new()
                    .create(true)
                    .truncate(true)
                    .read(true)
                    .write(true)
                    .open(part_path)
                    .map_err(|e| {
                        (
                            PackResult::IoError,
                            format!("cannot create {}: {e}", part_path.display()),
                        )
                    })?;
                write_zero_pad(&mut file, layout.blob_offset)?;
                file.flush()
                    .map_err(|e| (PackResult::IoError, e.to_string()))?;

                let journal_writer = if options.delete_sources {
                    let jfile = OpenOptions::new()
                        .create(true)
                        .truncate(true)
                        .write(true)
                        .open(journal_path)
                        .map_err(|e| {
                            (
                                PackResult::IoError,
                                format!("cannot create {}: {e}", journal_path.display()),
                            )
                        })?;
                    let mut jw = BufWriter::new(jfile);
                    writeln!(jw, "{}", plan_to_json(&plan))
                        .and_then(|_| jw.flush())
                        .map_err(|e| {
                            (
                                PackResult::IoError,
                                format!("cannot write journal header: {e}"),
                            )
                        })?;
                    Some(jw)
                } else {
                    None
                };

                let count = layout.files.len();
                let blob_offset = layout.blob_offset;
                (
                    plan,
                    layout,
                    file,
                    journal_writer,
                    vec![None; count],
                    HashMap::new(),
                    blob_offset,
                )
            }
        };

    let mut progress = PackProgress {
        bytes_done: 0,
        bytes_total: 0,
        files_done: 0,
        files_total: layout.files.len() as u64,
    };
    for (idx, f) in layout.files.iter().enumerate() {
        let sz = completed[idx].as_ref().map_or(f.size, |e| e.size);
        progress.bytes_total += sz;
        if completed[idx].is_some() {
            progress.files_done += 1;
            progress.bytes_done += sz;
        }
    }
    if let Some(cb) = progress_cb.as_deref_mut() {
        cb(&progress);
    }

    let worker_threads = std::thread::available_parallelism()
        .map(|n| n.get().clamp(1, 8))
        .unwrap_or(4);

    let mut pending_deletes: Vec<PathBuf> = Vec::new();
    let mut chunk_source_bytes = 0u64;
    let mut chunk_file_count = 0usize;

    let commit_extent = |idx: usize,
                         rec: ExtentRecord,
                         payload: Option<&[u8]>,
                         completed: &mut [Option<ExtentRecord>],
                         part_file: &mut File,
                         journal_writer: &mut Option<BufWriter<File>>,
                         cursor: &mut u64,
                         dedup_map: &mut HashMap<(u64, [u8; 32]), (u8, u64, u64)>,
                         progress: &mut PackProgress,
                         progress_cb: &mut Option<&mut dyn FnMut(&PackProgress)>|
     -> Result<(), (PackResult, String)> {
        let mut final_rec = rec;
        if final_rec.size == 0 {
            final_rec.codec = CODEC_RAW;
            final_rec.offset = 0;
            final_rec.stored_size = 0;
        } else if let Some(&(d_codec, d_off, d_stored)) =
            dedup_map.get(&(final_rec.size, final_rec.sha256))
        {
            if payload.is_none() {
                part_file
                    .set_len(*cursor)
                    .map_err(|e| (PackResult::IoError, e.to_string()))?;
            }
            final_rec.codec = d_codec;
            final_rec.offset = d_off;
            final_rec.stored_size = d_stored;
        } else {
            let start = align_up(*cursor, EXTENT_ALIGNMENT);
            if start > *cursor {
                part_file
                    .seek(SeekFrom::Start(*cursor))
                    .map_err(|e| (PackResult::IoError, e.to_string()))?;
                write_zero_pad(part_file, start - *cursor)?;
            }
            if let Some(bytes) = payload {
                part_file
                    .seek(SeekFrom::Start(start))
                    .map_err(|e| (PackResult::IoError, e.to_string()))?;
                part_file
                    .write_all(bytes)
                    .map_err(|e| (PackResult::IoError, format!("write extent failed: {e}")))?;
                final_rec.stored_size = bytes.len() as u64;
            }
            final_rec.offset = start;
            *cursor = start + final_rec.stored_size;
            dedup_map.insert(
                (final_rec.size, final_rec.sha256),
                (final_rec.codec, final_rec.offset, final_rec.stored_size),
            );
        }

        if let Some(jw) = journal_writer.as_mut() {
            let jline = JournalExtent {
                file_index: idx,
                codec: final_rec.codec,
                offset: final_rec.offset,
                size: final_rec.size,
                stored_size: final_rec.stored_size,
                sha256: final_rec.sha256,
            };
            writeln!(jw, "{}", extent_to_json(&jline))
                .map_err(|e| (PackResult::IoError, format!("journal write failed: {e}")))?;
        }

        progress.files_done += 1;
        progress.bytes_done += final_rec.size;
        completed[idx] = Some(final_rec);
        if let Some(cb) = progress_cb.as_deref_mut() {
            cb(progress);
        }
        Ok(())
    };

    let flush_chunk = |part_file: &mut File,
                       journal_writer: &mut Option<BufWriter<File>>,
                       pending_deletes: &mut Vec<PathBuf>,
                       chunk_source_bytes: &mut u64,
                       chunk_file_count: &mut usize|
     -> Result<(), (PackResult, String)> {
        part_file
            .flush()
            .map_err(|e| (PackResult::IoError, format!("flush part failed: {e}")))?;
        if let Some(jw) = journal_writer.as_mut() {
            jw.flush()
                .map_err(|e| (PackResult::IoError, format!("flush journal failed: {e}")))?;
        }
        if options.delete_sources {
            for p in pending_deletes.drain(..) {
                let _ = fs::remove_file(&p);
            }
        } else {
            pending_deletes.clear();
        }
        *chunk_source_bytes = 0;
        *chunk_file_count = 0;
        Ok(())
    };

    let mut i = 0usize;
    while i < layout.files.len() {
        if completed[i].is_some() {
            i += 1;
            continue;
        }
        check_cancelled(cancel, cancel_poll)?;

        let file_info = &layout.files[i];
        let can_batch_parallel = worker_threads > 1
            && file_info.size <= PARALLEL_MAX_FILE_SIZE
            && file_info.size <= options.raw_threshold
            && !options.delete_sources;

        if can_batch_parallel {
            let mut batch_indices: Vec<usize> = Vec::new();
            let mut batch_bytes = 0u64;
            let mut j = i;
            while j < layout.files.len() && batch_indices.len() < worker_threads * 2 {
                if completed[j].is_some() {
                    break;
                }
                let f = &layout.files[j];
                if f.size > PARALLEL_MAX_FILE_SIZE || f.size > options.raw_threshold {
                    break;
                }
                if !batch_indices.is_empty() && batch_bytes + f.size > PARALLEL_BATCH_BYTES {
                    break;
                }
                batch_indices.push(j);
                batch_bytes += f.size;
                j += 1;
            }

            if batch_indices.len() > 1 {
                let results: Vec<Result<PreparedChunkFile, (PackResult, String)>> =
                    std::thread::scope(|s| {
                        let mut handles = Vec::with_capacity(batch_indices.len());
                        for &idx in &batch_indices {
                            let src = &layout.files[idx].source_path;
                            handles.push(s.spawn(move || {
                                prepare_small_file(
                                    idx,
                                    src,
                                    zstd,
                                    options.zstd_level,
                                    options.raw_threshold,
                                )
                            }));
                        }
                        handles
                            .into_iter()
                            .map(|h| {
                                h.join().unwrap_or_else(|_| {
                                    Err((PackResult::IoError, "worker thread panicked".to_string()))
                                })
                            })
                            .collect()
                    });

                for item in results {
                    check_cancelled(cancel, cancel_poll)?;
                    let prep = item?;
                    let rec = ExtentRecord {
                        codec: prep.codec,
                        offset: 0,
                        size: prep.size,
                        stored_size: prep.payload.len() as u64,
                        sha256: prep.sha256,
                    };
                    commit_extent(
                        prep.file_index,
                        rec,
                        Some(&prep.payload),
                        &mut completed,
                        &mut part_file,
                        &mut journal_writer,
                        &mut cursor,
                        &mut dedup_map,
                        &mut progress,
                        &mut progress_cb,
                    )?;
                    if let Some(jw) = journal_writer.as_mut() {
                        jw.flush()
                            .map_err(|e| (PackResult::IoError, e.to_string()))?;
                    }
                }
                i = j;
                continue;
            }
        }

        if file_info.size > options.raw_threshold || file_info.size > PARALLEL_MAX_FILE_SIZE {
            let start = align_up(cursor, EXTENT_ALIGNMENT);
            if start > cursor {
                part_file
                    .seek(SeekFrom::Start(cursor))
                    .map_err(|e| (PackResult::IoError, e.to_string()))?;
                write_zero_pad(&mut part_file, start - cursor)?;
            }
            let (actual_size, digest) = stream_large_file(
                &file_info.source_path,
                &mut part_file,
                start,
                cancel,
                cancel_poll,
                &progress,
                &mut progress_cb,
            )?;
            let rec = ExtentRecord {
                codec: CODEC_RAW,
                offset: start,
                size: actual_size,
                stored_size: actual_size,
                sha256: digest,
            };
            commit_extent(
                i,
                rec,
                None,
                &mut completed,
                &mut part_file,
                &mut journal_writer,
                &mut cursor,
                &mut dedup_map,
                &mut progress,
                &mut progress_cb,
            )?;
            chunk_source_bytes += actual_size;
        } else {
            let prep = prepare_small_file(
                i,
                &file_info.source_path,
                zstd,
                options.zstd_level,
                options.raw_threshold,
            )?;
            let rec = ExtentRecord {
                codec: prep.codec,
                offset: 0,
                size: prep.size,
                stored_size: prep.payload.len() as u64,
                sha256: prep.sha256,
            };
            commit_extent(
                i,
                rec,
                Some(&prep.payload),
                &mut completed,
                &mut part_file,
                &mut journal_writer,
                &mut cursor,
                &mut dedup_map,
                &mut progress,
                &mut progress_cb,
            )?;
            chunk_source_bytes += prep.size;
        }

        if options.delete_sources {
            pending_deletes.push(file_info.source_path.clone());
            chunk_file_count += 1;
            let limit_bytes = options.chunk_bytes.max(1);
            if chunk_source_bytes >= limit_bytes || chunk_file_count >= MAX_CHUNK_FILES {
                flush_chunk(
                    &mut part_file,
                    &mut journal_writer,
                    &mut pending_deletes,
                    &mut chunk_source_bytes,
                    &mut chunk_file_count,
                )?;
            }
        } else if let Some(jw) = journal_writer.as_mut() {
            jw.flush()
                .map_err(|e| (PackResult::IoError, e.to_string()))?;
        }

        i += 1;
    }

    flush_chunk(
        &mut part_file,
        &mut journal_writer,
        &mut pending_deletes,
        &mut chunk_source_bytes,
        &mut chunk_file_count,
    )?;

    check_cancelled(cancel, cancel_poll)?;

    let final_extents: Vec<ExtentRecord> = completed
        .into_iter()
        .map(|opt| opt.ok_or_else(|| (PackResult::IoError, "missing file extent".to_string())))
        .collect::<Result<_, _>>()?;

    // Append package TLV metadata blocks directly after the last extent.
    let mut meta_addrs: Vec<(u64, u32)> = Vec::with_capacity(layout.packages.len());
    part_file
        .seek(SeekFrom::Start(cursor))
        .map_err(|e| (PackResult::IoError, e.to_string()))?;
    for pkg in &layout.packages {
        if pkg.meta.is_empty() {
            meta_addrs.push((0, 0));
        } else {
            let len_u32 = u32::try_from(pkg.meta.len())
                .map_err(|_| (PackResult::IoError, "package metadata too large".to_string()))?;
            part_file
                .write_all(&pkg.meta)
                .map_err(|e| (PackResult::IoError, format!("meta write failed: {e}")))?;
            meta_addrs.push((cursor, len_u32));
            cursor += pkg.meta.len() as u64;
        }
    }
    let total_file_size = cursor;
    part_file
        .set_len(total_file_size)
        .map_err(|e| (PackResult::IoError, format!("truncate failed: {e}")))?;

    let toc = build_toc_bytes(&layout, &final_extents, &meta_addrs);
    let toc_sha = sha256(&toc);

    // Superblock UUID: SHA-256 over title_id (u64), version (u32), kind (u8) of each package.
    let mut uuid_hash = Sha256::new();
    for pkg in &plan.packages {
        let mut identity = [0u8; 13];
        put_u64_le(&mut identity, 0, pkg.title_id);
        put_u32_le(&mut identity, 8, pkg.version);
        identity[12] = pkg.kind;
        uuid_hash.update(&identity);
    }
    let uuid = uuid_hash.finalize();

    let mut super_buf = vec![0u8; SUPERBLOCK_SIZE];
    super_buf[0x00..0x04].copy_from_slice(&HEADER_MAGIC);
    put_u16_le(&mut super_buf, 0x04, SUPERBLOCK_SIZE as u16);
    put_u16_le(&mut super_buf, 0x06, 1);
    put_u64_le(&mut super_buf, 0x08, SUPERBLOCK_SIZE as u64);
    put_u64_le(&mut super_buf, 0x10, toc.len() as u64);
    super_buf[0x18..0x38].copy_from_slice(&toc_sha);
    put_u64_le(&mut super_buf, 0x38, layout.blob_offset);
    put_u64_le(&mut super_buf, 0x40, total_file_size);
    put_u32_le(&mut super_buf, 0x48, plan.packages.len() as u32);
    put_u32_le(&mut super_buf, 0x4C, 0);
    super_buf[0x50..0x60].copy_from_slice(&uuid[..16]);

    part_file
        .seek(SeekFrom::Start(0))
        .map_err(|e| (PackResult::IoError, e.to_string()))?;
    part_file
        .write_all(&super_buf)
        .map_err(|e| (PackResult::IoError, format!("superblock write failed: {e}")))?;
    part_file
        .write_all(&toc)
        .map_err(|e| (PackResult::IoError, format!("TOC write failed: {e}")))?;
    part_file
        .sync_all()
        .map_err(|e| (PackResult::IoError, format!("sync failed: {e}")))?;

    drop(part_file);
    drop(journal_writer);

    fs::rename(part_path, archive)
        .map_err(|e| (PackResult::IoError, format!("rename failed: {e}")))?;
    Ok((plan, progress, progress_cb))
}

/// Packs a loose Switch dump directory into a `.nup` archive (resumable via `.part` + `.journal`).
pub fn pack_dump(
    dump_root: &Path,
    archive: &Path,
    options: &PackOptions,
    zstd: &dyn ZstdCodec,
    cancel: Option<&AtomicBool>,
    progress_cb: Option<&mut dyn FnMut(&PackProgress)>,
) -> Result<(), (PackResult, String)> {
    pack_dump_with_cancel_poll(dump_root, archive, options, zstd, cancel, None, progress_cb)
}

pub(crate) fn pack_dump_with_cancel_poll(
    dump_root: &Path,
    archive: &Path,
    options: &PackOptions,
    zstd: &dyn ZstdCodec,
    cancel: Option<&AtomicBool>,
    cancel_poll: Option<&dyn Fn() -> bool>,
    progress_cb: Option<&mut dyn FnMut(&PackProgress)>,
) -> Result<(), (PackResult, String)> {
    check_cancelled(cancel, cancel_poll)?;
    if archive.exists() {
        return Err((
            PackResult::IoError,
            format!("{} already exists", archive.display()),
        ));
    }

    let part_path = part_path_for(archive);
    let journal_path = journal_path_for(archive);

    if let Some(parent) = archive.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| {
                (
                    PackResult::IoError,
                    format!("cannot create parent dir {}: {e}", parent.display()),
                )
            })?;
        }
    }

    let resumed = if options.delete_sources && journal_path.is_file() && part_path.is_file() {
        parse_journal_file(&journal_path).ok()
    } else {
        None
    };

    let excludes = vec![
        normalize_cmp_path(archive),
        normalize_cmp_path(&part_path),
        normalize_cmp_path(&journal_path),
    ];

    let start = if let Some((saved_plan, saved_extents)) = resumed {
        PackStart::Resumed(saved_plan, saved_extents)
    } else {
        if !is_loose_dump(dump_root) {
            return Err((
                PackResult::InvalidSource,
                format!("not a valid loose dump: {}", dump_root.display()),
            ));
        }
        let plan = Discovery::new(dump_root, excludes, options).run()?;
        let layout = build_layout(&plan, dump_root).map_err(|e| (PackResult::InvalidSource, e))?;
        PackStart::Fresh(plan, layout)
    };

    let write_res = pack_dump_write_phase(
        dump_root,
        archive,
        &part_path,
        &journal_path,
        options,
        zstd,
        cancel,
        cancel_poll,
        progress_cb,
        start,
    );
    if write_res.is_err() && !options.delete_sources {
        // File handles from pack_dump_write_phase are dropped before cleanup for Windows.
        let _ = fs::remove_file(&part_path);
        let _ = fs::remove_file(&journal_path);
    }
    let (plan, mut progress, progress_cb) = write_res?;
    let _ = fs::remove_file(&journal_path);

    if options.delete_sources {
        for entry in &plan.entries {
            if !entry.directory {
                let _ = fs::remove_file(dump_root.join(&entry.source));
            }
        }
        for entry in &plan.entries {
            let depth = entry.path.bytes().filter(|&b| b == b'/').count();
            let top =
                entry.directory && (depth == 0 || (depth == 1 && entry.path.starts_with("data/")));
            if top {
                let source = dump_root.join(&entry.source);
                remove_empty_tree(&source);
                if let Some(parent) = source.parent() {
                    remove_empty_parents(parent, dump_root);
                }
            }
        }
    }

    progress.bytes_done = progress.bytes_total;
    progress.files_done = progress.files_total;
    if let Some(cb) = progress_cb {
        cb(&progress);
    }

    Ok(())
}
