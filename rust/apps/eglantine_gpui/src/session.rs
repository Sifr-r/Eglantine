// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use serde::Deserialize;
use crate::emulator_config::EmulatorConfig;
use crate::software_keyboard::{SwkbdShowRequest, SwkbdTextCheckKind};
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::thread;
use std::time::{Duration, Instant};

const PROTOCOL: u32 = 1;
const MAX_FRAME: u64 = 64 * 1024;
const BOOT_TIMEOUT: Duration = Duration::from_secs(180);
const CONTROL_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SessionEvent {
    Running,
    Paused,
    Stopped,
    HomeRequested,
    HomeReleased,
    ControllerClaimed,
    ActionCompleted,
    SoftwareKeyboardShow(SwkbdShowRequest),
    SoftwareKeyboardTextCheck {
        request_id: u64,
        result: SwkbdTextCheckKind,
        message: String,
    },
    SoftwareKeyboardClose {
        request_id: u64,
    },
    Error(String),
}

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
enum State {
    Starting,
    Running,
    Paused,
    Stopping,
    Stopped,
}

#[derive(Debug, Deserialize)]
struct Envelope {
    version: u32,
    #[serde(flatten)]
    event: WireEvent,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "event", rename_all = "snake_case")]
enum WireEvent {
    Ready,
    State { state: State },
    HomeRequested,
    HomeReleased,
    ControllerClaimed,
    SwkbdShow {
        request_id: u64,
        #[serde(default)]
        header_text: String,
        #[serde(default)]
        sub_text: String,
        #[serde(default)]
        guide_text: String,
        #[serde(default)]
        ok_text: String,
        #[serde(default)]
        initial_text: String,
        #[serde(default)]
        min_length: u32,
        #[serde(default)]
        max_length: u32,
        #[serde(default)]
        password: bool,
        #[serde(default)]
        multiline: bool,
        #[serde(default)]
        disable_cancel: bool,
    },
    SwkbdTextCheck {
        request_id: u64,
        result: SwkbdTextCheckKind,
        #[serde(default)]
        message: String,
    },
    SwkbdClose {
        request_id: u64,
    },
    Error { message: String },
    Reply { id: u64, ok: bool, state: Option<State>, message: Option<String> },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ViewportRect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EmbedTarget {
    pub parent_window: u64,
    pub viewport: ViewportRect,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GbaLaunchConfig {
    pub bios_path: Option<String>,
    pub color_correction: bool,
    pub interframe_blending: bool,
    pub cheats: Vec<String>,
}

struct Launch {
    game: PathBuf,
    fullscreen: bool,
    config: Option<PathBuf>,
    home_key: String,
    home_button: String,
    user_profile: Option<u8>,
    handoff: bool,
    gba: GbaLaunchConfig,
    embed: Option<EmbedTarget>,
    ble_handoff: Option<String>,
    _override_file: Option<tempfile::TempPath>,
}

struct Host {
    child: Child,
    input: Receiver<Result<WireEvent, String>>,
    launch: Launch,
    state: Option<State>,
    pending: Option<(u64, Instant)>,
    deadline: Instant,
    ready: bool,
    reported_error: bool,
    desired_viewport: Option<ViewportRect>,
    sent_viewport: Option<ViewportRect>,
}

#[derive(Default)]
pub struct SessionClient {
    host: Option<Host>,
    restart: bool,
    next_id: u64,
    pending_ble_handoff: Option<String>,
}

impl SessionClient {
    pub fn set_next_ble_handoff(&mut self, handoff: Option<String>) {
        self.pending_ble_handoff = handoff;
    }
    pub fn launch(&mut self, game: &Path, fullscreen: bool, config: Option<&Path>) -> Result<(), String> {
        self.launch_with_gba(game, fullscreen, config, GbaLaunchConfig::default())
    }

    pub fn launch_with_gba(
        &mut self,
        game: &Path,
        fullscreen: bool,
        config: Option<&Path>,
        gba: GbaLaunchConfig,
    ) -> Result<(), String> {
        self.launch_with_embed(game, fullscreen, config, gba, None)
    }

    pub fn launch_with_embed(
        &mut self,
        game: &Path,
        fullscreen: bool,
        config: Option<&Path>,
        gba: GbaLaunchConfig,
        embed: Option<EmbedTarget>,
    ) -> Result<(), String> {
        self.launch_with_config(Launch {
            game: game.into(),
            fullscreen,
            config: config.map(Path::to_path_buf),
            home_key: "F10".into(),
            home_button: "guide".into(),
            user_profile: None,
            handoff: false,
            gba,
            embed,
            ble_handoff: None,
            _override_file: None,
        })
    }

    pub fn launch_game(&mut self, game: &Path, config: &EmulatorConfig) -> Result<(), String> {
        self.launch_game_with_profile(game, config, None)
    }

    pub fn launch_game_with_profile(
        &mut self, game: &Path, config: &EmulatorConfig, user_profile_override: Option<u8>,
    ) -> Result<(), String> {
        self.launch_game_with_gba(game, config, user_profile_override, GbaLaunchConfig::default())
    }

    pub fn launch_game_with_gba(
        &mut self,
        game: &Path,
        config: &EmulatorConfig,
        user_profile_override: Option<u8>,
        gba: GbaLaunchConfig,
    ) -> Result<(), String> {
        self.launch_game_with_embed(game, config, user_profile_override, gba, None)
    }

    pub fn launch_game_with_embed(
        &mut self,
        game: &Path,
        config: &EmulatorConfig,
        user_profile_override: Option<u8>,
        gba: GbaLaunchConfig,
        embed: Option<EmbedTarget>,
    ) -> Result<(), String> {
        let (fullscreen, override_file) = config.session_file(game)?;
        let path = override_file.as_deref().unwrap_or(config.path()).to_path_buf();
        let user_profile = user_profile_override.or_else(|| {
            let profile = config.user_profile();
            (profile != 0).then_some(profile)
        });
        self.launch_with_config(Launch {
            game: game.into(),
            fullscreen,
            config: Some(path),
            home_key: config.home_key().to_owned(),
            home_button: config.home_button().to_owned(),
            user_profile,
            handoff: false,
            gba,
            embed,
            ble_handoff: None,
            _override_file: override_file,
        })
    }

    fn launch_with_config(&mut self, mut launch: Launch) -> Result<(), String> {
        if self.host.is_some() {
            return Err("A game session is already active".into());
        }
        if !launch.game.is_file() && !launch.game.is_dir() {
            return Err(format!("Game was not found at {}", launch.game.display()));
        }
        launch.ble_handoff = self.pending_ble_handoff.take();
        self.start(launch)
    }

    fn start(&mut self, launch: Launch) -> Result<(), String> {
        let executable = std::env::current_exe().map_err(|error| error.to_string())?
            .with_file_name(format!("eden-cli{}", std::env::consts::EXE_SUFFIX));
        let mut command = Command::new(&executable);
        command.arg("--gpui-session").arg("-g").arg(&launch.game);
        command.env_remove("EDEN_CLI_RESTART_HANDOFF");
        command.env("EGLANTINE_GPUI_HOME_KEY", &launch.home_key);
        command.env("EGLANTINE_GPUI_HOME_BUTTON", &launch.home_button);
        if let Some(bios) = &launch.gba.bios_path {
            command.env("EGLANTINE_GPUI_GBA_BIOS", bios);
        } else {
            command.env_remove("EGLANTINE_GPUI_GBA_BIOS");
        }
        command.env(
            "EGLANTINE_GPUI_GBA_COLOR_CORRECTION",
            if launch.gba.color_correction { "1" } else { "0" },
        );
        command.env(
            "EGLANTINE_GPUI_GBA_INTERFRAME_BLENDING",
            if launch.gba.interframe_blending { "1" } else { "0" },
        );
        if !launch.gba.cheats.is_empty() {
            command.env("EGLANTINE_GPUI_GBA_CHEATS", launch.gba.cheats.join(";"));
        } else {
            command.env_remove("EGLANTINE_GPUI_GBA_CHEATS");
        }
        if launch.handoff {
            command.env("EDEN_CLI_RESTART_HANDOFF", "1");
        }
        if let Some(handoff) = &launch.ble_handoff {
            command.env("EGLANTINE_SWITCH2_BLE_HANDOFF", handoff);
        } else {
            command.env_remove("EGLANTINE_SWITCH2_BLE_HANDOFF");
        }
        if let Some(embed) = launch.embed {
            command.env("EGLANTINE_GPUI_PARENT_HWND", embed.parent_window.to_string());
            command.env(
                "EGLANTINE_GPUI_VIEWPORT",
                format!(
                    "{},{},{},{}",
                    embed.viewport.x, embed.viewport.y, embed.viewport.width, embed.viewport.height
                ),
            );
        } else {
            command.env_remove("EGLANTINE_GPUI_PARENT_HWND");
            command.env_remove("EGLANTINE_GPUI_VIEWPORT");
            if launch.fullscreen {
                command.arg("-f");
            }
        }
        if let Some(profile) = launch.user_profile {
            command.arg("-u").arg(profile.to_string());
        }
        if let Some(config) = &launch.config {
            command.arg("-c").arg(config);
        }
        let mut child = command.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null()).spawn()
            .map_err(|error| format!("Could not start {}: {error}", executable.display()))?;
        let stdout = child.stdout.take().expect("piped host stdout");
        let (sender, input) = mpsc::sync_channel(64);
        thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                match read_event(&mut reader) {
                    Ok(Some(event)) => {
                        if sender.send(Ok(event)).is_err() {
                            break;
                        }
                    }
                    Ok(None) => break,
                    Err(error) => {
                        let _ = sender.send(Err(error));
                        break;
                    }
                }
            }
        });
        let initial_viewport = launch.embed.map(|embed| embed.viewport);
        self.host = Some(Host {
            child, input, launch, state: None, pending: None,
            deadline: Instant::now() + BOOT_TIMEOUT, ready: false, reported_error: false,
            desired_viewport: initial_viewport,
            sent_viewport: initial_viewport,
        });
        Ok(())
    }

    pub fn pause(&mut self) -> Result<(), String> { self.send("pause", Some(State::Running)) }
    pub fn resume(&mut self) -> Result<(), String> { self.send("resume", Some(State::Paused)) }
    pub fn stop(&mut self) -> Result<(), String> { self.send("stop", None) }

    pub fn save_state(&mut self, slot: u8) -> Result<(), String> {
        self.send_fields("save_state", None, serde_json::json!({ "slot": slot }))
    }

    pub fn load_state(&mut self, slot: u8) -> Result<(), String> {
        self.send_fields("load_state", None, serde_json::json!({ "slot": slot }))
    }

    pub fn set_fast_forward(&mut self, enabled: bool) -> Result<(), String> {
        self.send_fields("set_fast_forward", None, serde_json::json!({ "enabled": enabled }))
    }

    pub fn rewind_step(&mut self) -> Result<(), String> {
        self.send("rewind_step", None)
    }

    pub fn set_video_options(&mut self, color_correction: bool, interframe_blending: bool) -> Result<(), String> {
        self.send_fields(
            "set_video_options",
            None,
            serde_json::json!({
                "color_correction": color_correction,
                "interframe_blending": interframe_blending,
            }),
        )?;
        if let Some(host) = &mut self.host {
            host.launch.gba.color_correction = color_correction;
            host.launch.gba.interframe_blending = interframe_blending;
        }
        Ok(())
    }

    pub fn set_cheats(&mut self, codes: &[String]) -> Result<(), String> {
        self.send_fields("set_cheats", None, serde_json::json!({ "codes": codes }))
    }

    pub fn reset_game(&mut self) -> Result<(), String> {
        self.send("reset", None)
    }

    pub fn restart(&mut self) -> Result<(), String> {
        self.stop()?;
        self.restart = true;
        Ok(())
    }

    pub fn submit_software_keyboard(
        &mut self,
        request_id: u64,
        text: &str,
        confirmed: bool,
    ) -> Result<(), String> {
        self.send_fields(
            "swkbd_submit",
            None,
            serde_json::json!({
                "request_id": request_id,
                "text": text,
                "confirmed": confirmed,
            }),
        )
    }

    pub fn cancel_software_keyboard(&mut self, request_id: u64) -> Result<(), String> {
        self.send_fields(
            "swkbd_cancel",
            None,
            serde_json::json!({
                "request_id": request_id,
            }),
        )
    }

    pub fn sync_viewport(&mut self, viewport: ViewportRect) {
        if let Some(host) = &mut self.host {
            host.desired_viewport = Some(viewport);
            if let Some(embed) = &mut host.launch.embed {
                embed.viewport = viewport;
            }
        }
    }

    fn send(&mut self, command: &str, state: Option<State>) -> Result<(), String> {
        self.send_fields(command, state, serde_json::json!({}))
    }

    fn send_fields(&mut self, command: &str, state: Option<State>, extra: serde_json::Value) -> Result<(), String> {
        let host = self.host.as_mut().ok_or("No game session is active")?;
        if !host.ready || host.pending.is_some()
            || !matches!(host.state, Some(State::Running | State::Paused))
            || state.is_some_and(|state| Some(state) != host.state)
        {
            return Err("The game session cannot perform that action yet".into());
        }
        self.next_id += 1;
        let id = self.next_id;
        let line = build_command_line(id, command, extra)?;
        let input = host.child.stdin.as_mut().ok_or("The game host input was closed")?;
        writeln!(input, "{line}")
            .map_err(|error| format!("Could not send {command} to the game: {error}"))?;
        host.pending = Some((id, Instant::now() + CONTROL_TIMEOUT));
        Ok(())
    }

    pub fn poll(&mut self) -> Vec<SessionEvent> {
        let mut events = Vec::new();
        let Some(host) = self.host.as_mut() else { return events };
        drain_events(host, &mut events, &mut self.restart);
        let now = Instant::now();
        let timed_out = (matches!(host.state, None | Some(State::Starting | State::Stopping | State::Stopped))
            && now >= host.deadline) || host.pending.is_some_and(|(_, deadline)| now >= deadline);
        if timed_out {
            let _ = host.child.kill();
            host.pending = None;
            host.deadline = now + CONTROL_TIMEOUT;
            events.push(SessionEvent::Error("The game host did not respond in time".into()));
            host.reported_error = true;
            self.restart = false;
        }
        let status = match host.child.try_wait() {
            Ok(Some(status)) => status,
            Ok(None) => {
                let pending_viewport = (host.ready
                    && host.pending.is_none()
                    && matches!(host.state, Some(State::Running | State::Paused)))
                .then(|| {
                    host.desired_viewport
                        .filter(|viewport| host.sent_viewport != Some(*viewport))
                })
                .flatten();
                if let Some(viewport) = pending_viewport {
                    if self
                        .send_fields(
                            "set_viewport",
                            None,
                            serde_json::json!({
                                "x": viewport.x,
                                "y": viewport.y,
                                "width": viewport.width,
                                "height": viewport.height,
                            }),
                        )
                        .is_ok()
                        && let Some(host) = &mut self.host
                    {
                        host.sent_viewport = Some(viewport);
                    }
                }
                return events;
            }
            Err(error) => {
                self.restart = false;
                let _ = host.child.kill();
                events.push(SessionEvent::Error(format!("Could not supervise the game host: {error}")));
                events.push(SessionEvent::Stopped);
                self.host = None;
                return events;
            }
        };
        // A short-lived host can exit between the channel drain and the process status check.
        drain_events(host, &mut events, &mut self.restart);
        let mut host = self.host.take().expect("active host was checked above");
        let applet_restarted = status.code() == Some(75);
        if !host.reported_error && !status.success() && !applet_restarted {
            events.push(SessionEvent::Error(format!("The game host exited with {status}")));
            host.reported_error = true;
        }
        if (std::mem::take(&mut self.restart) || applet_restarted) && !host.reported_error {
            host.launch.handoff = applet_restarted;
            match self.start(host.launch) {
                Ok(()) => return events,
                Err(error) => events.push(SessionEvent::Error(error)),
            }
        }
        events.push(SessionEvent::Stopped);
        events
    }
}

