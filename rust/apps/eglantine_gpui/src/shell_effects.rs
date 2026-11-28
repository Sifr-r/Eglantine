// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

use eglantine_gpui::{
    content::{self, ContentOperation},
    emulator_config::EmulatorSetting,
    session::{EmbedTarget, SessionEvent},
    software_keyboard::SoftwareKeyboardState,
};
use gpui::{Context, PathPromptOptions, Window};

use crate::controller::Input;

use super::{Category, Overlay, PauseDestination, Screen, Session, Shell, Target};

#[derive(Clone, Copy)]
pub(super) enum Picker {
    Game,
    Folder,
    Content,
    Firmware,
    Bios,
}

impl Shell {
    pub(super) fn poll(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let name = self.controller.name();
        if name != self.controller_name {
            self.controller_name = name;
            cx.notify();
        }
        if self.session != Session::Idle {
            if self.parent_hwnd.is_none() {
                self.parent_hwnd = crate::embed::current_window_hwnd();
            }
            if let Some(hwnd) = self.parent_hwnd
                && let Some(rect) = crate::embed::viewport_rect(
                    hwnd,
                    crate::embed::GAME_TOP_BAR_HEIGHT,
                    window.scale_factor(),
                )
            {
                self.client.sync_viewport(rect);
            }
        }
        if self.ble_handoff_deadline.is_some_and(|deadline| Instant::now() >= deadline) {
            self.ble_handoff_deadline = None;
            self.controller.suspend_native();
        }
        for event in self.client.poll() {
            match event {
                SessionEvent::Running => {
                    self.session = Session::Running;
                    self.overlay = Overlay::None;
                    self.gba_fast_forward = false;
                    self.pending_gba_action = None;
                    if self.active_ble_handoff.is_some() && self.controller.is_native_active() {
                        self.ble_handoff_deadline = Some(Instant::now() + Duration::from_secs(4));
                    }
                    let selected = self.library_selection_path();
                    if let (Some(library), Some(path)) = (&mut self.library, self.session_path.clone()) {
                        if let Err(error) = library.note_played(&path) {
                            self.message = Some(error);
                        } else {
                            let path_console = self.entries().iter().find(|entry| entry.path == path).map(|entry| entry.console);
                            let preferred = if self.console.is_none() || path_console == Some(self.console) {
                                Some(path.as_path())
                            } else { selected.as_deref() };
                            self.rebuild_library(preferred);
                            if self.library_indices.contains(&self.selected) {
                                self.return_target = Target::Game(self.selected);
                            }
                        }
                    }
                }
                SessionEvent::Paused => {
                    self.session = Session::Paused;
                    match std::mem::replace(&mut self.pause_destination, PauseDestination::PauseMenu) {
                        PauseDestination::PauseMenu => {
                            self.overlay = Overlay::Pause;
                            self.target = Target::Resume;
                        }
                        PauseDestination::EmulationSettings => {
                            self.open_settings(Category::Emulation);
                        }
                        PauseDestination::ConfirmStop => {
                            self.overlay = Overlay::Confirm(Target::Stop);
                            self.target = Target::Back;
                        }
                    }
                    window.activate_window();
                    window.focus(&self.focus);
                }
                SessionEvent::HomeRequested => {
                    self.home_released = false;
                    self.request_pause();
                }
                SessionEvent::HomeReleased => self.home_released = true,
                SessionEvent::ControllerClaimed => {
                    self.ble_handoff_deadline = None;
                    self.controller.suspend_native();
                }
                SessionEvent::Stopped => {
                    self.session = Session::Idle;
                    self.home_released = true;
                    self.gba_fast_forward = false;
                    self.pending_gba_action = None;
                    self.pause_destination = PauseDestination::PauseMenu;
                    self.swkbd = None;
                    self.overlay = Overlay::None;
                    self.target = self.return_target;
                    self.session_path = None;
                    let handoff = self.active_ble_handoff.take();
                    self.ble_handoff_deadline = None;
                    self.controller.set_native_muted(false);
                    self.controller.resume_native_with_handoff(handoff.as_deref());
                    self.scroll_to_game(self.selected);
                    window.activate_window();
                    window.focus(&self.focus);
                }
                SessionEvent::SoftwareKeyboardShow(request) => {
                    self.swkbd = Some(SoftwareKeyboardState::from_request(request));
                    self.overlay = Overlay::SoftwareKeyboard;
                    self.target = Target::SwkbdSubmit;
                    window.activate_window();
                    window.focus(&self.focus);
                }
                SessionEvent::SoftwareKeyboardTextCheck { request_id, result, message } => {
                    if let Some(swkbd) = &mut self.swkbd
                        && swkbd.apply_text_check(request_id, result, message)
                    {
                        self.target = Target::SwkbdSubmit;
                        window.activate_window();
                        window.focus(&self.focus);
                    }
                }
                SessionEvent::SoftwareKeyboardClose { request_id } => {
                    if self.swkbd.as_ref().is_some_and(|swkbd| swkbd.request_id == request_id) {
                        self.swkbd = None;
                        if self.overlay == Overlay::SoftwareKeyboard {
                            self.overlay = Overlay::None;
                        }
                    }
                }
                SessionEvent::ActionCompleted => {
                    if let Some(action) = self.pending_gba_action.take() {
                        self.commit_gba_action(action);
                    }
                }
                SessionEvent::Error(error) => {
                    self.pending_gba_action = None;
                    self.pause_destination = PauseDestination::PauseMenu;
                    if self.overlay == Overlay::SoftwareKeyboard
                        && let Some(swkbd) = &mut self.swkbd
                    {
                        swkbd.clear_submitting_on_error(error.clone());
                    }
                    self.message = Some(error);
                    if self.session == Session::Starting {
                        self.session = Session::Idle;
                        let handoff = self.active_ble_handoff.take();
                        self.ble_handoff_deadline = None;
                        self.controller.set_native_muted(false);
                        self.controller.resume_native_with_handoff(handoff.as_deref());
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
            Picker::Bios => "Choose GBA BIOS (16 KiB)",
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
                                            Ok(()) => shell.refresh_metadata(false, window, cx),
                                            Err(error) => shell.message = Some(error),
                                        }
                                    }
                                }
                                Picker::Game => {
                                    shell.overlay = Overlay::None;
                                    shell.launch(path, window);
                                }
                                Picker::Content => shell.run_content(ContentOperation::InstallContent, Some(path), window, cx),
                                Picker::Firmware => shell.run_content(ContentOperation::InstallFirmware, Some(path), window, cx),
                                Picker::Bios => {
                                    shell.gba_bios_path = Some(path.to_string_lossy().into_owned());
                                    shell.message = Some("Selected custom GBA BIOS.".into());
                                }
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

    pub(super) fn refresh_metadata(&mut self, force_refresh: bool, window: &Window, cx: &mut Context<Self>) {
        let Some(library) = &self.library else { return };
        self.metadata_request += 1;
        let generation = self.metadata_request;
        self.metadata_busy = true;
        cx.notify();
        let scan = library.metadata_request(self.auto_extract_metadata, force_refresh);
        let task = cx.background_executor().spawn(async move { scan.read() });
        cx.spawn_in(window, async move |entity, cx| {
            let mut scan = task.await;
            let complete = scan.is_complete();
            let warning = scan.warning.take();
            let _ = cx.update(|_, cx| entity.update(cx, |shell, cx| {
                if shell.metadata_request != generation {
                    return;
                }
                shell.metadata_busy = false;
                let selected = shell.library_selection_path();
                let focused = matches!(shell.target, Target::Game(_));
                if let Some(library) = &mut shell.library {
                    library.apply_metadata(scan);
                }
                shell.rebuild_library(selected.as_deref());
                let retained = selected.as_ref().is_some_and(|path| shell.entries().get(shell.selected)
                    .is_some_and(|entry| &entry.path == path));
                if retained || complete {
                    shell.pending_library_selection = None;
                }
                if matches!(shell.return_target, Target::Game(_)) {
                    shell.return_target = if shell.library_indices.is_empty() {
                        Target::AddFolder
                    } else {
                        Target::Game(shell.selected)
                    };
                }
                if focused || shell.target == Target::AddFolder && !shell.library_indices.is_empty() && shell.screen == Screen::Home {
                    shell.target = if shell.library_indices.is_empty() { Target::AddFolder } else { Target::Game(shell.selected) };
                    shell.scroll_to_game(shell.selected);
                } else if retained && shell.target == Target::AddFolder && shell.screen == Screen::Library {
                    shell.target = Target::Game(shell.selected);
                    shell.scroll_to_game(shell.selected);
                }
                if let Some(warning) = warning {
                    shell.message = Some(warning);
                } else if force_refresh {
                    shell.message = Some("Metadata updated.".into());
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

    pub(super) fn launch(&mut self, path: PathBuf, window: &mut Window) {
        if self.session != Session::Idle || self.content_busy {
            return;
        }
        if self.parent_hwnd.is_none() {
            self.parent_hwnd = crate::embed::current_window_hwnd();
        }
        let want_fullscreen = self.settings.as_ref().is_some_and(|config| {
            config
                .game_value(&path, EmulatorSetting::GameDisplay)
                .ok()
                .flatten()
                .map_or_else(|| config.game_fullscreen(), |value| value == "Fullscreen")
        });
        if self.parent_hwnd.is_some() && want_fullscreen && !window.is_fullscreen() {
            window.toggle_fullscreen();
        }
        let embed = self.parent_hwnd.and_then(|hwnd| {
            crate::embed::viewport_rect(
                hwnd,
                crate::embed::GAME_TOP_BAR_HEIGHT,
                window.scale_factor(),
            )
            .map(|viewport| EmbedTarget { parent_window: hwnd, viewport })
        });
        let gba = self.gba_launch_config();
        let ble_handoff = self.controller.ble_handoff();
        if ble_handoff.is_some() {
            self.controller.set_native_muted(true);
            self.client.set_next_ble_handoff(ble_handoff.clone());
        } else {
            self.controller.suspend_native();
            self.client.set_next_ble_handoff(None);
        }
        let result = match &self.settings {
            Some(config) => self.client.launch_game_with_embed(
                &path,
                config,
                self.user_profile_override,
                gba,
                embed,
            ),
            None => self.client.launch_with_embed(&path, false, None, gba, embed),
        };
        match result {
            Ok(()) => {
                self.return_target = self.target;
                self.session_path = Some(path);
                self.session = Session::Starting;
                self.pause_destination = PauseDestination::PauseMenu;
                self.active_ble_handoff = ble_handoff;
                self.ble_handoff_deadline = None;
            }
            Err(error) => {
                self.active_ble_handoff = None;
                self.ble_handoff_deadline = None;
                self.controller.set_native_muted(false);
                self.controller.resume_native_with_handoff(ble_handoff.as_deref());
                self.message = Some(error);
            }
        }
    }
}
