// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::host_tool::{self, Event};
use std::ffi::OsStr;
use std::path::Path;
use std::time::Duration;

#[derive(Clone, Copy)]
pub enum ContentOperation {
    InstallContent,
    InstallFirmware,
    VerifyContent,
}

pub fn run(operation: ContentOperation, path: Option<&Path>) -> Result<(), String> {
    let (operation, input) = match (operation, path) {
        (ContentOperation::InstallContent, Some(path)) if path.is_file() => ("install_content", Some(path)),
        (ContentOperation::InstallFirmware, Some(path)) if path.is_dir() => ("install_firmware", Some(path)),
        (ContentOperation::VerifyContent, None) => ("verify_content", None),
        _ => return Err("Choose an existing file or folder for this operation".into()),
    };
    let mut args = vec![OsStr::new("--gpui-content"), OsStr::new(operation)];
    if let Some(path) = input {
        args.push(path.as_os_str());
    }
    let mut done = false;
    host_tool::run(&args, Duration::from_secs(60 * 60), |event| match event {
        Event::Ready => Ok(()),
        Event::ContentDone => {
            done = true;
            Ok(())
        }
        _ => Err("The emulator returned an unexpected content event".into()),
    })?;
    if done { Ok(()) } else { Err("The emulator did not complete the content operation".into()) }
}