fn drain_events(host: &mut Host, events: &mut Vec<SessionEvent>, restart: &mut bool) {
    while let Ok(event) = host.input.try_recv() {
        match event {
            Ok(WireEvent::Ready) => host.ready = true,
            Ok(WireEvent::State { state }) => apply_state(host, state, events),
            Ok(WireEvent::HomeRequested) => events.push(SessionEvent::HomeRequested),
            Ok(WireEvent::HomeReleased) => events.push(SessionEvent::HomeReleased),
            Ok(WireEvent::ControllerClaimed) => events.push(SessionEvent::ControllerClaimed),
            Ok(WireEvent::SwkbdShow {
                request_id,
                header_text,
                sub_text,
                guide_text,
                ok_text,
                initial_text,
                min_length,
                max_length,
                password,
                multiline,
                disable_cancel,
            }) => events.push(SessionEvent::SoftwareKeyboardShow(SwkbdShowRequest {
                request_id,
                header_text,
                sub_text,
                guide_text,
                ok_text,
                initial_text,
                min_length,
                max_length,
                password,
                multiline,
                disable_cancel,
            })),
            Ok(WireEvent::SwkbdTextCheck { request_id, result, message }) => {
                events.push(SessionEvent::SoftwareKeyboardTextCheck { request_id, result, message });
            }
            Ok(WireEvent::SwkbdClose { request_id }) => {
                events.push(SessionEvent::SoftwareKeyboardClose { request_id });
            }
            Ok(WireEvent::Error { message }) => {
                host.reported_error = true;
                events.push(SessionEvent::Error(message));
            }
            Err(message) => {
                host.reported_error = true;
                let _ = host.child.kill();
                events.push(SessionEvent::Error(message));
            }
            Ok(WireEvent::Reply { id, ok, state, message }) => {
                if !host.pending.is_some_and(|(pending, _)| pending == id) {
                    continue;
                }
                host.pending = None;
                if ok {
                    if let Some(state) = state {
                        apply_state(host, state, events);
                    }
                    events.push(SessionEvent::ActionCompleted);
                } else {
                    *restart = false;
                    events.push(SessionEvent::Error(message.unwrap_or_else(|| "Game action was rejected".into())));
                }
            }
        }
    }
}

