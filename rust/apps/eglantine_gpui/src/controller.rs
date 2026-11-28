// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::io::{BufRead, BufReader, Read};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::thread;
use std::time::{Duration, Instant};

use serde::Deserialize;

use crate::navigation::Direction;

const BRIDGE_PROTOCOL_VERSION: u32 = 1;
const MAX_BRIDGE_FRAME: u64 = 64 * 1024;
const STOP_TIMEOUT: Duration = Duration::from_millis(1500);

const BTN_B: u32 = 0x0000_0004;
const BTN_A: u32 = 0x0000_0008;
const BTN_R: u32 = 0x0000_0040;
const BTN_MINUS: u32 = 0x0000_0100;
const BTN_PLUS: u32 = 0x0000_0200;
const BTN_STICK_R: u32 = 0x0000_0400;
const BTN_STICK_L: u32 = 0x0000_0800;
const BTN_HOME: u32 = 0x0000_1000;
const BTN_CAPTURE: u32 = 0x0000_2000;
const BTN_DOWN: u32 = 0x0001_0000;
const BTN_UP: u32 = 0x0002_0000;
const BTN_RIGHT: u32 = 0x0004_0000;
const BTN_LEFT: u32 = 0x0008_0000;
const BTN_L: u32 = 0x0040_0000;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Input {
    Move(Direction),
    Accept,
    Back,
    Options,
    ConsoleStep(i32),
    Home,
}

#[derive(Debug, Deserialize)]
struct BridgeEnvelope {
    version: u32,
    #[serde(flatten)]
    event: BridgeWireEvent,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "event", rename_all = "snake_case")]
enum BridgeWireEvent {
    Controller { name: Option<String> },
    ControllerInput { buttons: u32, left_x: f32, left_y: f32 },
    #[serde(other)]
    Other,
}

#[derive(Debug, PartialEq)]
enum Switch2Event {
    Controller(Option<String>),
    Input { buttons: u32, left_x: f32, left_y: f32 },
}

struct Switch2Bridge {
    child: Child,
    stdin: Option<ChildStdin>,
    receiver: Receiver<Switch2Event>,
}

