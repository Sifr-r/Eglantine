// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod appearance;
mod controller;
mod navigation;
mod preferences;
mod shell;
mod views;

use std::ffi::OsString;
use std::path::PathBuf;

use gpui::{
    App, AppContext, Application, Bounds, TitlebarOptions, WindowBounds, WindowOptions, px, size,
};

#[derive(Default, PartialEq, Eq, Debug)]
struct LaunchOptions {
    safe_theme: bool,
    fullscreen: Option<bool>,
    game: Option<PathBuf>,
    user_profile: Option<u8>,
    warning: Option<String>,
}

impl LaunchOptions {
    fn parse(args: impl IntoIterator<Item = OsString>) -> Self {
        let mut options = Self::default();
        let mut iter = args.into_iter().skip(1);
        while let Some(arg) = iter.next() {
            let text = arg.to_string_lossy();
            match text.as_ref() {
                "--safe-theme" => options.safe_theme = true,
                "-f" | "--fullscreen" => options.fullscreen = Some(true),
                "-g" | "--game" => match iter.next() {
                    Some(path) if !path.is_empty() => options.game = Some(PathBuf::from(path)),
                    _ => options.warning = Some("Missing game path after -g/--game.".into()),
                },
                "-u" | "--user" => match iter.next().and_then(|v| v.to_string_lossy().parse::<u8>().ok()) {
                    Some(index) if index < 8 => options.user_profile = Some(index),
                    _ => options.warning = Some("User profile index must be between 0 and 7.".into()),
                },
                _ => {
                    if let Some(value) = text.strip_prefix("--game=") && !value.is_empty() {
                        options.game = Some(PathBuf::from(value));
                    } else if let Some(value) = text.strip_prefix("--user=") {
                        if let Ok(index) = value.parse::<u8>() && index < 8 {
                            options.user_profile = Some(index);
                        } else {
                            options.warning = Some("User profile index must be between 0 and 7.".into());
                        }
                    } else {
                        options.warning.get_or_insert_with(|| format!("Ignored unknown option: {text}"));
                    }
                }
            }
        }
        options
    }
}

fn main() {
    let options = LaunchOptions::parse(std::env::args_os());
    Application::new().run(move |app: &mut App| {
        let dir = eglantine_gpui::library::data_dir();
        let (preferences, preference_error) = match preferences::Preferences::load(&dir) {
            Ok(preferences) => (preferences, None),
            Err(error) => (preferences::Preferences::default(), Some(error)),
        };
        let initial_message = preference_error.or(options.warning.clone());
        let bounds = Bounds::centered(
            None, size(px(preferences.width.into()), px(preferences.height.into())), app,
        );
        let fullscreen = options.fullscreen.unwrap_or(preferences.fullscreen);
        let bounds = if fullscreen {
            WindowBounds::Fullscreen(bounds)
        } else {
            WindowBounds::Windowed(bounds)
        };
        let safe_theme = options.safe_theme;
        let initial_game = options.game.clone();
        let user_profile = options.user_profile;
        let result = app.open_window(
            WindowOptions {
                window_bounds: Some(bounds),
                window_min_size: Some(size(px(630.0), px(450.0))),
                titlebar: Some(TitlebarOptions {
                    title: Some("Eglantine".into()),
                    ..Default::default()
                }),
                ..Default::default()
            },
            |window, app| app.new(|cx| {
                shell::Shell::new(
                    window, cx, dir, preferences, initial_message, safe_theme, initial_game, user_profile,
                )
            }),
        );
        match result {
            Ok(_) => app.activate(true),
            Err(error) => {
                eprintln!("Couldn't open Eglantine: {error:#}");
                app.quit();
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_launch_options_and_ignores_unknown_flags() {
        let opts = LaunchOptions::parse([
            OsString::from("eglantine-gpui"),
            OsString::from("--safe-theme"),
            OsString::from("-f"),
            OsString::from("-u"),
            OsString::from("3"),
            OsString::from("-g"),
            OsString::from("Games/Demo.nsp"),
        ]);
        assert!(opts.safe_theme);
        assert_eq!(opts.fullscreen, Some(true));
        assert_eq!(opts.user_profile, Some(3));
        assert_eq!(opts.game, Some(PathBuf::from("Games/Demo.nsp")));
        assert!(opts.warning.is_none());

        let bad = LaunchOptions::parse([
            OsString::from("eglantine-gpui"),
            OsString::from("-u"),
            OsString::from("99"),
            OsString::from("--unknown"),
        ]);
        assert!(bad.user_profile.is_none());
        assert!(bad.warning.is_some());
    }
}