fn apply_state(host: &mut Host, state: State, events: &mut Vec<SessionEvent>) {
    if host.state.replace(state) == Some(state) {
        return;
    }
    match state {
        State::Starting => {}
        State::Running => events.push(SessionEvent::Running),
        State::Paused => events.push(SessionEvent::Paused),
        State::Stopping | State::Stopped => host.deadline = Instant::now() + CONTROL_TIMEOUT,
    }
}

impl Drop for SessionClient {
    fn drop(&mut self) {
        if let Some(host) = &mut self.host {
            // EOF asks the host to tear down even when a reply is outstanding.
            drop(host.child.stdin.take());
            let deadline = Instant::now() + CONTROL_TIMEOUT;
            while Instant::now() < deadline {
                match host.child.try_wait() {
                    Ok(Some(_)) => return,
                    Ok(None) => thread::sleep(Duration::from_millis(20)),
                    Err(_) => break,
                }
            }
            let _ = host.child.kill();
            let _ = host.child.wait();
        }
    }
}

fn build_command_line(id: u64, command: &str, mut extra: serde_json::Value) -> Result<String, String> {
    let obj = extra.as_object_mut().ok_or("Session command payload must be an object")?;
    obj.insert("version".into(), serde_json::json!(PROTOCOL));
    obj.insert("id".into(), serde_json::json!(id));
    obj.insert("command".into(), serde_json::json!(command));
    serde_json::to_string(&extra).map_err(|error| format!("Could not encode {command}: {error}"))
}

