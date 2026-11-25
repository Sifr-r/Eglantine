// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::PathBuf;

use eglantine_gpui::{
    content::{self, ContentOperation},
    session::SessionEvent,
};
use gpui::{Context, PathPromptOptions, Window};

use crate::controller::Input;

use super::{Overlay, Screen, Session, Shell, Target};

#[derive(Clone, Copy)]
pub(super) enum Picker {
    Game,
    Folder,
    Content,
    Firmware,
}

impl Shell {
    pub(super) fn poll(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let name = self.controller.name();
        if name != self.controller_name {
            self.controller_name = name;
            cx.notify();
        }
        for event in self.client.poll() {
            match event {
                SessionEvent::Running => {
                    self.session = Session::Running;
                    self.overlay = Overlay::None;
                    if let (Some(library), Some(path)) = (&mut self.library, &self.session_path) {
                        if let Err(error) = library.note_played(path) {
                            self.message = Some(error);
                        } else if let Some(index) = library.entries().iter().position(|entry| &entry.path == path) {
                            self.set_library_selection(index);
                            self.return_target = Target::Game(index);
                        }
                    }
                }
                SessionEvent::Paused => {
                    self.session = Session::Paused;
                    self.overlay = Overlay::Pause;
                    self.target = Target::Resume;
                    window.activate_window();
                    window.focus(&self.focus);
                }
                SessionEvent::HomeRequested => {
                    self.home_released = false;
                    self.request_pause();
                }
                SessionEvent::HomeReleased => self.home_released = true,
                SessionEvent::Stopped => {
                    self.session = Session::Idle;
                    self.home_released = true;
                    self.overlay = Overlay::None;
                    self.target = self.return_target;
                    self.session_path = None;
                    self.scroll_to_game(self.selected);
                    window.activate_window();
                    window.focus(&self.focus);
                }
                SessionEvent::Error(error) => {
                    self.message = Some(error);
                    if self.session == Session::Starting {
                        self.session = Session::Idle;
                    } else if self.session == Session::Pausing {
                        self.session = Session::Running;
                    } else if matches!(self.session, Session::Closing | Session::Resuming) {
                        self.session = Session::Paused;
                        self.overlay = Overlay::Pause;
                        self.target = Target::Resume;
                    }
                }
            }
            cx.notify();
        }
        let home_button = self.settings.as_ref().map_or("guide", |config| config.home_button());
        for input in self.controller.poll(home_button) {
            if window.is_window_active() || matches!(input, Input::Home) {
                self.handle_input(input, window, cx);
            }
        }
    }

    pub(super) fn request_pause(&mut self) {
        if self.session == Session::Running {
            match self.client.pause() {
                Ok(()) => self.session = Session::Pausing,
                Err(error) => {
                    self.home_released = true;
                    self.message = Some(error);
                }
            }
        }
    }

    pub(super) fn pick(&mut self, picker: Picker, window: &Window, cx: &Context<Self>) {
        let folder = matches!(picker, Picker::Folder | Picker::Firmware);
        if self.content_busy && matches!(picker, Picker::Content | Picker::Firmware) {
            return;
        }
        let prompt = match picker {
            Picker::Game => "Open game",
            Picker::Folder => "Add game folder",
            Picker::Content => "Choose content to install",
            Picker::Firmware => "Choose a firmware folder",
        };
        let reply = cx.prompt_for_paths(PathPromptOptions {
            files: !folder,
            directories: folder,
            multiple: false,
            prompt: Some(prompt.into()),
        });
        cx.spawn_in(window, async move |entity, cx| {
            let result = reply.await;
            let _ = cx.update(|window, cx| entity.update(cx, |shell, cx| {
                window.focus(&shell.focus);
                match result {
                    Ok(Ok(Some(paths))) => {
                        if let Some(path) = paths.into_iter().next() {
                            match picker {
                                Picker::Folder => {
                                    if let Some(library) = &mut shell.library {
                                        match library.add_folder(path) {
                                            Ok(()) => shell.refresh_metadata(window, cx),
                                            Err(error) => shell.message = Some(error),
                                        }
                                    }
                                }
                                Picker::Game => {
                                    shell.overlay = Overlay::None;
                                    shell.launch(path);
                                }
                                Picker::Content => shell.run_content(ContentOperation::InstallContent, Some(path), window, cx),
                                Picker::Firmware => shell.run_content(ContentOperation::InstallFirmware, Some(path), window, cx),
                            }
                        }
                    }
                    Ok(Err(error)) => shell.message = Some(error.to_string()),
                    _ => {},
                }
                cx.notify();
            }));
        }).detach();
    }

    pub(super) fn refresh_metadata(&mut self, window: &Window, cx: &Context<Self>) {
        let Some(library) = &self.library else { return };
        self.metadata_request += 1;
        let generation = self.metadata_request;
        let scan = library.metadata_request();
        let task = cx.background_executor().spawn(async move { scan.read() });
        cx.spawn_in(window, async move |entity, cx| {
            let mut scan = task.await;
            let complete = scan.is_complete();
            let warning = scan.warning.take();
            let _ = cx.update(|_, cx| entity.update(cx, |shell, cx| {
                if shell.metadata_request != generation {
                    return;
                }
                let selected = shell.library_selection_path();
                let focused = matches!(shell.target, Target::Game(_));
                if let Some(library) = &mut shell.library {
                    library.apply_metadata(scan);
                }
                let index = selected.and_then(|path| shell.entries().iter().position(|entry| entry.path == path));
                if index.is_some() || complete {
                    shell.pending_library_selection = None;
                }
                shell.selected = index.unwrap_or_default();
                if focused || shell.target == Target::AddFolder && !shell.entries().is_empty() && shell.screen == Screen::Home {
                    shell.target = if shell.entries().is_empty() { Target::AddFolder } else { Target::Game(shell.selected) };
                    shell.scroll_to_game(shell.selected);
                }
                if let Some(warning) = warning {
                    shell.message = Some(warning);
                }
                cx.notify();
            }));
        }).detach();
    }

    pub(super) fn run_content(&mut self, operation: ContentOperation, path: Option<PathBuf>, window: &Window, cx: &Context<Self>) {
        if self.content_busy || self.session != Session::Idle {
            return;
        }
        self.content_busy = true;
        self.message = Some("Working on installed content…".into());
        let task = cx.background_executor().spawn(async move { content::run(operation, path.as_deref()) });
        cx.spawn_in(window, async move |entity, cx| {
            let result = task.await;
            let _ = cx.update(|_, cx| entity.update(cx, |shell, cx| {
                shell.content_busy = false;
                shell.message = Some(match result {
                    Ok(()) => "Content operation completed.".into(),
                    Err(error) => error,
                });
                cx.notify();
            }));
        }).detach();
    }

    pub(super) fn launch(&mut self, path: PathBuf) {
        if self.session != Session::Idle || self.content_busy {
            return;
        }
        let result = match &self.settings {
            Some(config) => self.client.launch_game_with_profile(&path, config, self.user_profile_override),
            None => self.client.launch(&path, false, None),
        };
        match result {
            Ok(()) => {
                self.return_target = self.target;
                self.session_path = Some(path);
                self.session = Session::Starting;
            }
            Err(error) => self.message = Some(error),
        }
    }
}
