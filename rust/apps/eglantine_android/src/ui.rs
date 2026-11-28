// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::ffi::{CStr, CString, c_char};

use crate::state::{App, VISIBLE_GAME_ROWS, ViewMode};

pub(super) const VIEWPORT_WIDTH: i32 = 480;
pub(super) const VIEWPORT_HEIGHT: i32 = 320;

const WRAP_COLUMNS: usize = 54;
const ROW_TEXT_COLUMNS: usize = 50;
const LINE_HEIGHT: f32 = 13.0;

const DRAW_FILL_RECT: i32 = 0;
const DRAW_TEXT: i32 = 1;

const ACTION_NONE: i32 = 0;
const ACTION_OPEN_DOCUMENT: i32 = 1;
const ACTION_LAUNCH: i32 = 2;
const ACTION_ADD_FOLDER: i32 = 3;
const ACTION_INSTALL_KEYS: i32 = 4;
const ACTION_INSTALL_DRIVER: i32 = 5;
const ACTION_CLEAR_DRIVER: i32 = 6;
const ACTION_APPLY_SETTINGS: i32 = 7;

const KEY_CONFIRM: i32 = 1;
const KEY_LAUNCH: i32 = 2;

static KEYS_READY: &CStr = c"Keys: Ready";
static KEYS_MISSING: &CStr = c"Keys: Missing";

const BG_COLOR: Color = Color { r: 22, g: 27, b: 35, a: 255 };
const PANEL_BG_COLOR: Color = Color { r: 28, g: 35, b: 45, a: 255 };
const ROW_BG_COLOR: Color = Color { r: 35, g: 43, b: 56, a: 255 };
const ROW_SELECTED_COLOR: Color = Color { r: 53, g: 88, b: 118, a: 255 };
const BUTTON_ENABLED_COLOR: Color = Color { r: 46, g: 77, b: 103, a: 255 };
const BUTTON_DISABLED_COLOR: Color = Color { r: 38, g: 44, b: 55, a: 255 };
const TEXT_COLOR: Color = Color { r: 239, g: 244, b: 247, a: 255 };
const MUTED_TEXT_COLOR: Color = Color { r: 170, g: 184, b: 196, a: 255 };

#[derive(Clone, Copy)]
struct Rect {
    x: f32,
    y: f32,
    w: f32,
    h: f32,
}

impl Rect {
    const fn contains(self, px: f32, py: f32) -> bool {
        px >= self.x && px < self.x + self.w && py >= self.y && py < self.y + self.h
    }
}

const VIEW_TOGGLE_BUTTON: Rect = Rect { x: 244.0, y: 8.0, w: 96.0, h: 22.0 };

const PICKER_BUTTON: Rect = Rect { x: 16.0, y: 52.0, w: 106.0, h: 34.0 };
const FOLDER_BUTTON: Rect = Rect { x: 130.0, y: 52.0, w: 106.0, h: 34.0 };
const KEYS_BUTTON: Rect = Rect { x: 244.0, y: 52.0, w: 106.0, h: 34.0 };
const LAUNCH_BUTTON: Rect = Rect { x: 358.0, y: 52.0, w: 106.0, h: 34.0 };

const LIBRARY_PANEL: Rect = Rect { x: 16.0, y: 96.0, w: 448.0, h: 156.0 };
const PREV_BUTTON: Rect = Rect { x: 340.0, y: 228.0, w: 56.0, h: 20.0 };
const NEXT_BUTTON: Rect = Rect { x: 402.0, y: 228.0, w: 56.0, h: 20.0 };

const DRIVER_PICK_BUTTON: Rect = Rect { x: 312.0, y: 228.0, w: 76.0, h: 20.0 };
const DRIVER_RESET_BUTTON: Rect = Rect { x: 394.0, y: 228.0, w: 64.0, h: 20.0 };

const ROW_X: f32 = 22.0;
const ROW_START_Y: f32 = 102.0;
const ROW_WIDTH: f32 = 436.0;
const ROW_HEIGHT: f32 = 22.0;
const ROW_STEP: f32 = 25.0;

const SETTING_ROWS: usize = 6;
const SETTING_ROW_HEIGHT: f32 = 19.0;
const SETTING_ROW_STEP: f32 = 21.0;
const SETTING_START_Y: f32 = 100.0;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DrawCmd {
    pub kind: i32,
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub color: Color,
    pub text: *const c_char,
}

