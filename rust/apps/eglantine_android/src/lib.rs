// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings, unsafe_op_in_unsafe_fn)]

use std::ffi::{CStr, c_char, c_int, c_void};
use std::ptr;

use overlay::OverlayEvent;
use settings::SettingsSnapshot;
use state::{App, SelectionResult};
use ui::{DrawCmd, FrameDrawList};

mod library;
mod overlay;
mod settings;
mod state;
mod ui;

const TITLE: &CStr = c"Eglantine Android";
const INSTRUCTION: &CStr = c"Pick a game or folder. Tap 3 fingers to stop.";
const PICKER_LABEL: &CStr = c"Open file";
const LAUNCH_LABEL: &CStr = c"Start";

#[repr(C)]
pub struct GameEntryC {
    pub uri: *const c_char,
    pub title: *const c_char,
    pub program_id: u64,
}

#[derive(Default)]
struct Frontend {
    app: App,
    frame: FrameDrawList,
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_create() -> *mut c_void {
    Box::into_raw(Box::new(Frontend::default())).cast()
}

/// # Safety
/// `app` must be null or a live handle created by `eglantine_android_create`.
/// A live handle may only be destroyed once, on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_destroy(app: *mut c_void) {
    if !app.is_null() {
        drop(unsafe { Box::from_raw(app.cast::<Frontend>()) });
    }
}

