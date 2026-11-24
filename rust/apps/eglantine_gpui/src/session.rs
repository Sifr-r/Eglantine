// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use serde::Deserialize;
use crate::emulator_config::EmulatorConfig;
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
    Error { message: String },
    Reply { id: u64, ok: bool, state: Option<State>, message: Option<String> },
}

struct Launch {
    game: PathBuf,
    fullscreen: bool,
    config: Option<PathBuf>,
    home_key: String,
    home_button: String,
    user_profile: Option<u8>,
    handoff: bool,
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
}

#[derive(Default)]
pub struct SessionClient {
    host: Option<Host>,
    restart: bool,
    next_id: u64,
}

impl SessionClient {
    pub fn launch(&mut self, game: &Path, fullscreen: bool, config: Option<&Path>) -> Result<(), String> {
        self.launch_with_config(game, fullscreen, config.map(Path::to_path_buf), "F10", "guide", None, None)
    }

    pub fn launch_game(&mut self, game: &Path, config: &EmulatorConfig) -> Result<(), String> {
        self.launch_game_with_profile(game, config, None)
    }

    pub fn launch_game_with_profile(
        &mut self, game: &Path, config: &EmulatorConfig, user_profile_override: Option<u8>,
    ) -> Result<(), String> {
        let (fullscreen, override_file) = config.session_file(game)?;
        let path = override_file.as_deref().unwrap_or(config.path()).to_path_buf();
        let user_profile = user_profile_override.or_else(|| {
            let profile = config.user_profile();
            (profile != 0).then_some(profile)
        });
        self.launch_with_config(
            game,
            fullscreen,
            Some(path),
            config.home_key(),
            config.home_button(),
            user_profile,
            override_file,
        )
    }

    fn launch_with_config(
        &mut self, game: &Path, fullscreen: bool, config: Option<PathBuf>,
        home_key: &str, home_button: &str, user_profile: Option<u8>,
        override_file: Option<tempfile::TempPath>,
    ) -> Result<(), String> {
        if self.host.is_some() {
            return Err("A game session is already active".into());
        }
        if !game.is_file() && !game.is_dir() {
            return Err(format!("Game was not found at {}", game.display()));
        }
        self.start(Launch {
            game: game.into(),
            fullscreen,
            config,
            home_key: home_key.to_owned(),
            home_button: home_button.to_owned(),
            user_profile,
            handoff: false,
            _override_file: override_file,
        })
    }

    fn start(&mut self, launch: Launch) -> Result<(), String> {
        let executable = std::env::current_exe().map_err(|error| error.to_string())?
            .with_file_name(format!("eden-cli{}", std::env::consts::EXE_SUFFIX));
        let mut command = Command::new(&executable);
        command.arg("--gpui-session").arg("-g").arg(&launch.game);
        command.env_remove("EDEN_CLI_RESTART_HANDOFF");
        command.env("EGLANTINE_GPUI_HOME_KEY", &launch.home_key);
        command.env("EGLANTINE_GPUI_HOME_BUTTON", &launch.home_button);
        if launch.handoff {
            command.env("EDEN_CLI_RESTART_HANDOFF", "1");
        }
        if launch.fullscreen {
            command.arg("-f");
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
        self.host = Some(Host {
            child, input, launch, state: None, pending: None,
            deadline: Instant::now() + BOOT_TIMEOUT, ready: false, reported_error: false,
        });
        Ok(())
    }

    pub fn pause(&mut self) -> Result<(), String> { self.send("pause", Some(State::Running)) }
    pub fn resume(&mut self) -> Result<(), String> { self.send("resume", Some(State::Paused)) }
    pub fn stop(&mut self) -> Result<(), String> { self.send("stop", None) }

    pub fn restart(&mut self) -> Result<(), String> {
        self.stop()?;
        self.restart = true;
        Ok(())
    }

    fn send(&mut self, command: &str, state: Option<State>) -> Result<(), String> {
        let host = self.host.as_mut().ok_or("No game session is active")?;
        if !host.ready || host.pending.is_some()
            || !matches!(host.state, Some(State::Running | State::Paused))
            || state.is_some_and(|state| Some(state) != host.state)
        {
            return Err("The game session cannot perform that action yet".into());
        }
        self.next_id += 1;
        let id = self.next_id;
        let input = host.child.stdin.as_mut().ok_or("The game host input was closed")?;
        writeln!(input, "{{\"version\":{PROTOCOL},\"id\":{id},\"command\":\"{command}\"}}")
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
            Ok(None) => return events,
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
        assert!(read_event(&mut &b"{\"version\":2,\"event\":\"ready\"}\n"[..]).is_err());
        assert!(read_event(&mut &b"{\"version\":1,\"event\":\"ready\"}"[..]).is_err());
        assert!(read_event(&mut &vec![b' '; MAX_FRAME as usize + 1][..]).is_err());
    }
}