#[derive(Default)]
pub(super) struct FrameDrawList {
    _texts: Vec<CString>,
    cmds: Vec<DrawCmd>,
}

impl FrameDrawList {
    pub(super) fn as_ptr(&self) -> *const DrawCmd {
        self.cmds.as_ptr()
    }

    pub(super) fn len(&self) -> usize {
        self.cmds.len()
    }
}

pub(super) fn handle_pointer_up(app: &mut App, x: f32, y: f32) -> i32 {
    if VIEW_TOGGLE_BUTTON.contains(x, y) {
        app.toggle_view();
        return ACTION_NONE;
    }
    if PICKER_BUTTON.contains(x, y) {
        return ACTION_OPEN_DOCUMENT;
    }
    if FOLDER_BUTTON.contains(x, y) {
        return ACTION_ADD_FOLDER;
    }
    if KEYS_BUTTON.contains(x, y) {
        return ACTION_INSTALL_KEYS;
    }
    if app.can_launch() && LAUNCH_BUTTON.contains(x, y) {
        return ACTION_LAUNCH;
    }

    match app.view() {
        ViewMode::Library => handle_library_pointer_up(app, x, y),
        ViewMode::Settings => handle_settings_pointer_up(app, x, y),
    }
}

fn handle_library_pointer_up(app: &mut App, x: f32, y: f32) -> i32 {
    let game_count = app.visible_game_count();
    if game_count > VISIBLE_GAME_ROWS {
        if PREV_BUTTON.contains(x, y) {
            app.scroll_games(-(VISIBLE_GAME_ROWS as isize));
            return ACTION_NONE;
        }
        if NEXT_BUTTON.contains(x, y) {
            app.scroll_games(VISIBLE_GAME_ROWS as isize);
            return ACTION_NONE;
        }
    }

    let scroll = app.scroll_offset();
    for slot in 0..VISIBLE_GAME_ROWS {
        let index = scroll + slot;
        if index >= game_count {
            break;
        }
        if row_rect(slot).contains(x, y) {
            app.select_game_index(index);
            return ACTION_NONE;
        }
    }
    ACTION_NONE
}

fn handle_settings_pointer_up(app: &mut App, x: f32, y: f32) -> i32 {
    for slot in 0..SETTING_ROWS {
        if setting_row_rect(slot).contains(x, y) {
            let s = app.settings_mut();
            match slot {
                0 => s.toggle_docked(),
                1 => s.toggle_async_gpu(),
                2 => s.toggle_shader_cache(),
                3 => s.toggle_vsync(),
                4 => s.cycle_resolution(),
                5 => s.cycle_gpu_accuracy(),
                _ => {}
            }
            return ACTION_APPLY_SETTINGS;
        }
    }
    if DRIVER_PICK_BUTTON.contains(x, y) {
        return ACTION_INSTALL_DRIVER;
    }
    if DRIVER_RESET_BUTTON.contains(x, y) {
        app.clear_driver();
        return ACTION_CLEAR_DRIVER;
    }
    ACTION_NONE
}

pub(super) fn key_action(can_launch: bool, key: i32) -> i32 {
    match key {
        KEY_CONFIRM => ACTION_OPEN_DOCUMENT,
        KEY_LAUNCH if can_launch => ACTION_LAUNCH,
        _ => ACTION_NONE,
    }
}