/// # Safety
/// `width` and `height` may be null or writable pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_viewport_size(width: *mut i32, height: *mut i32) {
    if !width.is_null() {
        unsafe { *width = ui::VIEWPORT_WIDTH };
    }
    if !height.is_null() {
        unsafe { *height = ui::VIEWPORT_HEIGHT };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_title() -> *const c_char {
    TITLE.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_instruction() -> *const c_char {
    INSTRUCTION.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_picker_label() -> *const c_char {
    PICKER_LABEL.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_launch_label() -> *const c_char {
    LAUNCH_LABEL.as_ptr()
}

/// The returned string belongs to the app and remains valid until the app is mutated or destroyed.
///
/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_status(app: *const c_void) -> *const c_char {
    unsafe { app.cast::<Frontend>().as_ref() }
        .map_or(c"Android frontend is unavailable.", |frontend| frontend.app.status())
        .as_ptr()
}

/// # Safety
/// `app` must be null or a live handle. `path` may be null or a readable NUL-terminated UTF-8 path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_set_storage_dir(app: *mut c_void, path: *const c_char) {
    let Some(frontend) = (unsafe { app.cast::<Frontend>().as_mut() }) else {
        return;
    };
    if let Some(path) = unsafe { optional_cstr(path) } {
        frontend.app.set_storage_dir(path);
    }
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_set_has_prod_keys(app: *mut c_void, has_keys: bool) {
    if let Some(frontend) = unsafe { app.cast::<Frontend>().as_mut() } {
        frontend.app.set_has_prod_keys(has_keys);
    }
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_set_suspended(app: *mut c_void, suspended: bool) {
    if let Some(frontend) = unsafe { app.cast::<Frontend>().as_mut() } {
        frontend.app.set_suspended(suspended);
    }
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_begin_selection(app: *mut c_void) -> bool {
    unsafe { app.cast::<Frontend>().as_mut() }.is_some_and(|frontend| frontend.app.begin_selection())
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_begin_folder_selection(app: *mut c_void) -> bool {
    unsafe { app.cast::<Frontend>().as_mut() }
        .is_some_and(|frontend| frontend.app.begin_folder_selection())
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_begin_keys_selection(app: *mut c_void) -> bool {
    unsafe { app.cast::<Frontend>().as_mut() }
        .is_some_and(|frontend| frontend.app.begin_keys_selection())
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_begin_driver_selection(app: *mut c_void) -> bool {
    unsafe { app.cast::<Frontend>().as_mut() }
        .is_some_and(|frontend| frontend.app.begin_driver_selection())
}

/// # Safety
/// `app` must be null or a live handle. `driver_name` may be null or a readable NUL-terminated
/// UTF-8 string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_set_settings(
    app: *mut c_void,
    settings: SettingsSnapshot,
    driver_name: *const c_char,
) {
    let Some(frontend) = (unsafe { app.cast::<Frontend>().as_mut() }) else {
        return;
    };
    frontend
        .app
        .set_settings(settings, unsafe { optional_cstr(driver_name) });
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_get_settings(app: *const c_void) -> SettingsSnapshot {
    unsafe { app.cast::<Frontend>().as_ref() }
        .map_or_else(SettingsSnapshot::default, |frontend| frontend.app.settings())
}

/// # Safety
/// `app` must be null or a live handle. `error` and `driver_name` may be null or readable
/// NUL-terminated UTF-8 strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_driver_install_result(
    app: *mut c_void,
    error: *const c_char,
    driver_name: *const c_char,
) {
    let Some(frontend) = (unsafe { app.cast::<Frontend>().as_mut() }) else {
        return;
    };
    frontend.app.driver_install_result(
        unsafe { optional_cstr(error) },
        unsafe { optional_cstr(driver_name) },
    );
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_game_count(app: *const c_void) -> usize {
    unsafe { app.cast::<Frontend>().as_ref() }.map_or(0, |frontend| frontend.app.games().len())
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_can_launch(app: *const c_void) -> bool {
    unsafe { app.cast::<Frontend>().as_ref() }.is_some_and(|frontend| frontend.app.can_launch())
}

/// The returned URI belongs to the app and remains valid until it is mutated or destroyed.
///
/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_begin_launch(app: *mut c_void) -> *const c_char {
    unsafe { app.cast::<Frontend>().as_mut() }
        .and_then(|frontend| frontend.app.begin_launch())
        .map_or(ptr::null(), CStr::as_ptr)
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
/// `error` may be null. Otherwise, it must be a readable NUL-terminated string independent of
/// the app state and valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_launch_result(app: *mut c_void, error: *const c_char) {
    if let Some(frontend) = unsafe { app.cast::<Frontend>().as_mut() } {
        frontend.app.launch_result(unsafe { lifecycle_error(error) });
    }
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
/// `error` may be null. Otherwise, it must be a readable NUL-terminated string independent of
/// the app state and valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_session_ended(app: *mut c_void, error: *const c_char) {
    if let Some(frontend) = unsafe { app.cast::<Frontend>().as_mut() } {
        frontend.app.session_ended(unsafe { lifecycle_error(error) });
    }
}

unsafe fn optional_cstr<'a>(text: *const c_char) -> Option<&'a str> {
    if text.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(text) }.to_str().ok()
}

unsafe fn lifecycle_error<'a>(error: *const c_char) -> Option<&'a str> {
    if error.is_null() {
        return None;
    }
    Some(unsafe { CStr::from_ptr(error) }.to_str().unwrap_or("Unknown error."))
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
/// For selected and failed results, `detail` must be null or a valid NUL-terminated string
/// independent of the app state and readable for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_select_result(
    app: *mut c_void,
    status: c_int,
    detail: *const c_char,
) {
    let Some(frontend) = (unsafe { app.cast::<Frontend>().as_mut() }) else {
        return;
    };
    let result = match status {
        0 => SelectionResult::Cancelled,
        1 | 2 => {
            let detail = unsafe { optional_cstr(detail) };
            if status == 1 {
                SelectionResult::Selected(detail)
            } else {
                SelectionResult::Failed(detail)
            }
        }
        _ => SelectionResult::Failed(Some("Unexpected document picker result.")),
    };
    frontend.app.selection_result(result);
}

/// # Safety
/// `app` must be null or a live handle. `tree_uri` and `error` may be null or readable NUL-terminated
/// strings. If `game_count > 0`, `games` must point to `game_count` initialized `GameEntryC` items.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_folder_scan_result(
    app: *mut c_void,
    tree_uri: *const c_char,
    games: *const GameEntryC,
    game_count: usize,
    error: *const c_char,
) {
    let Some(frontend) = (unsafe { app.cast::<Frontend>().as_mut() }) else {
        return;
    };
    let tree = unsafe { optional_cstr(tree_uri) };
    let err = unsafe { optional_cstr(error) };
    let mut entries = Vec::new();
    if game_count > 0 && !games.is_null() {
        let slice = unsafe { std::slice::from_raw_parts(games, game_count) };
        entries.reserve(slice.len());
        for item in slice {
            if let Some(uri) = unsafe { optional_cstr(item.uri) } {
                let title = unsafe { optional_cstr(item.title) }.unwrap_or("Untitled game");
                entries.push((uri.to_owned(), title.to_owned(), item.program_id));
            }
        }
    }
    frontend.app.folder_scan_result(tree, entries, err);
}

/// # Safety
/// `app` must be null or a live handle. `error` may be null or a readable NUL-terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_keys_install_result(
    app: *mut c_void,
    error: *const c_char,
    has_keys: bool,
) {
    let Some(frontend) = (unsafe { app.cast::<Frontend>().as_mut() }) else {
        return;
    };
    frontend
        .app
        .keys_install_result(unsafe { optional_cstr(error) }, has_keys);
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_pointer_up(app: *mut c_void, x: f32, y: f32) -> i32 {
    let Some(frontend) = (unsafe { app.cast::<Frontend>().as_mut() }) else {
        return 0;
    };
    ui::handle_pointer_up(&mut frontend.app, x, y)
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_key_down(app: *mut c_void, key: i32) -> i32 {
    let can_launch = unsafe { eglantine_android_can_launch(app) };
    ui::key_action(can_launch, key)
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
/// `out_cmds` may be null or a writable pointer. Returned pointers remain valid until the next
/// call to `eglantine_android_build_draw_list` or `eglantine_android_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_build_draw_list(
    app: *mut c_void,
    out_cmds: *mut *const DrawCmd,
) -> usize {
    let Some(frontend) = (unsafe { app.cast::<Frontend>().as_mut() }) else {
        if !out_cmds.is_null() {
            unsafe { *out_cmds = ptr::null() };
        }
        return 0;
    };
    frontend.frame = ui::build_frame(
        &frontend.app,
        TITLE,
        INSTRUCTION,
        PICKER_LABEL,
        LAUNCH_LABEL,
    );
    if !out_cmds.is_null() {
        unsafe { *out_cmds = frontend.frame.as_ptr() };
    }
    frontend.frame.len()
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_overlay_touch_down(
    app: *mut c_void,
    finger_id: i64,
    norm_x: f32,
    norm_y: f32,
) -> OverlayEvent {
    unsafe { app.cast::<Frontend>().as_mut() }
        .map_or_else(default_overlay_event, |frontend| {
            frontend.app.overlay_mut().touch_down(finger_id, norm_x, norm_y)
        })
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_overlay_touch_move(
    app: *mut c_void,
    finger_id: i64,
    norm_x: f32,
    norm_y: f32,
) -> OverlayEvent {
    unsafe { app.cast::<Frontend>().as_mut() }
        .map_or_else(default_overlay_event, |frontend| {
            frontend.app.overlay_mut().touch_move(finger_id, norm_x, norm_y)
        })
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_overlay_touch_up(
    app: *mut c_void,
    finger_id: i64,
) -> OverlayEvent {
    unsafe { app.cast::<Frontend>().as_mut() }
        .map_or_else(default_overlay_event, |frontend| {
            frontend.app.overlay_mut().touch_up(finger_id)
        })
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_overlay_set_controller_active(
    app: *mut c_void,
    active: bool,
) -> bool {
    unsafe { app.cast::<Frontend>().as_mut() }
        .is_some_and(|frontend| frontend.app.overlay_mut().set_controller_active(active))
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_overlay_reset(app: *mut c_void) {
    if let Some(frontend) = unsafe { app.cast::<Frontend>().as_mut() } {
        frontend.app.overlay_mut().reset_touches();
    }
}

const fn default_overlay_event() -> OverlayEvent {
    OverlayEvent {
        kind: 0,
        consumed: false,
        id: 0,
        pressed: false,
        x: 0.0,
        y: 0.0,
    }
}

#[cfg(test)]
mod tests {
    use std::ffi::CString;
    use std::ptr;

    use super::*;

    #[test]
    fn c_bridge_ignores_null_app_handles() {
        // Null is an accepted sentinel on every handle-based entry point.
        unsafe {
            assert!(!eglantine_android_begin_selection(ptr::null_mut()));
            assert!(!eglantine_android_begin_folder_selection(ptr::null_mut()));
            assert!(!eglantine_android_begin_keys_selection(ptr::null_mut()));
            assert!(!eglantine_android_begin_driver_selection(ptr::null_mut()));
            assert_eq!(eglantine_android_get_settings(ptr::null()), SettingsSnapshot::default());
            eglantine_android_set_settings(ptr::null_mut(), SettingsSnapshot::default(), ptr::null());
            eglantine_android_driver_install_result(ptr::null_mut(), ptr::null(), ptr::null());
            assert!(!eglantine_android_can_launch(ptr::null()));
            assert_eq!(eglantine_android_game_count(ptr::null()), 0);
            assert!(eglantine_android_begin_launch(ptr::null_mut()).is_null());
            assert_eq!(
                CStr::from_ptr(eglantine_android_status(ptr::null())),
                c"Android frontend is unavailable."
            );
            eglantine_android_set_storage_dir(ptr::null_mut(), ptr::null());
            eglantine_android_set_has_prod_keys(ptr::null_mut(), true);
            eglantine_android_set_suspended(ptr::null_mut(), true);
            eglantine_android_select_result(ptr::null_mut(), 1, ptr::null());
            eglantine_android_folder_scan_result(
                ptr::null_mut(),
                ptr::null(),
                ptr::null(),
                0,
                ptr::null(),
            );
            eglantine_android_keys_install_result(ptr::null_mut(), ptr::null(), false);
            eglantine_android_launch_result(ptr::null_mut(), ptr::null());
            eglantine_android_session_ended(ptr::null_mut(), ptr::null());
            assert_eq!(eglantine_android_pointer_up(ptr::null_mut(), 30.0, 60.0), 0);
            assert_eq!(eglantine_android_key_down(ptr::null_mut(), 2), 0);
            let mut cmds = ptr::null();
            assert_eq!(eglantine_android_build_draw_list(ptr::null_mut(), &mut cmds), 0);
            assert!(cmds.is_null());
            assert!(!eglantine_android_overlay_touch_down(ptr::null_mut(), 1, 0.92, 0.53).consumed);
            assert!(!eglantine_android_overlay_touch_move(ptr::null_mut(), 1, 0.92, 0.53).consumed);
            assert!(!eglantine_android_overlay_touch_up(ptr::null_mut(), 1).consumed);
            assert!(!eglantine_android_overlay_set_controller_active(ptr::null_mut(), true));
            eglantine_android_overlay_reset(ptr::null_mut());
            eglantine_android_destroy(ptr::null_mut());
        }
    }

    #[test]
    fn c_bridge_reports_unknown_picker_codes_and_can_retry() {
        let app = eglantine_android_create();
        let unexpected_status = 42;
        // The handle is live and confined to this thread until its single destroy call.
        unsafe {
            assert!(eglantine_android_begin_selection(app));
            eglantine_android_select_result(app, unexpected_status, ptr::null());
            assert_eq!(
                CStr::from_ptr(eglantine_android_status(app)),
                c"Document picker failed: Unexpected document picker result."
            );
            assert!(eglantine_android_begin_selection(app));
            eglantine_android_destroy(app);
        }
    }

    #[test]
    fn c_bridge_copies_selected_document_text_before_returning() {
        let app = eglantine_android_create();
        let uri = CString::new("content://provider/document/game.nsp").unwrap();
        // The live app stays on this thread and the owned input remains readable for the call.
        unsafe {
            assert!(eglantine_android_begin_selection(app));
            eglantine_android_select_result(app, 1, uri.as_ptr());
            drop(uri);
            assert_eq!(
                CStr::from_ptr(eglantine_android_status(app)),
                c"Selected: content://provider/document/game.nsp"
            );
            assert_eq!(
                CStr::from_ptr(eglantine_android_begin_launch(app)),
                c"content://provider/document/game.nsp"
            );
            eglantine_android_destroy(app);
        }
    }

    #[test]
    fn c_bridge_builds_draw_list_and_routes_folder_keys_and_pointer_actions() {
        let app = eglantine_android_create();
        let tree_uri = CString::new("content://provider/tree/games").unwrap();
        let game_uri = CString::new("content://provider/document/game.nsp").unwrap();
        let game_title = CString::new("Test Game").unwrap();
        let entries = [GameEntryC {
            uri: game_uri.as_ptr(),
            title: game_title.as_ptr(),
            program_id: 0x0100_0000_0001_0000,
        }];
        unsafe {
            let mut width = 0;
            let mut height = 0;
            eglantine_android_viewport_size(&mut width, &mut height);
            assert_eq!((width, height), (480, 320));

            assert_eq!(eglantine_android_pointer_up(app, 30.0, 60.0), 1);
            assert_eq!(eglantine_android_pointer_up(app, 150.0, 60.0), 3);
            assert_eq!(eglantine_android_pointer_up(app, 260.0, 60.0), 4);
            assert_eq!(eglantine_android_pointer_up(app, 380.0, 60.0), 0);

            assert!(eglantine_android_begin_folder_selection(app));
            eglantine_android_folder_scan_result(
                app,
                tree_uri.as_ptr(),
                entries.as_ptr(),
                entries.len(),
                ptr::null(),
            );
            assert_eq!(eglantine_android_game_count(app), 1);
            assert_eq!(eglantine_android_pointer_up(app, 380.0, 60.0), 2);

            assert!(eglantine_android_begin_keys_selection(app));
            eglantine_android_keys_install_result(app, ptr::null(), true);
            assert_eq!(
                CStr::from_ptr(eglantine_android_status(app)),
                c"Keys installed (prod.keys ready)."
            );

            let mut cmds = ptr::null();
            let count = eglantine_android_build_draw_list(app, &mut cmds);
            assert!(count >= 12);
            assert!(!cmds.is_null());
            let slice = std::slice::from_raw_parts(cmds, count);
            assert!(
                slice
                    .iter()
                    .filter(|cmd| cmd.kind == 1)
                    .any(|cmd| CStr::from_ptr(cmd.text) == c"Test Game")
            );

            eglantine_android_destroy(app);
        }
    }

    #[test]
    fn c_bridge_routes_overlay_touches_and_controller_auto_hide() {
        let app = eglantine_android_create();
        unsafe {
            let down = eglantine_android_overlay_touch_down(app, 10, 0.92, 0.53);
            assert!(down.consumed);
            assert_eq!(down.kind, 1);
            assert_eq!(down.id, 0);
            assert!(down.pressed);

            assert!(eglantine_android_overlay_set_controller_active(app, true));
            assert!(!eglantine_android_overlay_touch_down(app, 11, 0.92, 0.53).consumed);
            assert!(!eglantine_android_overlay_set_controller_active(app, false));
            assert!(eglantine_android_overlay_touch_down(app, 12, 0.92, 0.53).consumed);
            eglantine_android_overlay_reset(app);
            assert!(!eglantine_android_overlay_touch_up(app, 12).consumed);

            eglantine_android_destroy(app);
        }
    }

    #[test]
    fn c_bridge_round_trips_settings_and_driver_updates() {
        let app = eglantine_android_create();
        let driver = CString::new("turnip_v25.so").unwrap();
        let initial = SettingsSnapshot {
            use_docked_mode: true,
            async_gpu: false,
            disk_shader_cache: true,
            use_vsync: false,
            resolution_index: 4,
            gpu_accuracy: 1,
        };
        unsafe {
            eglantine_android_set_settings(app, initial, driver.as_ptr());
            assert_eq!(eglantine_android_get_settings(app), initial);

            assert!(eglantine_android_begin_driver_selection(app));
            eglantine_android_driver_install_result(app, ptr::null(), driver.as_ptr());
            assert_eq!(
                CStr::from_ptr(eglantine_android_status(app)),
                c"Custom GPU driver installed."
            );

            eglantine_android_destroy(app);
        }
    }
}
