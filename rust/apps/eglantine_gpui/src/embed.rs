// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use eglantine_gpui::session::ViewportRect;

pub const GAME_TOP_BAR_HEIGHT: f32 = 48.0;

#[cfg(windows)]
mod platform {
    use super::ViewportRect;

    const GWL_STYLE: i32 = -16;
    const WS_CLIPCHILDREN: isize = 0x0200_0000;

    #[repr(C)]
    struct Rect {
        left: i32,
        top: i32,
        right: i32,
        bottom: i32,
    }

    struct SearchState {
        visible_match: isize,
        fallback_match: isize,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentThreadId() -> u32;
    }

    #[link(name = "user32")]
    unsafe extern "system" {
        fn EnumThreadWindows(
            thread_id: u32,
            enum_fn: unsafe extern "system" fn(isize, isize) -> i32,
            lparam: isize,
        ) -> i32;
        fn GetParent(hwnd: isize) -> isize;
        fn IsWindowVisible(hwnd: isize) -> i32;
        fn IsWindow(hwnd: isize) -> i32;
        fn GetWindowTextW(hwnd: isize, text: *mut u16, max_count: i32) -> i32;
        fn GetClientRect(hwnd: isize, rect: *mut Rect) -> i32;
        fn GetWindowLongPtrW(hwnd: isize, index: i32) -> isize;
        fn SetWindowLongPtrW(hwnd: isize, index: i32, new_long: isize) -> isize;
    }

    unsafe extern "system" fn enum_thread_window(hwnd: isize, lparam: isize) -> i32 {
        let state = unsafe { &mut *(lparam as *mut SearchState) };
        if unsafe { GetParent(hwnd) } != 0 {
            return 1;
        }
        let mut title = [0u16; 64];
        let len = unsafe { GetWindowTextW(hwnd, title.as_mut_ptr(), title.len() as i32) };
        if len <= 0 {
            return 1;
        }
        let text = String::from_utf16_lossy(&title[..len as usize]);
        if text == "Eglantine" || text.starts_with("Eglantine") {
            if unsafe { IsWindowVisible(hwnd) } != 0 {
                state.visible_match = hwnd;
                return 0;
            }
            if state.fallback_match == 0 {
                state.fallback_match = hwnd;
            }
        }
        1
    }

    pub fn current_window_hwnd() -> Option<u64> {
        let mut state = SearchState { visible_match: 0, fallback_match: 0 };
        let thread_id = unsafe { GetCurrentThreadId() };
        unsafe {
            EnumThreadWindows(thread_id, enum_thread_window, &mut state as *mut SearchState as isize);
        }
        let hwnd = if state.visible_match != 0 { state.visible_match } else { state.fallback_match };
        if hwnd == 0 || unsafe { IsWindow(hwnd) } == 0 {
            return None;
        }
        let style = unsafe { GetWindowLongPtrW(hwnd, GWL_STYLE) };
        if (style & WS_CLIPCHILDREN) == 0 {
            unsafe {
                SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CLIPCHILDREN);
            }
        }
        Some(hwnd as u64)
    }

    pub fn viewport_rect(hwnd: u64, top_inset_logical: f32, scale_factor: f32) -> Option<ViewportRect> {
        let raw = hwnd as isize;
        if hwnd == 0 || unsafe { IsWindow(raw) } == 0 {
            return None;
        }
        let mut rect = Rect { left: 0, top: 0, right: 0, bottom: 0 };
        if unsafe { GetClientRect(raw, &mut rect) } == 0 {
            return None;
        }
        let top = (top_inset_logical * scale_factor).round().max(0.0) as i32;
        let width = (rect.right - rect.left).max(1);
        let height = ((rect.bottom - rect.top) - top).max(1);
        Some(ViewportRect { x: 0, y: top, width, height })
    }
}

#[cfg(not(windows))]
mod platform {
    use super::ViewportRect;

    pub fn current_window_hwnd() -> Option<u64> {
        None
    }

    pub fn viewport_rect(_hwnd: u64, _top_inset_logical: f32, _scale_factor: f32) -> Option<ViewportRect> {
        None
    }
}

pub use platform::{current_window_hwnd, viewport_rect};
