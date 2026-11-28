// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::time::Duration;

use eglantine_gpui::{
    software_keyboard::TextCheckFeedback,
    theme::{ComponentId, HomeMode, LibraryMode, PanelSide, Placement, ThemeComponent, ThemeStyle},
};
use gpui::{
    Animation, AnimationExt, Background, Context, Div, FontWeight, Hsla, MouseButton, ObjectFit, SharedString,
    Window, div, img, linear_color_stop, linear_gradient, prelude::*, px, relative, rgb,
};

use crate::{
    appearance::{PreviewScreen, ThemeAction},
    shell::{Category, Overlay, Screen, Session, Shell, Target},
};

#[path = "console_views.rs"]
mod console_views;

impl Render for Shell {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let width = f32::from(window.viewport_size().width);
        self.compact_rail = width < 760.0;
        let height = f32::from(window.viewport_size().height);
        let style = &self.appearance.package().theme.style;
        let shell_style = self.console_component(ThemeComponent::Shell);
        let bg = shell_style.background.unwrap_or(style.background);
        let fg = shell_style.foreground.unwrap_or(style.foreground);
        let tile = f32::from(style.cover_size).min(height * 0.53).max(100.0);
        let layout = &self.appearance.package().theme.layout;
        let side_width = if layout.home_navigation == Placement::Below { 0.0 } else { 170.0 };
        let grid_width = if self.screen == Screen::Home { width - side_width - 64.0 } else { width - 64.0 };
        let family_tile = match self.console {
            Some(eglantine_library::ConsoleId::Ds) => Some(168.0),
            Some(eglantine_library::ConsoleId::Nintendo3Ds) => Some(143.0),
            _ => None,
        };
        self.grid_columns = (grid_width / (family_tile.unwrap_or(tile) + f32::from(style.spacing))).floor().max(1.0) as usize;

        let active_session = self.session != Session::Idle;
        let content = if active_session {
            self.gameplay_view(window, cx).into_any_element()
        } else {
            match self.screen {
                Screen::Home => self.home(window, cx, tile).into_any_element(),
                Screen::Library if self.console_layout().is_some() => self.console_library(window, cx).into_any_element(),
                Screen::Library => self.library(cx, tile.min(210.0)).into_any_element(),
            }
        };
        let background = style.background_asset.as_ref().and_then(|id| self.appearance.image(id));
        let text_scale = (f32::from(style.text_scale) / 100.0) * crate::controller::os_text_scale();
        let mut root = div()
            .id("eglantine-shell")
            .size_full()
            .relative()
            .overflow_hidden()
            .bg(rgb(bg.0))
            .text_color(rgb(fg.0))
            .font_family(style.font_family.clone())
            .text_size(px(16.0 * text_scale))
            .track_focus(&self.focus)
            .on_key_down(cx.listener(Self::on_key));
        if let Some(background) = background {
            root = root.child(img(background).absolute().size_full().top_0().left_0().object_fit(ObjectFit::Cover))
                .child(div().absolute().size_full().top_0().left_0().bg(Hsla::from(rgb(bg.0)).opacity(0.72)));
        }
        let mut body = column().relative().size_full().child(content);
        if !active_session {
            body = body.child(self.footer());
        }
        root = root.child(body);
        if self.overlay != Overlay::None {
            let overlay = self.overlay(window, cx);
            let motion = if crate::controller::reduced_motion() { 0 } else { style.motion_ms };
            root = if motion == 0 {
                root.child(overlay)
            } else {
                root.child(overlay.with_animation(
                    "overlay-appearance",
                    Animation::new(Duration::from_millis(motion.into())),
                    |view, progress| view.opacity(0.65 + 0.35 * progress),
                ))
            };
        }
        root
    }
}

impl Shell {
    fn session_title(&self) -> String {
        self.session_path
            .as_ref()
            .and_then(|path| {
                self.entries()
                    .iter()
                    .find(|entry| &entry.path == path)
                    .map(|entry| entry.title.clone())
                    .or_else(|| path.file_stem().map(|name| name.to_string_lossy().into_owned()))
            })
            .unwrap_or_else(|| "Game".to_owned())
    }

