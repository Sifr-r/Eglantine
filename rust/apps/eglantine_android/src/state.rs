// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::borrow::Cow;
use std::ffi::{CStr, CString};

const MAX_STATUS_DETAIL: usize = 88;

#[derive(Default)]
pub(super) struct App {
    suspended: bool,
    document: Option<CString>,
    activity: Activity,
}

impl App {
    pub(super) fn set_suspended(&mut self, suspended: bool) {
        self.suspended = suspended;
    }

    pub(super) fn begin_selection(&mut self) -> bool {
        if !self.is_idle() {
            return false;
        }
        self.activity = Activity::Picking;
        true
    }

    pub(super) fn selection_result(&mut self, result: SelectionResult<'_>) {
        if !matches!(self.activity, Activity::Picking) {
            return;
        }
        let message = match result {
            SelectionResult::Cancelled => Cow::Borrowed(c"Document selection cancelled."),
            SelectionResult::Selected(uri) => {
                if let Some(uri) = uri.filter(|uri| is_document_uri(uri)) {
                    self.document = CString::new(uri).ok();
                    status_with_detail("Selected: ", uri).into()
                } else {
                    Cow::Borrowed(c"The picker returned an invalid Android document URI.")
                }
            }
            SelectionResult::Failed(detail) => detail
                .map(str::trim)
                .filter(|detail| !detail.is_empty())
                .map_or(Cow::Borrowed(c"The Android document picker failed."), |detail| {
                    status_with_detail("Document picker failed: ", detail).into()
                }),
        };
        self.activity = Activity::Message(message);
    }

    pub(super) fn can_launch(&self) -> bool {
        self.is_idle() && self.document.is_some()
    }

    pub(super) fn begin_launch(&mut self) -> Option<&CStr> {
        if !self.can_launch() {
            return None;
        }
        self.activity = Activity::Launching;
        self.document.as_deref()
    }

    pub(super) fn launch_result(&mut self, error: Option<&str>) {
        if matches!(self.activity, Activity::Launching) {
            self.activity = error.map_or(Activity::Running, |error| {
                Activity::Message(status_with_detail("Launch failed: ", error).into())
            });
        }
    }

    pub(super) fn session_ended(&mut self, error: Option<&str>) {
        if matches!(self.activity, Activity::Running) {
            let message = error.map_or(Cow::Borrowed(c"Emulation ended."), |error| {
                status_with_detail("Emulation ended: ", error).into()
            });
            self.activity = Activity::Message(message);
        }
    }

    pub(super) fn status(&self) -> &CStr {
        match &self.activity {
            Activity::Ready => c"No document selected.",
            Activity::Picking => c"Waiting for the Android document picker...",
            Activity::Launching => c"Starting emulation...",
            Activity::Running => c"Emulation is running.",
            Activity::Message(message) => message,
        }
    }

    fn is_idle(&self) -> bool {
        !self.suspended && matches!(self.activity, Activity::Ready | Activity::Message(_))
    }
}

#[derive(Default)]
enum Activity {
    #[default]
    Ready,
    Picking,
    Launching,
    Running,
    Message(Cow<'static, CStr>),
}

pub(super) enum SelectionResult<'a> {
    Cancelled,
    Selected(Option<&'a str>),
    Failed(Option<&'a str>),
}

fn is_document_uri(uri: &str) -> bool {
    uri.strip_prefix("content://").is_some_and(|rest| {
        let authority_end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
        authority_end != 0 && !rest.chars().any(|c| c.is_control() || c.is_whitespace())
    })
}

fn status_with_detail(prefix: &str, detail: &str) -> CString {
    let mut message = String::from(prefix);
    let mut chars = detail.chars();
    message.extend(chars.by_ref().take(MAX_STATUS_DETAIL).map(|c| match c {
        ' '..='~' => c,
        _ if c.is_whitespace() => ' ',
        _ => '?',
    }));
    if chars.next().is_some() {
        message.push_str("...");
    }
    CString::new(message).expect("display text contains only printable ASCII")
}

#[cfg(test)]
mod tests {
    use super::{App, SelectionResult};

    #[test]
    fn a_pending_picker_cannot_be_opened_twice() {
        let mut app = App::default();
        assert!(app.begin_selection());
        assert!(!app.begin_selection());
        app.selection_result(SelectionResult::Cancelled);
        assert_eq!(app.status(), c"Document selection cancelled.");
        assert!(app.begin_selection());
    }

    #[test]
    fn a_picker_callback_during_backgrounding_survives_resume() {
        let mut app = App::default();
        assert!(app.begin_selection());
        app.set_suspended(true);
        let uri = "content://provider/document/game.nsp";
        app.selection_result(SelectionResult::Selected(Some(uri)));
        assert!(!app.begin_selection());
        app.set_suspended(false);
        assert_eq!(app.status(), c"Selected: content://provider/document/game.nsp");
        assert!(app.begin_selection());
    }

    #[test]
    fn callback_without_pending_request_cannot_replace_status() {
        let mut app = App::default();
        app.selection_result(SelectionResult::Selected(Some("content://provider/other")));
        assert_eq!(app.status(), c"No document selected.");
    }

    #[test]
    fn invalid_document_uris_end_the_selection_with_an_error() {
        let mut app = App::default();
        for uri in [None, Some("file:///a"), Some("content:///a"), Some("content://p/\na")] {
            assert!(app.begin_selection());
            app.selection_result(SelectionResult::Selected(uri));
            assert_eq!(app.status(), c"The picker returned an invalid Android document URI.");
        }
    }

    #[test]
    fn picker_failures_are_readable_and_allow_retry() {
        let mut app = App::default();
        assert!(app.begin_selection());
        app.selection_result(SelectionResult::Failed(Some("Picker\nis unavailable")));
        assert_eq!(app.status(), c"Document picker failed: Picker is unavailable");
        assert!(app.begin_selection());
        app.selection_result(SelectionResult::Failed(None));
        assert_eq!(app.status(), c"The Android document picker failed.");
    }

    #[test]
    fn launch_requires_selection_and_retains_the_document_on_failure() {
        let mut app = App::default();
        let uri = "content://provider/document/game.nsp";
        assert!(!app.can_launch());
        assert!(app.begin_selection());
        app.selection_result(SelectionResult::Selected(Some(uri)));
        assert!(app.can_launch());
        assert_eq!(app.begin_launch(), Some(c"content://provider/document/game.nsp"));
        assert!(!app.can_launch());
        assert!(!app.begin_selection());
        app.launch_result(Some("Missing firmware"));
        assert_eq!(app.status(), c"Launch failed: Missing firmware");
        assert!(app.can_launch());
    }

    #[test]
    fn running_session_blocks_picker_until_it_stops() {
        let mut app = App::default();
        assert!(app.begin_selection());
        app.selection_result(SelectionResult::Selected(Some("content://provider/game")));
        assert!(app.begin_launch().is_some());
        app.launch_result(None);
        app.set_suspended(true);
        app.set_suspended(false);
        assert!(!app.can_launch());
        assert!(!app.begin_selection());
        app.session_ended(None);
        assert_eq!(app.status(), c"Emulation ended.");
        assert!(app.can_launch());
    }
}
