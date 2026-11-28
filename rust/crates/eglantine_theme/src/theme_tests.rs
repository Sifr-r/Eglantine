// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use super::*;
use std::io::{Cursor, Write};

#[test]
fn component_overrides_round_trip_and_keep_other_consoles() {
    use eglantine_library::ConsoleId::{GameBoy, GameBoyAdvance};
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("component.eglantine-theme");
    let mut package = ThemePackage::default();
    let list = ThemeComponent::LibraryList;
    package.theme.version = 2;
    package.theme.components.insert(list, ComponentStyle { padding: Some(12), ..Default::default() });
    package.theme.consoles.entry(GameBoyAdvance.as_str().into()).or_default()
        .insert(list, ComponentStyle { radius: Some(4), background: Some(Color(0xc7da94)), ..Default::default() });
    package.theme.console_layouts.insert(GameBoyAdvance.as_str().into(), ConsoleLayout::Grid);
    package.export(&path).unwrap();
    let imported = ThemePackage::import(&path).unwrap();
    assert_eq!(package, imported);
    let gba = imported.theme.component(Some(GameBoyAdvance), list);
    assert_eq!(gba.padding, Some(12));
    assert_eq!(gba.radius, Some(4));
    assert_eq!(gba.background, Some(Color(0xc7da94)));
    assert_eq!(imported.theme.component(Some(GameBoy), list).background, Some(Color(0xbbcc86)));
    assert_eq!(imported.theme.console_layout(GameBoyAdvance), eglantine_library::LibraryLayout::Grid);
}

#[test]
fn component_validation_rejects_inaccessible_and_unknown_overrides() {
    let mut theme = Theme::default();
    let part = ThemeComponent::LibraryList;
    theme.components.insert(part, ComponentStyle { focus: Some(Color(0xbed28e)), ..Default::default() });
    assert!(theme.validate_draft().is_err());
    theme.version = 2;
    assert!(theme.validate_draft().is_ok());
    assert!(theme.validate().is_err());
    theme.components.clear();
    theme.consoles.insert("unknown".into(), BTreeMap::new());
    assert!(theme.validate_draft().is_err());
    theme.consoles.clear();
    theme.console_layouts.insert("switch-2".into(), ConsoleLayout::Grid);
    assert!(theme.validate_draft().is_err());
}

#[test]
fn default_theme_matches_approved_home() {
    let theme = Theme::default();
    theme.validate().unwrap();
    assert_eq!(theme.style.background, Color(0x201d1a));
    assert_eq!(theme.layout.home_mode, HomeMode::Shelf);
    assert_eq!(theme.layout.home_shortcuts, DEFAULT_SHORTCUTS);
}

#[test]
fn partial_manifest_inherits_defaults() {
    let theme: Theme = serde_json::from_str(
        r##"{"version":1,"id":"ocean","name":"Ocean","style":{"background":"#102030"},"layout":{"home_mode":"grid"}}"##,
    )
    .unwrap();
    theme.validate().unwrap();
    assert_eq!(theme.style.background, Color(0x102030));
    assert_eq!(theme.style.corner_radius, ThemeStyle::default().corner_radius);
    assert_eq!(theme.layout.home_mode, HomeMode::Grid);
    assert_eq!(theme.layout.home_shortcuts, DEFAULT_SHORTCUTS);
}

#[test]
fn rejects_missing_settings_and_low_contrast_focus() {
    let mut theme = Theme::default();
    theme.layout.home_shortcuts.retain(|id| *id != ComponentId::Settings);
    assert!(theme.validate().is_err());
    theme = Theme::default();
    theme.style.focus_end = theme.style.panel;
    theme.validate_draft().unwrap();
    assert!(theme.validate().is_err());
}

#[test]
fn theme_round_trips_with_assets() {
    let tmp = tempfile::tempdir().unwrap();
    let mut source = ThemePackage::default();
    let mut icon = Cursor::new(Vec::new());
    image::RgbImage::new(2, 2).write_to(&mut icon, image::ImageFormat::Png).unwrap();
    source.theme.name = "Ocean".into();
    source.theme.id = "ocean".into();
    source.set_asset("waves", AssetKind::Png, icon.into_inner()).unwrap();
    source.theme.style.background_asset = Some("waves".into());
    source.theme.layout.home_navigation = Placement::Left;
    let path = tmp.path().join("ocean.eglantine-theme");
    source.export(&path).unwrap();
    let loaded = ThemePackage::import(&path).unwrap();
    assert_eq!(loaded, source);
    assert!(loaded.asset("waves").is_some());
}

#[test]
fn rejects_path_traversal_and_missing_assets() {
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    writer.start_file("../theme.json", zip::write::SimpleFileOptions::default()).unwrap();
    writer.write_all(b"{}").unwrap();
    let data = writer.finish().unwrap();
    assert!(ThemePackage::read_package(data).is_err());

    let mut source = ThemePackage::default();
    source.theme.style.background_asset = Some("missing".into());
    assert!(source.validate().is_err());
    assert!(source.set_asset("bad", AssetKind::Png, b"not a PNG".to_vec()).is_err());
    assert!(source.asset("bad").is_none());
}

#[test]
fn rejects_duplicate_archive_entries() {
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    for name in ["theme.json", "other.json"] {
        writer.start_file(name, zip::write::SimpleFileOptions::default()).unwrap();
        writer.write_all(&serde_json::to_vec(&Theme::default()).unwrap()).unwrap();
    }
    let mut bytes = writer.finish().unwrap().into_inner();
    // Rename the second entry in its local and central headers without changing their sizes.
    for offset in (0..bytes.len() - 10).rev() {
        if &bytes[offset..offset + 10] == b"other.json" {
            bytes[offset..offset + 10].copy_from_slice(b"theme.json");
        }
    }
    assert!(ThemePackage::read_package(Cursor::new(bytes)).is_err());
}

#[test]
fn invalid_apply_preserves_active_theme_and_safe_theme_is_temporary() {
    let tmp = tempfile::tempdir().unwrap();
    let mut source = ThemePackage::default();
    source.theme.name = "Custom".into();
    source.save_active(tmp.path()).unwrap();
    assert_eq!(ThemePackage::load_active(tmp.path(), false).unwrap(), source);
    source.theme.version = 99;
    assert!(source.save_active(tmp.path()).is_err());
    assert_eq!(ThemePackage::load_active(tmp.path(), false).unwrap().theme.name, "Custom");
    assert_eq!(ThemePackage::load_active(tmp.path(), true).unwrap(), ThemePackage::default());
    assert_eq!(ThemePackage::load_active(tmp.path(), false).unwrap().theme.name, "Custom");
    ThemePackage::restore_default(tmp.path()).unwrap();
    assert_eq!(ThemePackage::load_active(tmp.path(), false).unwrap(), ThemePackage::default());
}

#[test]
fn duplicate_produces_valid_distinct_theme() {
    let original = Theme::default();
    let copy = original.duplicate();
    copy.validate().unwrap();
    assert_ne!(copy.id, original.id);
    assert_ne!(copy.name, original.name);
    assert!(copy.name.ends_with("Copy"));

    let long = Theme {
        id: "a".repeat(48),
        name: "Long Theme Name ".repeat(5),
        ..Default::default()
    };
    let long_copy = long.duplicate();
    long_copy.validate().unwrap();
}