pub(super) fn build_frame(
    app: &App,
    title: &'static CStr,
    instruction: &'static CStr,
    picker_label: &'static CStr,
    launch_label: &'static CStr,
) -> FrameDrawList {
    let mut texts = Vec::new();
    let mut cmds = Vec::with_capacity(32);

    let keys_badge = if app.has_prod_keys() {
        KEYS_READY
    } else {
        KEYS_MISSING
    };
    let toggle_label = match app.view() {
        ViewMode::Library => c"Settings",
        ViewMode::Settings => c"Library",
    };

    let launch_color = if app.can_launch() {
        BUTTON_ENABLED_COLOR
    } else {
        BUTTON_DISABLED_COLOR
    };

    cmds.push(fill_rect(
        Rect { x: 0.0, y: 0.0, w: VIEWPORT_WIDTH as f32, h: VIEWPORT_HEIGHT as f32 },
        BG_COLOR,
    ));
    cmds.push(fill_rect(VIEW_TOGGLE_BUTTON, BUTTON_ENABLED_COLOR));
    cmds.push(fill_rect(PICKER_BUTTON, BUTTON_ENABLED_COLOR));
    cmds.push(fill_rect(FOLDER_BUTTON, BUTTON_ENABLED_COLOR));
    cmds.push(fill_rect(KEYS_BUTTON, BUTTON_ENABLED_COLOR));
    cmds.push(fill_rect(LAUNCH_BUTTON, launch_color));
    cmds.push(fill_rect(LIBRARY_PANEL, PANEL_BG_COLOR));

    cmds.push(draw_text(16.0, 14.0, TEXT_COLOR, title.as_ptr()));
    cmds.push(draw_text(
        VIEW_TOGGLE_BUTTON.x + 16.0,
        15.0,
        TEXT_COLOR,
        toggle_label.as_ptr(),
    ));
    cmds.push(draw_text(358.0, 14.0, TEXT_COLOR, keys_badge.as_ptr()));
    cmds.push(draw_text(16.0, 34.0, MUTED_TEXT_COLOR, instruction.as_ptr()));

    cmds.push(draw_text(PICKER_BUTTON.x + 8.0, 65.0, TEXT_COLOR, picker_label.as_ptr()));
    cmds.push(draw_text(FOLDER_BUTTON.x + 8.0, 65.0, TEXT_COLOR, c"Add folder".as_ptr()));
    cmds.push(draw_text(KEYS_BUTTON.x + 8.0, 65.0, TEXT_COLOR, c"Install keys".as_ptr()));
    cmds.push(draw_text(LAUNCH_BUTTON.x + 8.0, 65.0, TEXT_COLOR, launch_label.as_ptr()));

    let mut deferred_texts: Vec<(f32, f32, Color, usize)> = Vec::new();
    match app.view() {
        ViewMode::Library => build_library_panel(app, &mut texts, &mut cmds, &mut deferred_texts),
        ViewMode::Settings => build_settings_panel(app, &mut texts, &mut cmds, &mut deferred_texts),
    }

    let status_start_index = texts.len();
    let mut status_ys = Vec::new();
    push_wrapped_lines(
        app.status().to_string_lossy().as_ref(),
        264.0,
        &mut texts,
        &mut status_ys,
    );

    for (x, y, color, text_idx) in deferred_texts {
        cmds.push(draw_text(x, y, color, texts[text_idx].as_ptr()));
    }
    for (&y, text) in status_ys.iter().zip(&texts[status_start_index..]) {
        cmds.push(draw_text(16.0, y, TEXT_COLOR, text.as_ptr()));
    }

    FrameDrawList { _texts: texts, cmds }
}

fn build_library_panel(
    app: &App,
    texts: &mut Vec<CString>,
    cmds: &mut Vec<DrawCmd>,
    deferred_texts: &mut Vec<(f32, f32, Color, usize)>,
) {
    let visible_count = app.visible_game_count();
    let scroll = app.scroll_offset();
    let selected = app.selected_game_index();

    if visible_count == 0 {
        cmds.push(draw_text(
            24.0,
            116.0,
            MUTED_TEXT_COLOR,
            c"No games in library yet. Use Open file or Add folder.".as_ptr(),
        ));
        return;
    }

    for slot in 0..VISIBLE_GAME_ROWS {
        let index = scroll + slot;
        let Some((_, game)) = app.visible_game_at(index) else {
            break;
        };
        let label = format_row_label(game.title.to_string_lossy().as_ref());
        if let Ok(cstr) = CString::new(label) {
            let rect = row_rect(slot);
            let color = if selected == Some(index) {
                ROW_SELECTED_COLOR
            } else {
                ROW_BG_COLOR
            };
            cmds.push(fill_rect(rect, color));
            let text_idx = texts.len();
            texts.push(cstr);
            deferred_texts.push((rect.x + 8.0, rect.y + 7.0, TEXT_COLOR, text_idx));
        }
    }

    if visible_count > VISIBLE_GAME_ROWS {
        let range_end = (scroll + VISIBLE_GAME_ROWS).min(visible_count);
        cmds.push(fill_rect(PREV_BUTTON, BUTTON_ENABLED_COLOR));
        cmds.push(fill_rect(NEXT_BUTTON, BUTTON_ENABLED_COLOR));
        cmds.push(draw_text(PREV_BUTTON.x + 12.0, 234.0, TEXT_COLOR, c"Prev".as_ptr()));
        cmds.push(draw_text(NEXT_BUTTON.x + 12.0, 234.0, TEXT_COLOR, c"Next".as_ptr()));
        if let Ok(page_text) =
            CString::new(format!("Games {}-{} of {}", scroll + 1, range_end, visible_count))
        {
            let text_idx = texts.len();
            texts.push(page_text);
            deferred_texts.push((22.0, 234.0, MUTED_TEXT_COLOR, text_idx));
        }
    }
}

