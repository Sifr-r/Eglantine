// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::fs;
#[cfg(test)]
use std::io;
use std::path::Path;

const OVERLAY_FILE: &str = "android_overlay.tsv";
#[cfg(test)]
const OVERLAY_TEMP_FILE: &str = "android_overlay.tsv.tmp";

pub(super) const OVERLAY_EVENT_NONE: i32 = 0;
pub(super) const OVERLAY_EVENT_BUTTON: i32 = 1;
pub(super) const OVERLAY_EVENT_STICK: i32 = 2;
pub(super) const OVERLAY_EVENT_PAUSE_MENU: i32 = 3;

pub(super) const BUTTON_A: i32 = 0;
pub(super) const BUTTON_B: i32 = 1;
pub(super) const BUTTON_X: i32 = 2;
pub(super) const BUTTON_Y: i32 = 3;
pub(super) const TRIGGER_L: i32 = 6;
pub(super) const TRIGGER_R: i32 = 7;
pub(super) const TRIGGER_ZL: i32 = 8;
pub(super) const TRIGGER_ZR: i32 = 9;
pub(super) const BUTTON_PLUS: i32 = 10;
pub(super) const BUTTON_MINUS: i32 = 11;
pub(super) const BUTTON_LEFT: i32 = 12;
pub(super) const BUTTON_UP: i32 = 13;
pub(super) const BUTTON_RIGHT: i32 = 14;
pub(super) const BUTTON_DOWN: i32 = 15;

pub(super) const STICK_LEFT: i32 = 0;
pub(super) const STICK_RIGHT: i32 = 1;

const PAUSE_CONTROL_ID: i32 = -1;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct OverlayEvent {
    pub kind: i32,
    pub consumed: bool,
    pub id: i32,
    pub pressed: bool,
    pub x: f32,
    pub y: f32,
}

impl OverlayEvent {
    const fn passthrough() -> Self {
        Self {
            kind: OVERLAY_EVENT_NONE,
            consumed: false,
            id: 0,
            pressed: false,
            x: 0.0,
            y: 0.0,
        }
    }

    const fn consumed_none() -> Self {
        Self {
            kind: OVERLAY_EVENT_NONE,
            consumed: true,
            id: 0,
            pressed: false,
            x: 0.0,
            y: 0.0,
        }
    }
}

#[derive(Clone, Copy)]
struct ButtonControl {
    id: i32,
    cx: f32,
    cy: f32,
    radius: f32,
}

#[derive(Clone, Copy)]
struct StickControl {
    id: i32,
    cx: f32,
    cy: f32,
    radius: f32,
}

#[derive(Clone, Copy)]
enum TouchBinding {
    Pause,
    Button(i32),
    Stick(usize),
}

#[derive(Clone, Copy)]
struct ActiveTouch {
    finger_id: i64,
    binding: TouchBinding,
}

pub(super) struct OverlayState {
    enabled: bool,
    hide_on_controller: bool,
    controller_active: bool,
    opacity: f32,
    scale: f32,
    buttons: Vec<ButtonControl>,
    sticks: [StickControl; 2],
    active_touches: Vec<ActiveTouch>,
}

impl Default for OverlayState {
    fn default() -> Self {
        Self {
            enabled: true,
            hide_on_controller: true,
            controller_active: false,
            opacity: 0.65,
            scale: 1.0,
            buttons: default_buttons(),
            sticks: default_sticks(),
            active_touches: Vec::new(),
        }
    }
}

impl OverlayState {
    pub(super) fn is_visible(&self) -> bool {
        self.enabled && !(self.hide_on_controller && self.controller_active)
    }

    pub(super) fn set_controller_active(&mut self, active: bool) -> bool {
        let was_visible = self.is_visible();
        self.controller_active = active;
        if was_visible && !self.is_visible() && !self.active_touches.is_empty() {
            self.active_touches.clear();
            return true;
        }
        false
    }

    pub(super) fn reset_touches(&mut self) {
        self.active_touches.clear();
    }

    pub(super) fn touch_down(&mut self, finger_id: i64, nx: f32, ny: f32) -> OverlayEvent {
        if !self.is_visible() || !nx.is_finite() || !ny.is_finite() {
            return OverlayEvent::passthrough();
        }
        self.active_touches.retain(|t| t.finger_id != finger_id);

        for control in &self.buttons {
            if within_circle(nx, ny, control.cx, control.cy, control.radius * self.scale) {
                if control.id == PAUSE_CONTROL_ID {
                    self.active_touches.push(ActiveTouch {
                        finger_id,
                        binding: TouchBinding::Pause,
                    });
                    return OverlayEvent {
                        kind: OVERLAY_EVENT_PAUSE_MENU,
                        consumed: true,
                        id: 0,
                        pressed: true,
                        x: 0.0,
                        y: 0.0,
                    };
                }
                self.active_touches.push(ActiveTouch {
                    finger_id,
                    binding: TouchBinding::Button(control.id),
                });
                return OverlayEvent {
                    kind: OVERLAY_EVENT_BUTTON,
                    consumed: true,
                    id: control.id,
                    pressed: true,
                    x: 0.0,
                    y: 0.0,
                };
            }
        }

        for (slot, stick) in self.sticks.iter().enumerate() {
            let radius = stick.radius * self.scale;
            if within_circle(nx, ny, stick.cx, stick.cy, radius) {
                self.active_touches.push(ActiveTouch {
                    finger_id,
                    binding: TouchBinding::Stick(slot),
                });
                let (sx, sy) = stick_axes(stick, radius, nx, ny);
                return OverlayEvent {
                    kind: OVERLAY_EVENT_STICK,
                    consumed: true,
                    id: stick.id,
                    pressed: true,
                    x: sx,
                    y: sy,
                };
            }
        }

        OverlayEvent::passthrough()
    }

