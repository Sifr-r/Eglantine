// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings, unsafe_op_in_unsafe_fn)]

use std::ffi::{CStr, c_char, c_int, c_void};
use std::ptr;

use state::{App, SelectionResult};

mod state;

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_create() -> *mut c_void {
    Box::into_raw(Box::new(App::default())).cast()
}

/// # Safety
/// `app` must be null or a live handle created by `eglantine_android_create`.
/// A live handle may only be destroyed once, on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_destroy(app: *mut c_void) {
    if !app.is_null() {
        drop(unsafe { Box::from_raw(app.cast::<App>()) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_title() -> *const c_char {
    c"Eglantine Android".as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_instruction() -> *const c_char {
    c"Select a game file. Tap with three fingers to stop.".as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_picker_label() -> *const c_char {
    c"Open game file".as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn eglantine_android_launch_label() -> *const c_char {
    c"Start emulation".as_ptr()
}

/// The returned string belongs to the app and remains valid until the app is mutated or destroyed.
///
/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_status(app: *const c_void) -> *const c_char {
    unsafe { app.cast::<App>().as_ref() }
        .map_or(c"Android frontend is unavailable.", App::status)
        .as_ptr()
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_set_suspended(app: *mut c_void, suspended: bool) {
    if let Some(app) = unsafe { app.cast::<App>().as_mut() } {
        app.set_suspended(suspended);
    }
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_begin_selection(app: *mut c_void) -> bool {
    unsafe { app.cast::<App>().as_mut() }.is_some_and(App::begin_selection)
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_can_launch(app: *const c_void) -> bool {
    unsafe { app.cast::<App>().as_ref() }.is_some_and(App::can_launch)
}

/// The returned URI belongs to the app and remains valid until it is mutated or destroyed.
///
/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_begin_launch(app: *mut c_void) -> *const c_char {
    unsafe { app.cast::<App>().as_mut() }
        .and_then(App::begin_launch)
        .map_or(ptr::null(), CStr::as_ptr)
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
/// `error` may be null. Otherwise, it must be a readable NUL-terminated string independent of
/// the app state and valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_launch_result(app: *mut c_void, error: *const c_char) {
    if let Some(app) = unsafe { app.cast::<App>().as_mut() } {
        app.launch_result(unsafe { lifecycle_error(error) });
    }
}

/// # Safety
/// `app` must be null or a live handle. A live handle may only be used on its owning thread.
/// `error` may be null. Otherwise, it must be a readable NUL-terminated string independent of
/// the app state and valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn eglantine_android_session_ended(app: *mut c_void, error: *const c_char) {
    if let Some(app) = unsafe { app.cast::<App>().as_mut() } {
        app.session_ended(unsafe { lifecycle_error(error) });
    }
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
    let Some(app) = (unsafe { app.cast::<App>().as_mut() }) else {
        return;
    };
    let result = match status {
        0 => SelectionResult::Cancelled,
        1 | 2 => {
            let detail = if detail.is_null() {
                None
            } else {
                unsafe { CStr::from_ptr(detail) }.to_str().ok()
            };
            if status == 1 {
                SelectionResult::Selected(detail)
            } else {
                SelectionResult::Failed(detail)
            }
        }
        _ => SelectionResult::Failed(Some("Unexpected document picker result.")),
    };
    app.selection_result(result);
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
            assert!(!eglantine_android_can_launch(ptr::null()));
            assert!(eglantine_android_begin_launch(ptr::null_mut()).is_null());
            assert_eq!(
                CStr::from_ptr(eglantine_android_status(ptr::null())),
                c"Android frontend is unavailable."
            );
            eglantine_android_set_suspended(ptr::null_mut(), true);
            eglantine_android_select_result(ptr::null_mut(), 1, ptr::null());
            eglantine_android_launch_result(ptr::null_mut(), ptr::null());
            eglantine_android_session_ended(ptr::null_mut(), ptr::null());
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
}