    fn gameplay_view(&self, window: &Window, cx: &Context<Self>) -> impl IntoElement {
        let style = &self.appearance.package().theme.style;
        let title = self.session_title();
        let home_key = self.settings.as_ref().map_or("F10", |config| config.home_key());
        let status: SharedString = match self.session {
            Session::Starting => "Starting…".into(),
            Session::Running => format!("Running · {home_key} to pause").into(),
            Session::Pausing => "Pausing…".into(),
            Session::Paused => "Paused".into(),
            Session::Resuming => "Resuming…".into(),
            Session::Closing => "Closing…".into(),
            Session::Idle => "".into(),
        };
        let pause_or_resume = if self.session == Session::Paused {
            self.button(Target::Resume, "Resume", cx)
        } else {
            self.button(Target::PauseGame, "Pause", cx)
        };
        let bar = row()
            .w_full()
            .h(px(crate::embed::GAME_TOP_BAR_HEIGHT))
            .flex_none()
            .items_center()
            .justify_between()
            .gap_3()
            .px_4()
            .bg(rgb(style.panel.0))
            .border_b_1()
            .border_color(Hsla::from(rgb(style.muted.0)).opacity(0.25))
            .child(
                row()
                    .min_w_0()
                    .items_center()
                    .gap_3()
                    .child(div().font_weight(FontWeight::SEMIBOLD).truncate().child(title.clone()))
                    .child(div().text_sm().text_color(rgb(style.muted.0)).child(status)),
            )
            .child(
                row()
                    .flex_none()
                    .items_center()
                    .gap_2()
                    .child(self.button(Target::EmulationSettings, "Emulation settings", cx))
                    .child(pause_or_resume)
                    .child(self.button(
                        Target::Fullscreen,
                        if window.is_fullscreen() { "Exit fullscreen" } else { "Fullscreen" },
                        cx,
                    ))
                    .child(self.button(Target::Stop, "Close Game", cx)),
            );
        let viewport = div()
            .flex_1()
            .w_full()
            .min_h_0()
            .bg(gpui::black())
            .flex()
            .items_center()
            .justify_center()
            .when(self.session == Session::Starting, |view| {
                view.child(
                    div()
                        .text_color(rgb(style.muted.0))
                        .child(format!("Starting {title}…")),
                )
            });
        column().flex_1().size_full().min_h_0().child(bar).child(viewport)
    }

