// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use serde::Deserialize;

pub const DEFAULT_MAX_TEXT_LENGTH: usize = 500;
pub const MAX_UTF8_BYTES: usize = 0x7D4 - 1;

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SwkbdTextCheckKind {
    Failure,
    Confirm,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SwkbdShowRequest {
    pub request_id: u64,
    pub header_text: String,
    pub sub_text: String,
    pub guide_text: String,
    pub ok_text: String,
    pub initial_text: String,
    pub min_length: u32,
    pub max_length: u32,
    pub password: bool,
    pub multiline: bool,
    pub disable_cancel: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TextCheckFeedback {
    Failure(String),
    Confirm(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SoftwareKeyboardState {
    pub request_id: u64,
    pub header_text: String,
    pub sub_text: String,
    pub guide_text: String,
    pub ok_text: String,
    pub text: String,
    pub min_length: usize,
    pub max_length: usize,
    pub password: bool,
    pub multiline: bool,
    pub disable_cancel: bool,
    pub text_check: Option<TextCheckFeedback>,
    pub validation_error: Option<String>,
    pub submitting: bool,
}

impl SoftwareKeyboardState {
    pub fn from_request(req: SwkbdShowRequest) -> Self {
        let max_length = if req.max_length == 0 {
            DEFAULT_MAX_TEXT_LENGTH
        } else {
            (req.max_length as usize).min(DEFAULT_MAX_TEXT_LENGTH)
        };
        let min_length = (req.min_length as usize).min(max_length);
        let mut state = Self {
            request_id: req.request_id,
            header_text: req.header_text,
            sub_text: req.sub_text,
            guide_text: req.guide_text,
            ok_text: req.ok_text,
            text: String::new(),
            min_length,
            max_length,
            password: req.password,
            multiline: req.multiline,
            disable_cancel: req.disable_cancel,
            text_check: None,
            validation_error: None,
            submitting: false,
        };
        state.push_str(&req.initial_text);
        state
    }

    pub fn utf16_len(&self) -> usize {
        self.text.encode_utf16().count()
    }

    pub fn push_str(&mut self, input: &str) {
        if self.submitting {
            return;
        }
        let mut changed = false;
        for ch in input.chars() {
            if ch.is_control() && !(self.multiline && ch == '\n') {
                continue;
            }
            let next_utf16 = self.utf16_len() + ch.len_utf16();
            let next_utf8 = self.text.len() + ch.len_utf8();
            if next_utf16 > self.max_length || next_utf8 > MAX_UTF8_BYTES {
                break;
            }
            self.text.push(ch);
            changed = true;
        }
        if changed {
            self.validation_error = None;
            if matches!(self.text_check, Some(TextCheckFeedback::Confirm(_))) {
                self.text_check = None;
            }
        }
    }

    pub fn backspace(&mut self) {
        if self.submitting {
            return;
        }
        if self.text.pop().is_some() {
            self.validation_error = None;
            if matches!(self.text_check, Some(TextCheckFeedback::Confirm(_))) {
                self.text_check = None;
            }
        }
    }

    pub fn dismiss_confirm(&mut self) -> bool {
        if !self.submitting && matches!(self.text_check, Some(TextCheckFeedback::Confirm(_))) {
            self.text_check = None;
            return true;
        }
        false
    }

    pub fn is_confirm_prompt(&self) -> bool {
        matches!(self.text_check, Some(TextCheckFeedback::Confirm(_)))
    }

    pub fn display_text(&self) -> String {
        if self.password {
            "•".repeat(self.text.chars().count())
        } else {
            self.text.clone()
        }
    }

    pub fn prepare_submit(&mut self) -> Result<(u64, String, bool), String> {
        if self.submitting {
            return Err("Already submitting software keyboard input".into());
        }
        let len = self.utf16_len();
        if len < self.min_length {
            let error = format!("Enter at least {} characters.", self.min_length);
            self.validation_error = Some(error.clone());
            return Err(error);
        }
        if len > self.max_length {
            let error = format!("Enter at most {} characters.", self.max_length);
            self.validation_error = Some(error.clone());
            return Err(error);
        }
        if self.text.len() > MAX_UTF8_BYTES {
            let error = "Text exceeds the keyboard buffer limit.".to_owned();
            self.validation_error = Some(error.clone());
            return Err(error);
        }
        self.validation_error = None;
        let confirmed = self.is_confirm_prompt();
        self.submitting = true;
        Ok((self.request_id, self.text.clone(), confirmed))
    }

    pub fn prepare_cancel(&mut self) -> Result<u64, String> {
        if self.submitting {
            return Err("Software keyboard request is already completing".into());
        }
        if self.disable_cancel {
            let error = "Cancel is disabled for this prompt.".to_owned();
            self.validation_error = Some(error.clone());
            return Err(error);
        }
        self.validation_error = None;
        self.submitting = true;
        Ok(self.request_id)
    }

    pub fn apply_text_check(&mut self, request_id: u64, kind: SwkbdTextCheckKind, message: String) -> bool {
        if self.request_id != request_id {
            return false;
        }
        self.submitting = false;
        self.validation_error = None;
        self.text_check = Some(match kind {
            SwkbdTextCheckKind::Failure => TextCheckFeedback::Failure(message),
            SwkbdTextCheckKind::Confirm => TextCheckFeedback::Confirm(message),
        });
        true
    }

    pub fn clear_submitting_on_error(&mut self, error: String) {
        self.submitting = false;
        self.validation_error = Some(error);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_request() -> SwkbdShowRequest {
        SwkbdShowRequest {
            request_id: 7,
            header_text: "Player Name".into(),
            sub_text: "Enter your name".into(),
            guide_text: "Name".into(),
            ok_text: "OK".into(),
            initial_text: "Églantine".into(),
            min_length: 2,
            max_length: 12,
            password: false,
            multiline: false,
            disable_cancel: false,
        }
    }

    #[test]
    fn initializes_with_utf16_bounds_and_non_ascii_text() {
        let mut state = SoftwareKeyboardState::from_request(sample_request());
        assert_eq!(state.text, "Églantine");
        assert_eq!(state.utf16_len(), 9);

        // Surrogate pair emoji takes 2 UTF-16 code units: 9 + 2 = 11 <= 12.
        state.push_str("🎮");
        assert_eq!(state.utf16_len(), 11);
        // Another emoji would take 13 > 12, so it is rejected.
        state.push_str("🎮");
        assert_eq!(state.utf16_len(), 11);
        // Single BMP non-ASCII char takes 1 UTF-16 code unit: 11 + 1 = 12.
        state.push_str("日");
        assert_eq!(state.utf16_len(), 12);
        assert_eq!(state.text, "Églantine🎮日");
    }

    #[test]
    fn enforces_min_length_and_disable_cancel() {
        let mut req = sample_request();
        req.initial_text = "A".into();
        req.min_length = 3;
        req.disable_cancel = true;
        let mut state = SoftwareKeyboardState::from_request(req);

        assert!(state.prepare_submit().is_err());
        assert!(!state.submitting);
        assert!(state.prepare_cancel().is_err());
        assert!(!state.submitting);

        state.push_str("BC");
        assert_eq!(state.prepare_submit().unwrap(), (7, "ABC".into(), false));
        // Single-completion guard while awaiting validation/close.
        assert!(state.prepare_submit().is_err());
        assert!(state.prepare_cancel().is_err());
    }

    #[test]
    fn handles_text_check_failure_and_confirm_retry() {
        let mut state = SoftwareKeyboardState::from_request(sample_request());
        assert_eq!(state.prepare_submit().unwrap(), (7, "Églantine".into(), false));

        // Mismatched request_id is ignored.
        assert!(!state.apply_text_check(99, SwkbdTextCheckKind::Failure, "Wrong".into()));
        assert!(state.submitting);

        // Failure unlocks editing and keeps confirmed = false on retry.
        assert!(state.apply_text_check(7, SwkbdTextCheckKind::Failure, "Reserved".into()));
        assert!(!state.submitting);
        assert_eq!(state.text_check, Some(TextCheckFeedback::Failure("Reserved".into())));
        state.backspace();
        assert_eq!(state.prepare_submit().unwrap(), (7, "Églantin".into(), false));

        // Confirm allows submitting with confirmed = true.
        assert!(state.apply_text_check(7, SwkbdTextCheckKind::Confirm, "Use this?".into()));
        assert!(state.is_confirm_prompt());
        // Editing during confirm clears confirm prompt back to unconfirmed.
        state.push_str("e");
        assert!(!state.is_confirm_prompt());
        assert_eq!(state.prepare_submit().unwrap(), (7, "Églantine".into(), false));

        assert!(state.apply_text_check(7, SwkbdTextCheckKind::Confirm, "Really?".into()));
        assert_eq!(state.prepare_submit().unwrap(), (7, "Églantine".into(), true));
    }

    #[test]
    fn masks_password_display_without_mutating_value() {
        let mut req = sample_request();
        req.password = true;
        req.initial_text = "秘密12".into();
        let state = SoftwareKeyboardState::from_request(req);
        assert_eq!(state.display_text(), "••••");
        assert_eq!(state.text, "秘密12");
    }
}