fn build_settings_panel(
    app: &App,
    texts: &mut Vec<CString>,
    cmds: &mut Vec<DrawCmd>,
    deferred_texts: &mut Vec<(f32, f32, Color, usize)>,
) {
    let s = app.settings();
    let labels = [
        format!("Mode: {}", if s.use_docked_mode { "Docked" } else { "Handheld" }),
        format!("Async GPU: {}", if s.async_gpu { "On" } else { "Off" }),
        format!(
            "Disk Shader Cache: {}",
            if s.disk_shader_cache { "On" } else { "Off" }
        ),
        format!("VSync: {}", if s.use_vsync { "On" } else { "Off" }),
        format!("Resolution: {}", s.resolution_label()),
        format!("GPU Accuracy: {}", s.gpu_accuracy_label()),
    ];

    for (slot, label) in labels.into_iter().enumerate() {
        let rect = setting_row_rect(slot);
        cmds.push(fill_rect(rect, ROW_BG_COLOR));
        if let Ok(cstr) = CString::new(label) {
            let text_idx = texts.len();
            texts.push(cstr);
            deferred_texts.push((rect.x + 8.0, rect.y + 6.0, TEXT_COLOR, text_idx));
        }
    }

    cmds.push(fill_rect(DRIVER_PICK_BUTTON, BUTTON_ENABLED_COLOR));
    cmds.push(fill_rect(DRIVER_RESET_BUTTON, BUTTON_ENABLED_COLOR));
    cmds.push(draw_text(DRIVER_PICK_BUTTON.x + 10.0, 234.0, TEXT_COLOR, c"Pick .so".as_ptr()));
    cmds.push(draw_text(DRIVER_RESET_BUTTON.x + 12.0, 234.0, TEXT_COLOR, c"Reset".as_ptr()));

    let driver_label = format_row_label(&format!(
        "Driver: {}",
        app.driver_name().to_string_lossy()
    ));
    if let Ok(cstr) = CString::new(driver_label) {
        let text_idx = texts.len();
        texts.push(cstr);
        deferred_texts.push((22.0, 234.0, MUTED_TEXT_COLOR, text_idx));
    }
}

const fn row_rect(slot: usize) -> Rect {
    Rect {
        x: ROW_X,
        y: ROW_START_Y + (slot as f32) * ROW_STEP,
        w: ROW_WIDTH,
        h: ROW_HEIGHT,
    }
}

const fn setting_row_rect(slot: usize) -> Rect {
    Rect {
        x: ROW_X,
        y: SETTING_START_Y + (slot as f32) * SETTING_ROW_STEP,
        w: ROW_WIDTH,
        h: SETTING_ROW_HEIGHT,
    }
}

fn fill_rect(rect: Rect, color: Color) -> DrawCmd {
    DrawCmd {
        kind: DRAW_FILL_RECT,
        x: rect.x,
        y: rect.y,
        w: rect.w,
        h: rect.h,
        color,
        text: std::ptr::null(),
    }
}

fn draw_text(x: f32, y: f32, color: Color, text: *const c_char) -> DrawCmd {
    DrawCmd {
        kind: DRAW_TEXT,
        x,
        y,
        w: 0.0,
        h: 0.0,
        color,
        text,
    }
}

fn format_row_label(title: &str) -> String {
    let mut out = String::new();
    let mut chars = title.chars();
    out.extend(chars.by_ref().take(ROW_TEXT_COLUMNS).map(|c| match c {
        ' '..='~' => c,
        _ if c.is_whitespace() => ' ',
        _ => '?',
    }));
    if chars.next().is_some() {
        out.push_str("...");
    }
    out
}