fn read_event(reader: &mut impl BufRead) -> Result<Option<WireEvent>, String> {
    let mut frame = Vec::new();
    let size = reader.take(MAX_FRAME + 1).read_until(b'\n', &mut frame)
        .map_err(|error| format!("Could not read game host event: {error}"))?;
    if size == 0 {
        return Ok(None);
    }
    if size as u64 > MAX_FRAME || frame.last() != Some(&b'\n') {
        return Err("Game host sent an incomplete or oversized event".into());
    }
    let envelope: Envelope = serde_json::from_slice(&frame)
        .map_err(|error| format!("Invalid game host event: {error}"))?;
    if envelope.version != PROTOCOL {
        return Err(format!("Game host protocol {} is unsupported", envelope.version));
    }
    Ok(Some(envelope.event))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_acknowledged_state_and_rejects_bad_frames() {
        let mut frame = &b"{\"version\":1,\"event\":\"reply\",\"id\":17,\"ok\":true,\"state\":\"paused\"}\n"[..];
        assert!(matches!(read_event(&mut frame), Ok(Some(WireEvent::Reply { id: 17, ok: true, state: Some(State::Paused), .. }))));
        assert!(matches!(read_event(&mut frame), Ok(None)));
        assert!(matches!(
            read_event(&mut &b"{\"version\":1,\"event\":\"controller_claimed\"}\n"[..]),
            Ok(Some(WireEvent::ControllerClaimed))
        ));
        assert!(read_event(&mut &b"{\"version\":2,\"event\":\"ready\"}\n"[..]).is_err());
        assert!(read_event(&mut &b"{\"version\":1,\"event\":\"ready\"}"[..]).is_err());
        assert!(read_event(&mut &vec![b' '; MAX_FRAME as usize + 1][..]).is_err());
    }

    #[test]
    fn encodes_extended_gba_session_commands() {
        let line = build_command_line(4, "save_state", serde_json::json!({ "slot": 3 })).unwrap();
        let decoded: serde_json::Value = serde_json::from_str(&line).unwrap();
        assert_eq!(decoded["version"], 1);
        assert_eq!(decoded["id"], 4);
        assert_eq!(decoded["command"], "save_state");
        assert_eq!(decoded["slot"], 3);

        let cheats = build_command_line(
            5,
            "set_cheats",
            serde_json::json!({ "codes": ["02000010:BEEF"] }),
        ).unwrap();
        let decoded_cheats: serde_json::Value = serde_json::from_str(&cheats).unwrap();
        assert_eq!(decoded_cheats["command"], "set_cheats");
        assert_eq!(decoded_cheats["codes"][0], "02000010:BEEF");

        let viewport = build_command_line(
            6,
            "set_viewport",
            serde_json::json!({ "x": 0, "y": 48, "width": 1280, "height": 672 }),
        ).unwrap();
        let decoded_viewport: serde_json::Value = serde_json::from_str(&viewport).unwrap();
        assert_eq!(decoded_viewport["command"], "set_viewport");
        assert_eq!(decoded_viewport["x"], 0);
        assert_eq!(decoded_viewport["y"], 48);
        assert_eq!(decoded_viewport["width"], 1280);
        assert_eq!(decoded_viewport["height"], 672);
    }

    #[test]
    fn parses_and_encodes_software_keyboard_protocol() {
        let mut show = r#"{"version":1,"event":"swkbd_show","request_id":3,"header_text":"Name","initial_text":"Églantine","min_length":2,"max_length":16,"password":false,"multiline":false,"disable_cancel":false}
"#.as_bytes();
        match read_event(&mut show).unwrap().unwrap() {
            WireEvent::SwkbdShow { request_id, header_text, initial_text, min_length, max_length, .. } => {
                assert_eq!(request_id, 3);
                assert_eq!(header_text, "Name");
                assert_eq!(initial_text, "Églantine");
                assert_eq!((min_length, max_length), (2, 16));
            }
            other => panic!("unexpected event: {other:?}"),
        }

        let mut check = r#"{"version":1,"event":"swkbd_text_check","request_id":3,"result":"confirm","message":"Use this?"}
"#.as_bytes();
        assert!(matches!(
            read_event(&mut check).unwrap().unwrap(),
            WireEvent::SwkbdTextCheck { request_id: 3, result: SwkbdTextCheckKind::Confirm, message }
            if message == "Use this?"
        ));

        let mut close = r#"{"version":1,"event":"swkbd_close","request_id":3}
"#.as_bytes();
        assert!(matches!(read_event(&mut close).unwrap().unwrap(), WireEvent::SwkbdClose { request_id: 3 }));

        let submit = build_command_line(
            9,
            "swkbd_submit",
            serde_json::json!({ "request_id": 3, "text": "日本語", "confirmed": true }),
        ).unwrap();
        let decoded_submit: serde_json::Value = serde_json::from_str(&submit).unwrap();
        assert_eq!(decoded_submit["command"], "swkbd_submit");
        assert_eq!(decoded_submit["request_id"], 3);
        assert_eq!(decoded_submit["text"], "日本語");
        assert_eq!(decoded_submit["confirmed"], true);

        let cancel = build_command_line(10, "swkbd_cancel", serde_json::json!({ "request_id": 3 })).unwrap();
        let decoded_cancel: serde_json::Value = serde_json::from_str(&cancel).unwrap();
        assert_eq!(decoded_cancel["command"], "swkbd_cancel");
        assert_eq!(decoded_cancel["request_id"], 3);
    }
}