impl Switch2Bridge {
    fn start() -> Option<Self> {
        let executable = std::env::current_exe()
            .ok()?
            .with_file_name(format!("eden-cli{}", std::env::consts::EXE_SUFFIX));
        let mut child = Command::new(executable)
            .arg("--gpui-controller")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .ok()?;
        let stdin = child.stdin.take();
        let stdout = child.stdout.take()?;
        let (sender, receiver) = mpsc::sync_channel(64);
        thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                match read_bridge_event(&mut reader) {
                    Ok(Some(Some(event))) => {
                        if sender.send(event).is_err() {
                            break;
                        }
                    }
                    Ok(Some(None)) => {}
                    Ok(None) | Err(()) => break,
                }
            }
        });
        Some(Self { child, stdin, receiver })
    }

    fn drain(&self) -> Vec<Switch2Event> {
        let mut events = Vec::new();
        while let Ok(event) = self.receiver.try_recv() {
            events.push(event);
        }
        events
    }

    fn stop(&mut self) {
        drop(self.stdin.take());
        let deadline = Instant::now() + STOP_TIMEOUT;
        while Instant::now() < deadline {
            match self.child.try_wait() {
                Ok(Some(_)) => return,
                Ok(None) => thread::sleep(Duration::from_millis(15)),
                Err(_) => break,
            }
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Drop for Switch2Bridge {
    fn drop(&mut self) {
        self.stop();
    }
}

fn read_bridge_event(reader: &mut impl BufRead) -> Result<Option<Option<Switch2Event>>, ()> {
    let mut frame = Vec::new();
    let size = reader
        .take(MAX_BRIDGE_FRAME + 1)
        .read_until(b'\n', &mut frame)
        .map_err(|_| ())?;
    if size == 0 {
        return Ok(None);
    }
    if size as u64 > MAX_BRIDGE_FRAME || frame.last() != Some(&b'\n') {
        return Err(());
    }
    let envelope: BridgeEnvelope = serde_json::from_slice(&frame).map_err(|_| ())?;
    if envelope.version != BRIDGE_PROTOCOL_VERSION {
        return Err(());
    }
    let mapped = match envelope.event {
        BridgeWireEvent::Controller { name } => Some(Switch2Event::Controller(name)),
        BridgeWireEvent::ControllerInput { buttons, left_x, left_y } => {
            Some(Switch2Event::Input { buttons, left_x, left_y })
        }
        BridgeWireEvent::Other => None,
    };
    Ok(Some(mapped))
}

fn map_switch2_button_edges(prev_buttons: u32, next_buttons: u32, home_button: &str) -> Vec<Input> {
    let pressed = next_buttons & !prev_buttons;
    if pressed == 0 {
        return Vec::new();
    }

    let fallback_home_mask = match home_button {
        "back" => BTN_MINUS,
        "start" => BTN_PLUS,
        "leftstick" => BTN_STICK_L,
        "rightstick" => BTN_STICK_R,
        "misc1" => BTN_CAPTURE,
        _ => 0,
    };

    let mut actions = Vec::new();
    if (pressed & (BTN_HOME | fallback_home_mask)) != 0 {
        actions.push(Input::Home);
        if (pressed & fallback_home_mask) != 0 && fallback_home_mask == BTN_PLUS {
            // Avoid double-triggering Options when Plus is explicitly bound as the Home fallback.
            if (pressed & BTN_UP) != 0 {
                actions.push(Input::Move(Direction::Up));
            }
            if (pressed & BTN_DOWN) != 0 {
                actions.push(Input::Move(Direction::Down));
            }
            if (pressed & BTN_LEFT) != 0 {
                actions.push(Input::Move(Direction::Left));
            }
            if (pressed & BTN_RIGHT) != 0 {
                actions.push(Input::Move(Direction::Right));
            }
            if (pressed & BTN_A) != 0 {
                actions.push(Input::Accept);
            }
            if (pressed & BTN_B) != 0 {
                actions.push(Input::Back);
            }
            if (pressed & BTN_L) != 0 {
                actions.push(Input::ConsoleStep(-1));
            }
            if (pressed & BTN_R) != 0 {
                actions.push(Input::ConsoleStep(1));
            }
            return actions;
        }
    }

    if (pressed & BTN_UP) != 0 {
        actions.push(Input::Move(Direction::Up));
    }
    if (pressed & BTN_DOWN) != 0 {
        actions.push(Input::Move(Direction::Down));
    }
    if (pressed & BTN_LEFT) != 0 {
        actions.push(Input::Move(Direction::Left));
    }
    if (pressed & BTN_RIGHT) != 0 {
        actions.push(Input::Move(Direction::Right));
    }
    if (pressed & BTN_A) != 0 {
        actions.push(Input::Accept);
    }
    if (pressed & BTN_B) != 0 {
        actions.push(Input::Back);
    }
    if (pressed & BTN_PLUS) != 0 {
        actions.push(Input::Options);
    }
    if (pressed & BTN_L) != 0 {
        actions.push(Input::ConsoleStep(-1));
    }
    if (pressed & BTN_R) != 0 {
        actions.push(Input::ConsoleStep(1));
    }
    actions
}

pub struct Controller {
    #[cfg(windows)]
    backend: Option<gilrs::Gilrs>,
    state: Option<StickRepeat>,
    native: Option<Switch2Bridge>,
    native_name: Option<String>,
    native_buttons: u32,
    native_stick: (f32, f32),
}

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
            state: None,
            native: Switch2Bridge::start(),
            native_name: None,
            native_buttons: 0,
            native_stick: (0.0, 0.0),
        }
    }

    pub fn suspend_native(&mut self) {
        if let Some(mut bridge) = self.native.take() {
            bridge.stop();
        }
        self.native_name = None;
        self.native_buttons = 0;
        self.native_stick = (0.0, 0.0);
    }

    pub fn resume_native(&mut self) {
        if self.native.is_none() {
            self.native = Switch2Bridge::start();
        }
    }

    pub fn name(&self) -> Option<String> {
        #[cfg(windows)]
        if let Some(backend) = &self.backend
            && let Some(name) = backend.gamepads().next().map(|(_, gamepad)| gamepad.name().to_owned())
        {
            return Some(name);
        }
        self.native_name.clone()
    }

    pub fn poll(&mut self, home_button: &str) -> Vec<Input> {
        let mut actions = Vec::new();
        let mut stick: Option<(f32, f32)> = None;

        #[cfg(windows)]
        if let Some(backend) = &mut self.backend {
            use gilrs::{Axis, Button, EventType};

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
                        Button::LeftTrigger => Input::ConsoleStep(-1),
                        Button::RightTrigger => Input::ConsoleStep(1),
                        _ => continue,
                    }
                };
                actions.push(action);
            }

            stick = backend.gamepads().next().map(|(_, gamepad)| {
                (gamepad.value(Axis::LeftStickX), gamepad.value(Axis::LeftStickY))
            });
        }

        if let Some(bridge) = &self.native {
            for event in bridge.drain() {
                match event {
                    Switch2Event::Controller(name) => {
                        self.native_name = name;
                        if self.native_name.is_none() {
                            self.native_buttons = 0;
                            self.native_stick = (0.0, 0.0);
                        }
                    }
                    Switch2Event::Input { buttons, left_x, left_y } => {
                        actions.extend(map_switch2_button_edges(
                            self.native_buttons,
                            buttons,
                            home_button,
                        ));
                        self.native_buttons = buttons;
                        self.native_stick = (left_x, left_y);
                    }
                }
            }
        }

        if stick.is_none() && self.native_name.is_some() {
            stick = Some(self.native_stick);
        }

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

        actions
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_switch2_bridge_frames_and_ignores_unrelated_events() {
        let mut ready = &b"{\"version\":1,\"event\":\"ready\"}\n"[..];
        assert_eq!(read_bridge_event(&mut ready), Ok(Some(None)));

        let mut connected = &b"{\"version\":1,\"event\":\"controller\",\"name\":\"Switch 2 Pro Controller\"}\n"[..];
        assert_eq!(
            read_bridge_event(&mut connected),
            Ok(Some(Some(Switch2Event::Controller(Some("Switch 2 Pro Controller".into())))))
        );

        let mut disconnected = &b"{\"version\":1,\"event\":\"controller\",\"name\":null}\n"[..];
        assert_eq!(
            read_bridge_event(&mut disconnected),
            Ok(Some(Some(Switch2Event::Controller(None))))
        );

        let mut input = &b"{\"version\":1,\"event\":\"controller_input\",\"buttons\":8,\"left_x\":0.75,\"left_y\":-0.25}\n"[..];
        assert_eq!(
            read_bridge_event(&mut input),
            Ok(Some(Some(Switch2Event::Input { buttons: 8, left_x: 0.75, left_y: -0.25 })))
        );

        let mut bad_ver = &b"{\"version\":2,\"event\":\"ready\"}\n"[..];
        assert!(read_bridge_event(&mut bad_ver).is_err());
    }

    #[test]
    fn maps_switch2_button_edges_and_home_fallbacks() {
        assert!(
            map_switch2_button_edges(0, BTN_A | BTN_UP | BTN_L, "guide")
                == vec![Input::Move(Direction::Up), Input::Accept, Input::ConsoleStep(-1)]
        );
        // Holding a button across frames must not re-fire.
        assert!(map_switch2_button_edges(BTN_A, BTN_A, "guide").is_empty());

        assert!(map_switch2_button_edges(0, BTN_HOME, "guide") == vec![Input::Home]);
        assert!(map_switch2_button_edges(0, BTN_MINUS, "back") == vec![Input::Home]);
        assert!(map_switch2_button_edges(0, BTN_PLUS, "start") == vec![Input::Home]);
        assert!(map_switch2_button_edges(0, BTN_PLUS, "guide") == vec![Input::Options]);
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