fn push_wrapped_lines(text: &str, start_y: f32, texts: &mut Vec<CString>, ys: &mut Vec<f32>) {
    let mut y = start_y;
    let mut rest = text;
    while !rest.is_empty() {
        let split = rest
            .char_indices()
            .nth(WRAP_COLUMNS)
            .map_or(rest.len(), |(idx, _)| idx);
        let (chunk, next) = rest.split_at(split);
        if let Ok(line) = CString::new(chunk) {
            texts.push(line);
            ys.push(y);
        }
        rest = next;
        y += LINE_HEIGHT;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn app_with_games(count: usize) -> App {
        let mut app = App::default();
        assert!(app.begin_folder_selection());
        let games = (0..count)
            .map(|i| {
                (
                    format!("content://provider/doc/{i:02}"),
                    format!("Game {i:02}"),
                    (i + 1) as u64,
                )
            })
            .collect();
        app.folder_scan_result(Some("content://provider/tree/games"), games, None);
        app
    }

    #[test]
    fn pointer_up_routes_toolbar_buttons() {
        let mut app = App::default();
        assert_eq!(handle_pointer_up(&mut app, 30.0, 60.0), ACTION_OPEN_DOCUMENT);
        assert_eq!(handle_pointer_up(&mut app, 150.0, 60.0), ACTION_ADD_FOLDER);
        assert_eq!(handle_pointer_up(&mut app, 260.0, 60.0), ACTION_INSTALL_KEYS);
        assert_eq!(handle_pointer_up(&mut app, 380.0, 60.0), ACTION_NONE);

        let mut ready = app_with_games(1);
        assert_eq!(handle_pointer_up(&mut ready, 380.0, 60.0), ACTION_LAUNCH);
    }

    #[test]
    fn pointer_up_selects_game_rows_and_paginates() {
        let mut app = app_with_games(7);
        assert_eq!(app.scroll_offset(), 0);
        assert_eq!(handle_pointer_up(&mut app, 410.0, 235.0), ACTION_NONE);
        assert_eq!(app.scroll_offset(), 2);

        assert_eq!(handle_pointer_up(&mut app, 30.0, 110.0), ACTION_NONE);
        assert_eq!(app.selected_game_index(), Some(2));
        assert_eq!(app.status(), c"Selected: Game 02");

        assert_eq!(handle_pointer_up(&mut app, 350.0, 235.0), ACTION_NONE);
        assert_eq!(app.scroll_offset(), 0);
    }

    #[test]
    fn settings_view_toggles_rows_and_routes_driver_actions() {
        let mut app = App::default();
        assert_eq!(handle_pointer_up(&mut app, 260.0, 16.0), ACTION_NONE);
        assert_eq!(app.view(), ViewMode::Settings);

        assert_eq!(handle_pointer_up(&mut app, 40.0, 108.0), ACTION_APPLY_SETTINGS);
        assert!(app.settings().use_docked_mode);

        assert_eq!(handle_pointer_up(&mut app, 40.0, 212.0), ACTION_APPLY_SETTINGS);
        assert_eq!(app.settings().gpu_accuracy, 1);

        assert_eq!(handle_pointer_up(&mut app, 320.0, 234.0), ACTION_INSTALL_DRIVER);
        assert_eq!(handle_pointer_up(&mut app, 400.0, 234.0), ACTION_CLEAR_DRIVER);
    }

    #[test]
    fn key_action_respects_can_launch() {
        assert_eq!(key_action(false, KEY_CONFIRM), ACTION_OPEN_DOCUMENT);
        assert_eq!(key_action(false, KEY_LAUNCH), ACTION_NONE);
        assert_eq!(key_action(true, KEY_LAUNCH), ACTION_LAUNCH);
        assert_eq!(key_action(true, 99), ACTION_NONE);
    }

    #[test]
    fn build_frame_emits_toolbar_rows_paging_and_wrapped_status() {
        let mut app = app_with_games(6);
        app.set_has_prod_keys(true);
        let frame = build_frame(
            &app,
            c"Eglantine Android",
            c"Open a game or add a folder.",
            c"Open file",
            c"Start",
        );

        assert_eq!(frame.cmds[0].kind, DRAW_FILL_RECT);
        assert_eq!(frame.cmds[0].color, BG_COLOR);
        assert_eq!(frame.cmds[5].color, BUTTON_ENABLED_COLOR);

        let text_cmds: Vec<_> = frame.cmds.iter().filter(|cmd| cmd.kind == DRAW_TEXT).collect();
        assert!(text_cmds.iter().all(|cmd| !cmd.text.is_null()));
        assert!(text_cmds.iter().any(|cmd| cmd.text == KEYS_READY.as_ptr()));
        assert!(frame._texts.iter().any(|text| text.as_c_str() == c"Game 00"));
    }
}