    pub(super) fn touch_move(&mut self, finger_id: i64, nx: f32, ny: f32) -> OverlayEvent {
        if !nx.is_finite() || !ny.is_finite() {
            return OverlayEvent::passthrough();
        }
        let Some(active) = self.active_touches.iter().find(|t| t.finger_id == finger_id).copied()
        else {
            return OverlayEvent::passthrough();
        };
        match active.binding {
            TouchBinding::Pause | TouchBinding::Button(_) => OverlayEvent::consumed_none(),
            TouchBinding::Stick(slot) => {
                let stick = self.sticks[slot];
                let radius = stick.radius * self.scale;
                let (sx, sy) = stick_axes(&stick, radius, nx, ny);
                OverlayEvent {
                    kind: OVERLAY_EVENT_STICK,
                    consumed: true,
                    id: stick.id,
                    pressed: true,
                    x: sx,
                    y: sy,
                }
            }
        }
    }

    pub(super) fn touch_up(&mut self, finger_id: i64) -> OverlayEvent {
        let Some(pos) = self
            .active_touches
            .iter()
            .position(|t| t.finger_id == finger_id)
        else {
            return OverlayEvent::passthrough();
        };
        let active = self.active_touches.swap_remove(pos);
        match active.binding {
            TouchBinding::Pause => OverlayEvent::consumed_none(),
            TouchBinding::Button(id) => OverlayEvent {
                kind: OVERLAY_EVENT_BUTTON,
                consumed: true,
                id,
                pressed: false,
                x: 0.0,
                y: 0.0,
            },
            TouchBinding::Stick(slot) => OverlayEvent {
                kind: OVERLAY_EVENT_STICK,
                consumed: true,
                id: self.sticks[slot].id,
                pressed: false,
                x: 0.0,
                y: 0.0,
            },
        }
    }

    pub(super) fn load_from_dir(dir: &Path) -> Self {
        let mut state = Self::default();
        let Ok(contents) = fs::read_to_string(dir.join(OVERLAY_FILE)) else {
            return state;
        };
        for line in contents.lines() {
            let mut parts = line.split('\t');
            let (Some(key), Some(val), None) = (parts.next(), parts.next(), parts.next()) else {
                continue;
            };
            match key {
                "enabled" => {
                    if let Ok(parsed) = val.parse::<bool>() {
                        state.enabled = parsed;
                    }
                }
                "hide_on_controller" => {
                    if let Ok(parsed) = val.parse::<bool>() {
                        state.hide_on_controller = parsed;
                    }
                }
                "opacity" => {
                    if let Ok(parsed) = val.parse::<f32>()
                        && parsed.is_finite()
                    {
                        state.opacity = parsed.clamp(0.1, 1.0);
                    }
                }
                "scale" => {
                    if let Ok(parsed) = val.parse::<f32>()
                        && parsed.is_finite()
                    {
                        state.scale = parsed.clamp(0.5, 1.5);
                    }
                }
                _ => {}
            }
        }
        state
    }

    #[cfg(test)]
    pub(super) fn save_to_dir(&self, dir: &Path) -> io::Result<()> {
        fs::create_dir_all(dir)?;
        let out = format!(
            "enabled\t{}\nhide_on_controller\t{}\nopacity\t{:.3}\nscale\t{:.3}\n",
            self.enabled, self.hide_on_controller, self.opacity, self.scale
        );
        let temp_path = dir.join(OVERLAY_TEMP_FILE);
        let final_path = dir.join(OVERLAY_FILE);
        fs::write(&temp_path, out.as_bytes())?;
        fs::rename(temp_path, final_path)
    }
}

fn within_circle(px: f32, py: f32, cx: f32, cy: f32, radius: f32) -> bool {
    let dx = px - cx;
    let dy = py - cy;
    dx * dx + dy * dy <= radius * radius
}

fn stick_axes(stick: &StickControl, radius: f32, nx: f32, ny: f32) -> (f32, f32) {
    if radius <= f32::EPSILON {
        return (0.0, 0.0);
    }
    let dx = (nx - stick.cx) / radius;
    let dy = (stick.cy - ny) / radius;
    let len = (dx * dx + dy * dy).sqrt();
    if len > 1.0 {
        (dx / len, dy / len)
    } else {
        (dx, dy)
    }
}

