// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use serde::Deserialize;
use std::ffi::OsStr;
use std::io::{BufRead, BufReader, Read};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

const MAX_FRAME: u64 = 600 * 1024;

#[derive(Deserialize)]
#[serde(tag = "event", rename_all = "snake_case")]
pub(crate) enum Event {
    Ready,
    Error { message: String },
    Game { path: PathBuf, title: String, program_id: Option<String>, icon_hex: Option<String> },
    LibraryDone { scan_errors: usize },
    ContentDone,
}

#[derive(Deserialize)]
struct Envelope {
    version: u32,
    #[serde(flatten)]
    event: Event,
}

pub(crate) fn executable() -> Result<PathBuf, String> {
    std::env::current_exe().map_err(|error| error.to_string()).map(|current| {
        current.with_file_name(format!("eden-cli{}", std::env::consts::EXE_SUFFIX))
    })
}

pub(crate) fn run(args: &[&OsStr], timeout: Duration, mut event: impl FnMut(Event) -> Result<(), String>) -> Result<(), String> {
    let executable = executable()?;
    let mut child = Command::new(&executable).args(args).stdin(Stdio::piped())
        .stdout(Stdio::piped()).stderr(Stdio::null()).spawn()
        .map_err(|error| format!("Could not start {}: {error}", executable.display()))?;
    let stdout = child.stdout.take().expect("piped host stdout");
    let (sender, receiver) = mpsc::sync_channel(2);
    thread::spawn(move || {
        let mut reader = BufReader::new(stdout);
        loop {
            match read_event(&mut reader) {
                Ok(Some(envelope)) => {
                    if sender.send(Ok(envelope)).is_err() {
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
    let deadline = Instant::now() + timeout;
    let result = (|| {
        loop {
            match receiver.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
                Ok(Ok(Event::Error { message })) => return Err(message),
                Ok(Ok(update)) => event(update)?,
                Ok(Err(error)) => return Err(error),
                Err(mpsc::RecvTimeoutError::Timeout) => return Err("The emulator tool timed out".into()),
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
        }
        loop {
            if let Some(status) = child.try_wait().map_err(|error| error.to_string())? {
                return if status.success() { Ok(()) } else { Err(format!("The emulator tool exited with {status}")) };
            }
            if Instant::now() >= deadline {
                return Err("The emulator tool timed out during shutdown".into());
            }
            thread::sleep(Duration::from_millis(10));
        }
    })();
    if result.is_err() {
        drop(child.stdin.take());
        let _ = child.kill();
        let _ = child.wait();
    }
    result
}

fn read_event(reader: &mut impl BufRead) -> Result<Option<Event>, String> {
    let mut frame = Vec::new();
    let size = reader.take(MAX_FRAME + 1).read_until(b'\n', &mut frame)
        .map_err(|error| format!("Could not read emulator tool event: {error}"))?;
    if size == 0 {
        return Ok(None);
    }
    if size as u64 > MAX_FRAME || frame.last() != Some(&b'\n') {
        return Err("The emulator tool sent an incomplete or oversized event".into());
    }
    let parsed: Envelope = serde_json::from_slice(&frame)
        .map_err(|error| format!("Invalid emulator tool event: {error}"))?;
    if parsed.version != 1 {
        return Err(format!("The emulator tool protocol version {} is unsupported", parsed.version));
    }
    Ok(Some(parsed.event))
}