    fn home(&self, window: &Window, cx: &Context<Self>, tile: f32) -> impl IntoElement {
        let theme = &self.appearance.package().theme;
        let gap = px(theme.style.spacing.into());
        let game = self.library_indices.contains(&self.selected).then(|| &self.entries()[self.selected]);
        let header = row().w_full().justify_between().items_center().gap_4()
            .child(div().text_color(rgb(theme.style.muted.0)).child("Eglantine"))
            .child(row().items_center().gap_3()
                .child(div().text_color(rgb(theme.style.muted.0)).child(
                    self.controller_name.as_ref().map_or_else(|| "Keyboard and mouse".to_owned(), |_| "Controller connected".to_owned())
                ))
                .child(self.button(
                    Target::Fullscreen,
                    if window.is_fullscreen() { "Exit fullscreen" } else { "Fullscreen" },
                    cx,
                )));
        let title = game.map_or("", |entry| entry.title.as_str()).to_owned();
        let subtitle = game.and_then(|entry| entry.info.summary_line(entry.console));
        let game_shelf = if self.library_indices.is_empty() {
            column().gap_4().justify_center().h(px(tile))
                .child(div().text_xl().child("Add a game folder"))
                .child(div().text_color(rgb(theme.style.muted.0)).child("Games from that folder will appear here."))
                .child(row().child(self.button(Target::AddFolder, "Add folder", cx)))
                .into_any_element()
        } else if theme.layout.home_mode == HomeMode::Grid {
            let entries = self.visible_entries();
            column().id("home-grid").max_h(px(tile * 1.55)).overflow_y_scroll()
                .track_scroll(&self.shelf_scroll).gap(gap)
                .children(entries.chunks(self.grid_columns).map(|indices| {
                    row().gap(gap).children(indices.iter().map(|index| self.cover(*index, tile, cx)))
                })).into_any_element()
        } else {
            row().id("home-shelf").gap(gap).w_full().min_w_0().overflow_x_scroll().track_scroll(&self.shelf_scroll)
                .children(self.library_indices.iter().map(|&index| self.cover(index, tile, cx)))
                .into_any_element()
        };
        let shortcuts = theme.layout.home_shortcuts.iter().copied().map(|component| {
            let icon = theme.style.icons.get(&component).and_then(|id| self.appearance.image(id));
            let target = Target::Shortcut(component);
            let title = shortcut_label(component);
            let mut button = self.button_background(target, ("home-shortcut", component as usize));
            if let Some(icon) = icon {
                button = button.child(img(icon).size(px(20.0)).object_fit(ObjectFit::Contain));
            } else {
                #[cfg(windows)]
                {
                    button = button.child(div().font_family("Segoe MDL2 Assets")
                        .text_size(px(18.0)).child(shortcut_glyph(component)));
                }
            }
            button.child(title).on_click(cx.listener(move |this, _, window, cx| this.activate(target, window, cx)))
        }).chain(self.hidden_shortcuts().next().map(|_| self.button(Target::More, "More", cx)));
        let games = column().flex_1().min_w_0().gap_3()
            .child(
                column().gap_1().min_w_0()
                    .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).min_h(px(36.0)).truncate().child(title))
                    .child(
                        div().text_sm().text_color(rgb(theme.style.muted.0)).min_h(px(20.0)).truncate()
                            .child(subtitle.unwrap_or_default()),
                    ),
            )
            .child(game_shelf);
        let games_and_shortcuts = match theme.layout.home_navigation {
            Placement::Below => column().gap(px(32.0)).child(games)
                .child(row().items_center().flex_wrap().gap(gap).children(shortcuts)),
            Placement::Left => row().w_full().gap_4()
                .child(column().w(px(160.0)).gap(gap).children(shortcuts)).child(games),
            Placement::Right => row().w_full().gap_4()
                .child(games).child(column().w(px(160.0)).gap(gap).children(shortcuts)),
        };
        column().flex_1().min_h_0().px(px(32.0)).pt_4().justify_between()
            .child(column().gap_3().child(header).child(self.console_rail(cx)))
            .child(games_and_shortcuts).child(div().h_2())
    }

    fn cover(&self, index: usize, tile: f32, cx: &Context<Self>) -> impl IntoElement {
        let entry = &self.entries()[index];
        let style = &self.appearance.package().theme.style;
        let component = self.console_component(ThemeComponent::LibraryTile);
        let target = Target::Game(index);
        let home = self.screen == Screen::Home;
        let selected = self.target == target || (self.overlay != Overlay::None && self.selected == index);
        let mut cover = div().id(("cover", index)).relative().size(px(tile)).flex_none()
            .rounded(px(component.radius.unwrap_or(style.corner_radius).into())).overflow_hidden().cursor_pointer()
            .bg(rgb(component.background.unwrap_or(style.panel).0))
            .on_click(cx.listener(move |shell, event: &gpui::ClickEvent, window, cx| {
                shell.select_game(index, home || event.click_count() >= 2, false, window, cx)
            }))
            .on_mouse_down(MouseButton::Right, cx.listener(move |shell, _, window, cx| {
                shell.select_game(index, false, true, window, cx)
            }));
        if let Some(path) = &entry.cover {
            cover = cover.child(img(path.clone()).absolute().size_full().left_0().top_0().object_fit(ObjectFit::Cover));
        }
        if selected {
            let selection: Background = if let Some(color) = component.selected_background {
                Hsla::from(rgb(color.0)).opacity(if entry.cover.is_some() { 0.72 } else { 1.0 }).into()
            } else {
                linear_gradient(
                    180.0,
                    linear_color_stop(Hsla::from(rgb(style.focus_start.0)).opacity(if entry.cover.is_some() { 0.0 } else { 1.0 }), 0.18),
                    linear_color_stop(Hsla::from(rgb(style.focus_end.0)).opacity(if entry.cover.is_some() { 0.82 } else { 1.0 }), 1.0),
                )
            };
            cover = cover.child(div().absolute().size_full().left_0().top_0().bg(selection))
                .when_some(component.focus, |cover, focus| cover.border_2().border_color(rgb(focus.0)));
        }
        if entry.cover.is_none() {
            let foreground = component.foreground.unwrap_or(if selected { style.focus_text } else { style.foreground });
            cover = cover.child(column().relative().size_full().justify_end().p(px(component.padding.unwrap_or(16).into())).overflow_hidden()
                .text_size(px(22.0)).font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(foreground.0))
                .child(entry.title.clone()));
        }
        cover
    }

    fn library(&self, cx: &Context<Self>, tile: f32) -> impl IntoElement {
        let theme = &self.appearance.package().theme;
        let component = self.console_component(ThemeComponent::LibraryList);
        let indices = self.visible_entries();
        let search = if self.search.is_empty() && !self.searching {
            "Search games".into()
        } else {
            format!("{}{}", self.search, if self.searching { "▏" } else { "" })
        };
        let tools = row().flex_wrap().gap_3().items_center()
            .child(self.console_button(Target::Back, "Home", ThemeComponent::Toolbar, false, cx))
            .child(self.console_button(Target::Search, search, ThemeComponent::Toolbar, self.searching, cx))
            .children([(Target::AddFolder, "Add folder"), (Target::OpenGame, "Open game"), (Target::Refresh, "Rescan")]
                .into_iter().map(|(target, label)| self.console_button(target, label, ThemeComponent::Toolbar, false, cx)));
        let list = column().id("library-scroll").flex_1().min_h_0().overflow_y_scroll().track_scroll(&self.shelf_scroll);
        let list = if indices.is_empty() {
            list.child(div().p_4().text_color(rgb(theme.style.muted.0))
                .child(if self.search.is_empty() { "Your library is empty." } else { "No games match your search." }))
        } else if theme.layout.library_mode == LibraryMode::Grid {
            list.gap_3().children(indices.chunks(self.grid_columns).map(|row_indices|
                row().gap_3().children(row_indices.iter().map(|index| self.cover(*index, tile, cx)))
            ))
        } else {
            list.gap_2().children(indices.into_iter().map(|index| {
                let game = &self.entries()[index];
                let format = game.info.format.clone().unwrap_or_else(|| {
                    game.path.extension().and_then(|extension| extension.to_str()).unwrap_or_default().to_uppercase()
                });
                let subtitle = game.info.summary_line(game.console);
                let selected = self.target == Target::Game(index);
                let fill = if selected { component.selected_background.or(component.background) } else { component.background };
                self.button_background(Target::Game(index), ("library-game", index)).w_full()
                    .when_some(fill, |button, color| button.bg(rgb(color.0)))
                    .when_some(component.foreground, |button, color| button.text_color(rgb(color.0)))
                    .when_some(component.padding, |button, padding| button.p(px(padding.into())))
                    .when_some(component.radius, |button, radius| button.rounded(px(radius.into())))
                    .when_some(component.focus.filter(|_| selected), |button, focus| button.border_1().border_color(rgb(focus.0)))
                    .child(
                        column().min_w_0().flex_1()
                            .child(div().truncate().font_weight(FontWeight::SEMIBOLD).child(game.title.clone()))
                            .when_some(subtitle, |col, summary| {
                                col.child(div().text_xs().text_color(rgb(theme.style.muted.0)).truncate().child(summary))
                            }),
                    )
                    .child(div().ml_auto().flex_none().text_sm().text_color(rgb(theme.style.muted.0)).child(format))
                    .on_click(cx.listener(move |shell, event: &gpui::ClickEvent, window, cx| {
                        shell.select_game(index, event.click_count() >= 2, false, window, cx)
                    }))
                    .on_mouse_down(MouseButton::Right, cx.listener(move |shell, _, window, cx| {
                        shell.select_game(index, false, true, window, cx)
                    }))
            }))
        };
        column().flex_1().min_h_0().p_6().gap_4()
            .child(self.console_rail(cx))
            .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child(self.console.map_or("Library", |console| console.name())))
            .child(tools)
            .child(list)
            .when(!self.visible_entries().is_empty(), |view| view.child(
                row().gap_3().child(self.console_button(Target::PlaySelected, "Play", ThemeComponent::Play, false, cx))
                    .child(self.button(Target::GameOptions, "Game settings", cx))
                    .child(self.button(Target::GameDetails, "Game details", cx))
            ))
    }

    fn button(&self, target: Target, label: impl Into<SharedString>, cx: &Context<Self>) -> gpui::Stateful<Div> {
        self.scoped_button("main", target, label, cx)
    }

    pub(super) fn overlay_button(
        &self, target: Target, label: impl Into<SharedString>, cx: &Context<Self>,
    ) -> gpui::Stateful<Div> {
        self.scoped_button("overlay", target, label, cx)
    }

    pub(super) fn scoped_button(
        &self, scope: &'static str, target: Target, label: impl Into<SharedString>, cx: &Context<Self>,
    ) -> gpui::Stateful<Div> {
        let label = label.into();
        let id = SharedString::from(format!("btn-{scope}-{target:?}"));
        self.button_surface(target, id, label)
            .on_click(cx.listener(move |this, _, window, cx| this.activate(target, window, cx)))
    }

    fn button_surface(
        &self, target: Target, id: impl Into<gpui::ElementId>, label: impl Into<SharedString>,
    ) -> gpui::Stateful<Div> {
        self.button_background(target, id).child(label.into())
    }

    fn button_background(&self, target: Target, id: impl Into<gpui::ElementId>) -> gpui::Stateful<Div> {
        let style = &self.appearance.package().theme.style;
        let selected = self.target == target;
        let surface: Background = if selected { highlight(style) } else { rgb(style.panel.0).into() };
        row().id(id).items_center().gap_2().min_h(px(40.0)).px_3().py_2().cursor_pointer()
            .rounded(px(style.corner_radius.into()))
            .bg(surface)
            .text_color(rgb(if selected { style.focus_text.0 } else { style.foreground.0 }))
            .hover(|button| button.opacity(0.89))
    }

    fn footer(&self) -> impl IntoElement {
        let style = &self.appearance.package().theme.style;
        let foreground = self.console_component(ThemeComponent::Shell).foreground;
        let home_key = self.settings.as_ref().map_or("F10", |config| config.home_key());
        let status: SharedString = match self.session {
            Session::Starting => "Starting game…".into(),
            Session::Running => format!("Game running · {home_key} to pause").into(),
            Session::Pausing => "Pausing…".into(),
            Session::Paused => "Game paused".into(),
            Session::Resuming => "Resuming…".into(),
            Session::Closing => "Closing game…".into(),
            Session::Idle if self.searching => "Type to search · Enter to finish".into(),
            Session::Idle if !self.native_playback() && self.controller_name.is_none() => "Enter  Details     PgUp / PgDn  Console     Esc  Back".into(),
            Session::Idle if !self.native_playback() => "A  Details     LB / RB  Console     B  Back".into(),
            Session::Idle if self.controller_name.is_none() => "Enter  Open     +  Game options     Esc  Back".into(),
            Session::Idle => "A  Open     +  Game options     B  Back".into(),
        };
        column().px(px(32.0)).py_4().gap_2()
            .when_some(self.message.clone(), |footer, message| {
                footer.child(div().text_color(rgb(foreground.unwrap_or(style.foreground).0)).child(message))
            })
            .child(div().text_sm().text_color(rgb(foreground.unwrap_or(style.muted).0)).child(status))
    }

    fn overlay(&self, window: &Window, cx: &Context<Self>) -> Div {
        let theme = &self.appearance.package().theme;
        let styles = &theme.style;
        let background = rgb(styles.background.0);
        let surface = rgb(styles.panel.0);
        let width = f32::from(window.viewport_size().width);
        let position = match self.overlay {
            Overlay::Settings => theme.layout.settings_side,
            Overlay::Pause => theme.layout.pause_side,
            _ => PanelSide::Right,
        };
        let justification = match position { PanelSide::Left => false, PanelSide::Right => true };
        let make_panel = |width| column().w(px(width)).max_w_full().h_full().p_6().gap_4()
            .bg(background).text_color(rgb(styles.foreground.0));
        let view = match self.overlay {
            Overlay::Settings => {
                let main = make_panel((width * 0.58).max(576.0));
                let title = self.game_options.as_ref().and_then(|path|
                    self.entries().iter().find(|entry| &entry.path == path)
                    .map(|entry| format!("Game settings · {}", entry.title))
                ).unwrap_or_else(|| "Emulator settings".to_owned());
                main.child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child(title))
                    .child(self.settings_panel(cx))
                    .when_some(self.message.clone(), |panel, message| panel.child(div().text_sm().child(message)))
                    .child(row().w_full().justify_between().items_center()
                        .child(div().text_sm().text_color(rgb(styles.muted.0)).child(match (self.game_options.is_some(), self.controller_name.is_some()) {
                            (true, true) => "A  Change     +  Use global     B  Back",
                            (true, false) => "Enter  Change     +  Use global     Esc  Back",
                            (false, true) => "A  Change     B  Back",
                            (false, false) => "Enter  Change     Esc  Back",
                        }))
                        .child(self.overlay_button(Target::Back, "Back", cx)))
            }
            Overlay::Pause => {
                let title = self.session_path.as_ref().and_then(|path|
                    self.entries().iter().find(|entry| &entry.path == path).map(|entry| entry.title.clone())
                    .or_else(|| path.file_stem().map(|name| name.to_string_lossy().into_owned()))
                ).unwrap_or_default();
                let mut panel = make_panel((width * 0.40).max(360.0))
                    .child(div().text_3xl().font_weight(FontWeight::SEMIBOLD).child("Paused"))
                    .child(div().text_color(rgb(styles.muted.0)).child(title))
                    .child(self.overlay_button(Target::Resume, "Resume", cx))
                    .child(self.overlay_button(Target::EmulationSettings, "Emulation settings", cx));
                if self.session_is_gba() {
                    panel = panel
                        .child(row().gap_2()
                            .child(self.overlay_button(Target::SaveState, format!("Save Slot {}", self.gba_save_slot), cx))
                            .child(self.overlay_button(Target::LoadState, format!("Load Slot {}", self.gba_save_slot), cx))
                            .child(self.overlay_button(Target::CycleSaveSlot, format!("Slot {}", self.gba_save_slot), cx)))
                        .child(row().gap_2()
                            .child(self.overlay_button(Target::RewindStep, "Rewind Step", cx))
                            .child(self.overlay_button(Target::ToggleFastForward, if self.gba_fast_forward { "Fast-Forward: On" } else { "Fast-Forward: Off" }, cx)))
                        .child(row().gap_2()
                            .child(self.overlay_button(Target::ToggleGbaColorCorrection, if self.gba_color_correction { "GBA LCD Color: On" } else { "GBA LCD Color: Off" }, cx))
                            .child(self.overlay_button(Target::ToggleGbaBlending, if self.gba_interframe_blending { "Frame Blend: On" } else { "Frame Blend: Off" }, cx)))
                        .child(self.overlay_button(Target::ResetGame, "Reset GBA", cx));
                }
                panel
                    .child(self.overlay_button(Target::Restart, "Restart", cx))
                    .child(self.overlay_button(Target::Stop, "Close Game", cx))
                    .when_some(self.message.clone(), |panel, message| panel.child(div().text_sm().child(message)))
            }
            Overlay::SoftwareKeyboard => {
                let mut dialog = column()
                    .w(px(f32::from(theme.layout.dialog_width).max(460.0)))
                    .max_w_full()
                    .p_6()
                    .gap_4()
                    .rounded(px(styles.corner_radius.into()))
                    .bg(surface)
                    .text_color(rgb(styles.foreground.0));
                if let Some(swkbd) = &self.swkbd {
                    let header = if swkbd.header_text.is_empty() {
                        "Software Keyboard".to_owned()
                    } else {
                        swkbd.header_text.clone()
                    };
                    dialog = dialog.child(div().text_xl().font_weight(FontWeight::SEMIBOLD).child(header));
                    if !swkbd.sub_text.is_empty() {
                        dialog = dialog.child(div().text_sm().text_color(rgb(styles.muted.0)).child(swkbd.sub_text.clone()));
                    }
                    let display = swkbd.display_text();
                    let input_content = if display.is_empty() && !swkbd.guide_text.is_empty() {
                        div().text_color(rgb(styles.muted.0)).child(format!("{} _", swkbd.guide_text))
                    } else {
                        div().text_color(rgb(styles.foreground.0)).child(format!("{display}_"))
                    };
                    dialog = dialog.child(
                        div()
                            .w_full()
                            .min_h(px(44.0))
                            .p_3()
                            .rounded(px(f32::from(styles.corner_radius).min(10.0)))
                            .bg(background)
                            .border_2()
                            .border_color(rgb(styles.focus_end.0))
                            .child(input_content),
                    );
                    let count_text = if swkbd.min_length > 0 {
                        format!("{} / {} characters (min {})", swkbd.utf16_len(), swkbd.max_length, swkbd.min_length)
                    } else {
                        format!("{} / {} characters", swkbd.utf16_len(), swkbd.max_length)
                    };
                    dialog = dialog.child(div().text_xs().text_color(rgb(styles.muted.0)).child(count_text));
                    if let Some(feedback) = &swkbd.text_check {
                        let msg = match feedback {
                            TextCheckFeedback::Failure(m) | TextCheckFeedback::Confirm(m) => m.clone(),
                        };
                        dialog = dialog.child(div().text_sm().font_weight(FontWeight::MEDIUM).child(msg));
                    } else if let Some(err) = &swkbd.validation_error {
                        dialog = dialog.child(div().text_sm().child(err.clone()));
                    }
                    let ok_label = if swkbd.is_confirm_prompt() {
                        "Confirm".to_owned()
                    } else if swkbd.ok_text.is_empty() {
                        "OK".to_owned()
                    } else {
                        swkbd.ok_text.clone()
                    };
                    let mut actions = row().justify_end().gap_3();
                    if swkbd.is_confirm_prompt() {
                        actions = actions.child(self.overlay_button(Target::SwkbdEdit, "Edit", cx));
                    } else if !swkbd.disable_cancel {
                        actions = actions.child(self.overlay_button(Target::SwkbdCancel, "Cancel", cx));
                    }
                    actions = actions.child(self.overlay_button(Target::SwkbdSubmit, ok_label, cx));
                    dialog = dialog.child(actions);
                }
                dialog
            }
            Overlay::Confirm(action) => {
                let restart = action == Target::Restart;
                column().w(px(theme.layout.dialog_width.into())).max_w_full().p_6().gap_4()
                    .rounded(px(styles.corner_radius.into())).bg(surface).text_color(rgb(styles.foreground.0))
                    .child(div().text_xl().font_weight(FontWeight::SEMIBOLD).child(if restart { "Restart this game?" } else { "Close this game?" }))
                    .child(div().text_color(rgb(styles.muted.0)).child("Unsaved progress may be lost."))
                    .child(row().justify_end().gap_3()
                        .child(self.overlay_button(Target::Back, "Cancel", cx))
                        .child(self.overlay_button(Target::Approve, if restart { "Restart" } else { "Close Game" }, cx)))
            }
            Overlay::SavesMods => make_panel(450.0)
                .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child("Saves & Mods"))
                .child(self.overlay_button(Target::OpenMods, "Open mod folder", cx))
                .when(self.selected_program_id().is_some(), |view| {
                    view.child(self.overlay_button(Target::OpenGameMods, "Open game mod folder", cx))
                })
                .child(self.overlay_button(Target::OpenNand, "Open NAND folder", cx))
                .when_some(self.message.clone(), |panel, message| panel.child(div().text_sm().child(message)))
                .child(self.overlay_button(Target::Back, "Back", cx)),
            Overlay::Content => make_panel(450.0)
                .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child("Content"))
                .child(self.overlay_button(Target::InstallContent, "Install update or DLC", cx))
                .child(self.overlay_button(Target::InstallFirmware, "Install firmware", cx))
                .child(self.overlay_button(Target::VerifyContent, "Verify installed content", cx))
                .when_some(self.message.clone(), |panel, message| panel.child(div().p_2().child(message)))
                .child(self.overlay_button(Target::OpenGame, "Open game file", cx))
                .child(self.overlay_button(Target::OpenNand, "Open NAND folder", cx))
                .child(self.overlay_button(Target::Back, "Back", cx)),
            Overlay::More => make_panel(420.0)
                .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child("More"))
                .children(self.hidden_shortcuts().map(|item| self.overlay_button(Target::Shortcut(item), shortcut_label(item), cx)))
                .child(self.overlay_button(Target::Back, "Back", cx)),
            Overlay::Consoles | Overlay::Alphabet | Overlay::GameDetails => make_panel(500.0)
                .text_color(rgb(styles.foreground.0)).child(self.console_overlay(window, cx)),
            Overlay::None => div(),
        };
        let is_dialog = matches!(self.overlay, Overlay::Confirm(_) | Overlay::SoftwareKeyboard);
        let layer = row().absolute().size_full().top_0().left_0().bg(gpui::black().opacity(0.46))
            .occlude();
        if is_dialog { layer.items_center().justify_center().child(view) }
        else if justification { layer.justify_end().child(view) }
        else { layer.child(view) }
    }

    fn settings_panel(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = &self.appearance.package().theme;
        let categories = column().id("settings-categories").w(relative(0.36)).flex_none().gap_2()
            .overflow_y_scroll()
            .children(self.categories().iter().copied().map(|category| {
                let mut button = self.overlay_button(Target::Category(category), category.label(), cx);
                if self.category == category && self.target != Target::Category(category) {
                    button = button.bg(Hsla::from(rgb(theme.style.focus_end.0)).opacity(0.34));
                }
                button
            }));
        let mut settings = column().id("settings-options").flex_1().min_w_0().gap_2()
            .overflow_y_scroll().track_scroll(&self.view_scroll);
        if let Some(config) = &self.settings {
            settings = settings.children(self.category.values().iter().map(|setting| {
                let global = config.value(*setting);
                let value = match &self.game_options {
                    Some(path) => match config.game_value(path, *setting) {
                        Ok(Some(value)) => value.to_owned(),
                        Ok(None) => format!("{global} · Global"),
                        Err(_) => "Unavailable".into(),
                    },
                    None => global.to_owned(),
                };
                self.overlay_button(Target::Setting(*setting), setting.label(), cx).w_full()
                    .child(div().ml_auto().child(value))
            }));
        }
        settings = match self.category {
            Category::Emulation if self.game_options.is_none() => {
                let cheat_label = if self.editing_gba_cheat {
                    format!("Enter cheat: {}_ (Enter to add, Esc to cancel)", self.cheat_draft)
                } else {
                    "Add GBA cheat code".to_owned()
                };
                settings = settings
                    .child(self.overlay_button(Target::AddGbaCheat, cheat_label, cx).w_full()
                        .child(div().ml_auto().child(format!("{} active", self.gba_cheats.len()))));
                if !self.gba_cheats.is_empty() {
                    settings = settings.child(self.overlay_button(Target::ClearGbaCheats, "Clear GBA cheats", cx).w_full());
                }
                settings
            }
            Category::Graphics if self.game_options.is_none() => settings
                .child(self.overlay_button(Target::ToggleGbaSettingColor, "GBA LCD color correction", cx).w_full()
                    .child(div().ml_auto().child(if self.gba_color_correction { "On" } else { "Off" })))
                .child(self.overlay_button(Target::ToggleGbaSettingBlend, "GBA interframe blending", cx).w_full()
                    .child(div().ml_auto().child(if self.gba_interframe_blending { "On" } else { "Off" }))),
            Category::Controllers => settings.child(div().p_2().child(
                self.controller_name.clone().unwrap_or_else(|| "Connect a controller to navigate Home.".into())
            )),
            Category::Paths => {
                let bios_label = self.gba_bios_path.as_deref().unwrap_or("HLE BIOS (built-in)");
                settings = settings
                    .child(self.overlay_button(Target::AddFolder, "Add game folder", cx))
                    .child(self.overlay_button(Target::ToggleAutoMetadata, "Auto-extract game metadata", cx).w_full()
                        .child(div().ml_auto().child(if self.auto_extract_metadata { "On" } else { "Off" })))
                    .child(self.overlay_button(Target::OpenNand, "Open NAND folder", cx))
                    .child(self.overlay_button(Target::OpenMods, "Open mod folder", cx))
                    .child(self.overlay_button(Target::OpenLogs, "Open logs folder", cx))
                    .child(self.overlay_button(Target::PickGbaBios, "Select GBA BIOS", cx).w_full()
                        .child(div().ml_auto().child(bios_label.to_owned())));
                if self.gba_bios_path.is_some() {
                    settings = settings.child(self.overlay_button(Target::ClearGbaBios, "Clear custom GBA BIOS", cx).w_full());
                }
                settings.child(div().pt_2().text_xs().text_color(rgb(theme.style.muted.0)).child(
                    format!("Eglantine GPUI v{} · {}", env!("CARGO_PKG_VERSION"), eglantine_gpui::library::emulator_data_dir().display())
                ))
            }
            Category::Appearance => settings
                .child(self.theme_preview())
                .when_some(self.appearance.message.clone(), |options, message| {
                    options.child(div().text_sm().child(message))
                })
                .children(ThemeAction::ALL.iter().map(|action| {
                    self.overlay_button(Target::Theme(*action), action.label(), cx).w_full()
                        .child(div().ml_auto().child(self.appearance.value(*action)))
                })),
            _ => settings,
        };
        row().w_full().flex_1().min_h_0().gap_4().child(categories).child(settings)
    }

    fn theme_preview(&self) -> Div {
        let theme = &self.appearance.package().theme;
        let style = &theme.style;
        let layout = &theme.layout;
        let radius = px(f32::from(style.corner_radius).min(16.0));
        let screen = self.appearance.preview_screen();
        let chip = |label: &'static str, focused: bool| {
            div().px_2().py_1().rounded(px(f32::from(style.corner_radius).min(8.0)))
                .text_xs()
                .bg(if focused { highlight(style) } else { rgb(style.panel.0).into() })
                .text_color(rgb(if focused { style.focus_text.0 } else { style.foreground.0 }))
                .child(label)
        };
        let body = match screen {
            PreviewScreen::Home => {
                let covers = row().gap_2()
                    .child(div().size(px(46.0)).rounded(radius).bg(highlight(style))
                        .flex().items_end().p_1().text_xs().text_color(rgb(style.focus_text.0)).child("Focus"))
                    .child(div().size(px(46.0)).rounded(radius).bg(rgb(style.panel.0)));
                let nav = row().gap_1().flex_wrap()
                    .children(layout.home_shortcuts.iter().take(4).map(|id| chip(shortcut_label(*id), false)));
                match layout.home_navigation {
                    Placement::Below => column().gap_2().child(covers).child(nav),
                    Placement::Left => row().gap_2().child(column().gap_1().child(chip("Library", false)).child(chip("Settings", false))).child(covers),
                    Placement::Right => row().gap_2().child(covers).child(column().gap_1().child(chip("Library", false)).child(chip("Settings", false))),
                }
            }
            PreviewScreen::Library => {
                if layout.library_mode == LibraryMode::Grid {
                    row().gap_2()
                        .child(div().size(px(42.0)).rounded(radius).bg(highlight(style)))
                        .child(div().size(px(42.0)).rounded(radius).bg(rgb(style.panel.0)))
                        .child(div().size(px(42.0)).rounded(radius).bg(rgb(style.panel.0)))
                } else {
                    column().gap_1()
                        .child(chip("Title One", true))
                        .child(chip("Title Two", false))
                }
            }
            PreviewScreen::Console => self.console_theme_preview(),
            PreviewScreen::Pause => {
                let sheet = column().w(px(140.0)).p_2().gap_1().rounded(radius).bg(rgb(style.panel.0))
                    .child(chip("Resume", true))
                    .child(chip("Restart", false))
                    .child(chip("Close Game", false));
                row().w_full()
                    .when(layout.pause_side == PanelSide::Right, |r| r.justify_end())
                    .child(sheet)
            }
            PreviewScreen::Dialog => {
                let width = (f32::from(layout.dialog_width) * 0.42).clamp(130.0, 260.0);
                row().w_full().justify_center().child(
                    column().w(px(width)).p_2().gap_2().rounded(radius).bg(rgb(style.panel.0))
                        .child(div().text_xs().text_color(rgb(style.foreground.0)).child("Close this game?"))
                        .child(row().justify_end().gap_1().child(chip("Cancel", true)).child(chip("Close", false))),
                )
            }
        };
        let mut card = column().w_full().p_3().gap_2().relative().overflow_hidden()
            .rounded(radius)
            .bg(rgb(style.background.0))
            .text_color(rgb(style.foreground.0))
            .font_family(style.font_family.clone());
        if let Some(bg) = style.background_asset.as_ref().and_then(|id| self.appearance.image(id)) {
            card = card.child(img(bg).absolute().size_full().top_0().left_0().object_fit(ObjectFit::Cover))
                .child(div().absolute().size_full().top_0().left_0().bg(Hsla::from(rgb(style.background.0)).opacity(0.72)));
        }
        card.child(
            row().relative().w_full().justify_between().items_center()
                .child(div().text_xs().font_weight(FontWeight::SEMIBOLD).child(format!("{} · {}", theme.name, screen.label())))
                .child(div().text_xs().text_color(rgb(style.muted.0)).child(match layout.home_mode {
                    HomeMode::Shelf => "Shelf",
                    HomeMode::Grid => "Grid",
                })),
        )
        .child(div().relative().child(body))
    }

    fn console_theme_preview(&self) -> Div {
        use eglantine_library::{ConsoleId, LibraryLayout};
        let theme = &self.appearance.package().theme;
        let (console, component) = self.appearance.console_preview();
        let scope = console.map_or("All consoles", ConsoleId::name);
        let part = theme.component(console, component);
        let shell = theme.component(console, ThemeComponent::Shell);
        let style = &theme.style;
        let background = part.background.unwrap_or(if component == ThemeComponent::Play {
            style.focus_start
        } else {
            style.panel
        });
        let foreground = part.foreground.unwrap_or(if component == ThemeComponent::Play {
            style.focus_text
        } else {
            style.foreground
        });
        let sample = |label, selected| {
            div().flex_1().min_w_0().p(px(f32::from(part.padding.unwrap_or(8)).min(20.)))
                .rounded(px(f32::from(part.radius.unwrap_or(style.corner_radius))))
                .bg(rgb(if selected { part.selected_background.unwrap_or(background).0 } else { background.0 }))
                .text_color(rgb(foreground.0)).text_xs()
                .border_2().border_color(if selected {
                    rgb(part.focus.unwrap_or(style.focus_end).0).into()
                } else {
                    gpui::transparent_black()
                })
                .child(label)
        };
        let layout = console.map(|console| theme.console_layout(console)).unwrap_or(LibraryLayout::List);
        let examples = match layout {
            LibraryLayout::Grid | LibraryLayout::Shelf => row().gap_2()
                .child(sample("Title one", true)).child(sample("Title two", false)),
            LibraryLayout::SplitPreview => row().gap_2()
                .child(column().flex_1().gap_1().child(sample("Title one", true)).child(sample("Title two", false)))
                .child(sample("Preview", false)),
            LibraryLayout::List | LibraryLayout::Existing => column().gap_1()
                .child(sample("Title one", true)).child(sample("Title two", false)),
        };
        column().gap_2().p_2()
            .bg(rgb(shell.background.unwrap_or(style.background).0))
            .text_color(rgb(shell.foreground.unwrap_or(style.foreground).0))
            .child(div().text_xs().child(format!("{scope} · {}", component.label())))
            .child(examples)
    }
}

fn row() -> Div { div().flex() }
fn column() -> Div { row().flex_col() }

pub fn shortcut_label(component: ComponentId) -> &'static str {
    match component {
        ComponentId::Library => "Library",
        ComponentId::Controllers => "Controllers",
        ComponentId::SavesMods => "Saves & Mods",
        ComponentId::Content => "Content",
        ComponentId::Settings => "Settings",
        ComponentId::Exit => "Exit",
    }
}

#[cfg(windows)]
fn shortcut_glyph(component: ComponentId) -> &'static str {
    // Use the platform-supplied Segoe MDL2 icon font for the Windows default theme.
    match component {
        ComponentId::Library => "\u{e8f1}",
        ComponentId::Controllers => "\u{e7fc}",
        ComponentId::SavesMods => "\u{e74e}",
        ComponentId::Content => "\u{e81e}",
        ComponentId::Settings => "\u{e713}",
        ComponentId::Exit => "\u{e8bb}",
    }
}

fn highlight(style: &ThemeStyle) -> Background {
    linear_gradient(
        90.0,
        linear_color_stop(rgb(style.focus_start.0), 0.0),
        linear_color_stop(rgb(style.focus_end.0), 1.0),
    )
}