fn default_buttons() -> Vec<ButtonControl> {
    vec![
        ButtonControl { id: PAUSE_CONTROL_ID, cx: 0.50, cy: 0.06, radius: 0.045 },
        ButtonControl { id: TRIGGER_ZL, cx: 0.08, cy: 0.14, radius: 0.05 },
        ButtonControl { id: TRIGGER_L, cx: 0.08, cy: 0.28, radius: 0.05 },
        ButtonControl { id: TRIGGER_ZR, cx: 0.92, cy: 0.14, radius: 0.05 },
        ButtonControl { id: TRIGGER_R, cx: 0.92, cy: 0.28, radius: 0.05 },
        ButtonControl { id: BUTTON_MINUS, cx: 0.36, cy: 0.88, radius: 0.04 },
        ButtonControl { id: BUTTON_PLUS, cx: 0.64, cy: 0.88, radius: 0.04 },
        ButtonControl { id: BUTTON_UP, cx: 0.14, cy: 0.44, radius: 0.04 },
        ButtonControl { id: BUTTON_DOWN, cx: 0.14, cy: 0.62, radius: 0.04 },
        ButtonControl { id: BUTTON_LEFT, cx: 0.08, cy: 0.53, radius: 0.04 },
        ButtonControl { id: BUTTON_RIGHT, cx: 0.20, cy: 0.53, radius: 0.04 },
        ButtonControl { id: BUTTON_X, cx: 0.86, cy: 0.44, radius: 0.04 },
        ButtonControl { id: BUTTON_B, cx: 0.86, cy: 0.62, radius: 0.04 },
        ButtonControl { id: BUTTON_Y, cx: 0.80, cy: 0.53, radius: 0.04 },
        ButtonControl { id: BUTTON_A, cx: 0.92, cy: 0.53, radius: 0.04 },
    ]
}

const fn default_sticks() -> [StickControl; 2] {
    [
        StickControl { id: STICK_LEFT, cx: 0.16, cy: 0.78, radius: 0.10 },
        StickControl { id: STICK_RIGHT, cx: 0.82, cy: 0.78, radius: 0.10 },
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn button_press_move_release_are_consumed_and_center_passes_through() {
        let mut overlay = OverlayState::default();
        let center = overlay.touch_down(1, 0.5, 0.5);
        assert!(!center.consumed);

        let down = overlay.touch_down(2, 0.92, 0.53);
        assert!(down.consumed);
        assert_eq!(down.kind, OVERLAY_EVENT_BUTTON);
        assert_eq!(down.id, BUTTON_A);
        assert!(down.pressed);

        let moved = overlay.touch_move(2, 0.91, 0.52);
        assert!(moved.consumed);
        assert_eq!(moved.kind, OVERLAY_EVENT_NONE);

        let up = overlay.touch_up(2);
        assert!(up.consumed);
        assert_eq!(up.kind, OVERLAY_EVENT_BUTTON);
        assert_eq!(up.id, BUTTON_A);
        assert!(!up.pressed);
    }

    #[test]
    fn stick_clamps_to_unit_circle_and_resets_on_release() {
        let mut overlay = OverlayState::default();
        let down = overlay.touch_down(1, 0.16, 0.78);
        assert!(down.consumed);
        assert_eq!(down.kind, OVERLAY_EVENT_STICK);
        assert_eq!(down.id, STICK_LEFT);

        let dragged = overlay.touch_move(1, 0.16, 0.38);
        assert!(dragged.consumed);
        assert!((dragged.x - 0.0).abs() < 1e-4);
        assert!((dragged.y - 1.0).abs() < 1e-4);

        let up = overlay.touch_up(1);
        assert!(up.consumed);
        assert_eq!(up.x, 0.0);
        assert_eq!(up.y, 0.0);
    }

    #[test]
    fn controller_auto_hide_disables_overlay_and_clears_touches() {
        let mut overlay = OverlayState::default();
        assert!(overlay.touch_down(1, 0.92, 0.53).consumed);
        assert!(overlay.set_controller_active(true));
        assert!(!overlay.is_visible());
        assert!(!overlay.touch_down(2, 0.92, 0.53).consumed);
        assert!(!overlay.set_controller_active(false));
        assert!(overlay.is_visible());
    }

    #[test]
    fn persistence_round_trips_overlay_settings() {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let dir = std::env::temp_dir().join(format!("eglantine_overlay_{}_{}", std::process::id(), nanos));
        let overlay = OverlayState {
            enabled: false,
            hide_on_controller: false,
            opacity: 0.45,
            scale: 1.25,
            ..OverlayState::default()
        };
        overlay.save_to_dir(&dir).unwrap();

        let reloaded = OverlayState::load_from_dir(&dir);
        assert!(!reloaded.enabled);
        assert!(!reloaded.hide_on_controller);
        assert!((reloaded.opacity - 0.45).abs() < 1e-3);
        assert!((reloaded.scale - 1.25).abs() < 1e-3);
        let _ = fs::remove_dir_all(&dir);
    }
}
