// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#[cfg(windows)]
use std::time::{Duration, Instant};

use crate::navigation::Direction;

#[derive(Clone, Copy)]
pub enum Input {
    Move(Direction),
    Accept,
    Back,
    Options,
    Home,
}

pub struct Controller {
    #[cfg(windows)]
    backend: Option<gilrs::Gilrs>,
    #[cfg(windows)]
    state: Option<StickRepeat>,
}

#[cfg(windows)]
struct StickRepeat {
    direction: Direction,
    next: Instant,
}

impl Controller {
    pub fn new() -> Self {
        #[cfg(windows)]
        let backend = match gilrs::Gilrs::new() {
            Ok(backend) => Some(backend),
            Err(error) => {
                eprintln!("Couldn't initialize controller navigation: {error}");
                None
            }
        };
        Self {
            #[cfg(windows)]
            backend,
            #[cfg(windows)]
            state: None,
        }
    }

    pub fn name(&self) -> Option<String> {
        #[cfg(windows)]
        if let Some(backend) = &self.backend {
            return backend.gamepads().next().map(|(_, gamepad)| gamepad.name().to_owned());
        }
        None
    }

    pub fn poll(&mut self, home_button: &str) -> Vec<Input> {
        #[cfg(windows)]
        if let Some(backend) = &mut self.backend {
            use gilrs::{Axis, Button, EventType};

            let mut actions = Vec::new();
            while let Some(event) = backend.next_event() {
                let EventType::ButtonPressed(button, _) = event.event else {
                    continue;
                };
                let fallback_home = match home_button {
                    "back" => button == Button::Select,
                    "start" => button == Button::Start,
                    "leftstick" => button == Button::LeftThumb,
                    "rightstick" => button == Button::RightThumb,
                    "misc1" => button == Button::Unknown,
                    _ => false,
                };
                let gamepad = backend.gamepad(event.id);
                let switch_labels = gamepad.vendor_id() == Some(0x057e)
                    || gamepad.name().to_ascii_lowercase().contains("nintendo");
                let action = if fallback_home || button == Button::Mode {
                    Input::Home
                } else {
                    match button {
                        Button::DPadUp => Input::Move(Direction::Up),
                        Button::DPadDown => Input::Move(Direction::Down),
                        Button::DPadLeft => Input::Move(Direction::Left),
                        Button::DPadRight => Input::Move(Direction::Right),
                        Button::South if switch_labels => Input::Back,
                        Button::East if switch_labels => Input::Accept,
                        Button::South => Input::Accept,
                        Button::East => Input::Back,
                        Button::Start => Input::Options,
                        _ => continue,
                    }
                };
                actions.push(action);
            }

            let stick = backend.gamepads().next().map(|(_, gamepad)| {
                (gamepad.value(Axis::LeftStickX), gamepad.value(Axis::LeftStickY))
            });
            let direction = stick.and_then(|(x, y)| {
                if x.abs().max(y.abs()) < 0.55 {
                    return None;
                }
                Some(match (x.abs() > y.abs(), x.is_sign_positive(), y.is_sign_positive()) {
                    (true, true, _) => Direction::Right,
                    (true, false, _) => Direction::Left,
                    (false, _, true) => Direction::Up,
                    (false, _, false) => Direction::Down,
                })
            });
            let now = Instant::now();
            match (direction, &mut self.state) {
                (None, _) => self.state = None,
                (Some(direction), Some(repeat)) if repeat.direction == direction => {
                    if now >= repeat.next {
                        actions.push(Input::Move(direction));
                        repeat.next = now + Duration::from_millis(95);
                    }
                }
                (Some(direction), _) => {
                    actions.push(Input::Move(direction));
                    self.state = Some(StickRepeat {
                        direction,
                        next: now + Duration::from_millis(320),
                    });
                }
            }
            return actions;
        }
        Vec::new()
    }
}

pub fn reduced_motion() -> bool {
    #[cfg(windows)]
    {
        #[link(name = "user32")]
        unsafe extern "system" {
            fn SystemParametersInfoW(action: u32, param: u32, value: *mut i32, flags: u32) -> i32;
        }
        const GET_CLIENT_AREA_ANIMATION: u32 = 0x1042;
        let mut enabled = 1;
        // The action accepts a pointer to a writable Win32 BOOL.
        let success = unsafe { SystemParametersInfoW(GET_CLIENT_AREA_ANIMATION, 0, &mut enabled, 0) };
        success != 0 && enabled == 0
    }
    #[cfg(not(windows))]
    false
}

pub fn os_text_scale() -> f32 {
    #[cfg(windows)]
    {
        #[link(name = "advapi32")]
        unsafe extern "system" {
            fn RegGetValueW(
                hkey: isize,
                sub_key: *const u16,
                value: *const u16,
                flags: u32,
                pdw_type: *mut u32,
                pv_data: *mut u32,
                pcb_data: *mut u32,
            ) -> i32;
        }
        const HKEY_CURRENT_USER: isize = -0x7fff_ffff_isize; // 0x80000001
        const RRF_RT_REG_DWORD: u32 = 0x0000_0010;
        let sub_key: Vec<u16> = "Software\\Microsoft\\Accessibility\0".encode_utf16().collect();
        let value: Vec<u16> = "TextScaleFactor\0".encode_utf16().collect();
        let mut factor: u32 = 100;
        let mut size: u32 = std::mem::size_of::<u32>() as u32;
        let status = unsafe {
            RegGetValueW(
                HKEY_CURRENT_USER,
                sub_key.as_ptr(),
                value.as_ptr(),
                RRF_RT_REG_DWORD,
                std::ptr::null_mut(),
                &mut factor,
                &mut size,
            )
        };
        if status == 0 && (100..=225).contains(&factor) {
            return factor as f32 / 100.0;
        }
    }
    1.0
}
