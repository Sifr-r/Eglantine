// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::time::Duration;

use eglantine_gpui::{
    emulator_config::EmulatorSetting,
    theme::{ComponentId, HomeMode, LibraryMode, PanelSide, Placement, ThemeStyle},
};
use gpui::{
    Animation, AnimationExt, Background, Context, Div, FontWeight, Hsla, MouseButton, ObjectFit, SharedString,
    Window, div, img, linear_color_stop, linear_gradient, prelude::*, px, relative, rgb,
};

use crate::{
    appearance::{PreviewScreen, ThemeAction},
    shell::{Category, Overlay, Screen, Session, Shell, Target},
};

impl Render for Shell {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let width = f32::from(window.viewport_size().width);
        let height = f32::from(window.viewport_size().height);
        let style = &self.appearance.package().theme.style;
        let tile = f32::from(style.cover_size).min(height * 0.53).max(100.0);
        let layout = &self.appearance.package().theme.layout;
        let side_width = if layout.home_navigation == Placement::Below { 0.0 } else { 170.0 };
        let grid_width = if self.screen == Screen::Home { width - side_width - 64.0 } else { width - 64.0 };
        self.grid_columns = (grid_width / (tile + f32::from(style.spacing))).floor().max(1.0) as usize;

        let content = match self.screen {
            Screen::Home => self.home(window, cx, tile).into_any_element(),
            Screen::Library => self.library(cx, tile.min(210.0)).into_any_element(),
        };
        let background = style.background_asset.as_ref().and_then(|id| self.appearance.image(id));
        let text_scale = (f32::from(style.text_scale) / 100.0) * crate::controller::os_text_scale();
        let mut root = div()
            .id("eglantine-shell")
            .size_full()
            .relative()
            .overflow_hidden()
            .bg(rgb(style.background.0))
            .text_color(rgb(style.foreground.0))
            .font_family(style.font_family.clone())
            .text_size(px(16.0 * text_scale))
            .track_focus(&self.focus)
            .on_key_down(cx.listener(Self::on_key));
        if let Some(background) = background {
            root = root.child(img(background).absolute().size_full().top_0().left_0().object_fit(ObjectFit::Cover))
                .child(div().absolute().size_full().top_0().left_0().bg(Hsla::from(rgb(style.background.0)).opacity(0.72)));
        }
        let footer = self.footer();
        root = root.child(column().relative().size_full().child(content).child(footer));
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
    fn home(&self, window: &Window, cx: &Context<Self>, tile: f32) -> impl IntoElement {
        let theme = &self.appearance.package().theme;
        let gap = px(theme.style.spacing.into());
        let game = self.entries().get(self.selected);
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
        let game_shelf = if self.entries().is_empty() {
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
                .children(self.entries().iter().enumerate().map(|(index, _)| self.cover(index, tile, cx)))
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
            .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).min_h(px(36.0)).truncate().child(title))
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
            .child(header).child(games_and_shortcuts).child(div().h_2())
    }

    fn cover(&self, index: usize, tile: f32, cx: &Context<Self>) -> impl IntoElement {
        let entry = &self.entries()[index];
        let style = &self.appearance.package().theme.style;
        let target = Target::Game(index);
        let home = self.screen == Screen::Home;
        let selected = self.target == target || (self.overlay != Overlay::None && self.selected == index);
        let mut cover = div().id(("cover", index)).relative().size(px(tile)).flex_none()
            .rounded(px(style.corner_radius.into())).overflow_hidden().cursor_pointer()
            .bg(rgb(style.panel.0))
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
            cover = cover.child(
                div().absolute().size_full().left_0().top_0().bg(linear_gradient(
                    180.0,
                    linear_color_stop(Hsla::from(rgb(style.focus_start.0)).opacity(if entry.cover.is_some() { 0.0 } else { 1.0 }), 0.18),
                    linear_color_stop(Hsla::from(rgb(style.focus_end.0)).opacity(if entry.cover.is_some() { 0.82 } else { 1.0 }), 1.0),
                )),
            );
        }
        if entry.cover.is_none() {
            cover = cover.child(column().relative().size_full().justify_end().p_4().overflow_hidden()
                .text_size(px(22.0)).font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(if selected { style.focus_text.0 } else { style.foreground.0 }))
                .child(entry.title.clone()));
        }
        cover
    }

    fn library(&self, cx: &Context<Self>, tile: f32) -> impl IntoElement {
        let theme = &self.appearance.package().theme;
        let indices = self.visible_entries();
        let search = if self.search.is_empty() && !self.searching {
            "Search games".into()
        } else {
            format!("{}{}", self.search, if self.searching { "▏" } else { "" })
        };
        let tools = row().flex_wrap().gap_3().items_center()
            .child(self.button(Target::Back, "Home", cx))
            .child(self.button(Target::Search, search, cx)).child(self.button(Target::AddFolder, "Add folder", cx))
            .child(self.button(Target::OpenGame, "Open game", cx)).child(self.button(Target::Refresh, "Rescan", cx));
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
                let format = game.path.extension().and_then(|extension| extension.to_str()).unwrap_or_default().to_uppercase();
                self.button_surface(Target::Game(index), ("library-game", index), game.title.clone()).w_full()
                    .child(div().ml_auto().child(format))
                    .on_click(cx.listener(move |shell, event: &gpui::ClickEvent, window, cx| {
                        shell.select_game(index, event.click_count() >= 2, false, window, cx)
                    }))
                    .on_mouse_down(MouseButton::Right, cx.listener(move |shell, _, window, cx| {
                        shell.select_game(index, false, true, window, cx)
                    }))
            }))
        };
        column().flex_1().min_h_0().p_6().gap_4()
            .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child("Library")).child(tools)
            .child(list)
            .when(!self.visible_entries().is_empty(), |view| view.child(
                row().gap_3().child(self.button(Target::PlaySelected, "Play", cx))
                    .child(self.button(Target::GameOptions, "Game settings", cx))
            ))
    }

    fn button(&self, target: Target, label: impl Into<SharedString>, cx: &Context<Self>) -> gpui::Stateful<Div> {
        let label = label.into();
        let id = SharedString::from(format!("button-{label}"));
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
        let home_key = self.settings.as_ref().map_or("F10", |config| config.home_key());
        let status: SharedString = match self.session {
            Session::Starting => "Starting game…".into(),
            Session::Running => format!("Game running · {home_key} to pause").into(),
            Session::Pausing => "Pausing…".into(),
            Session::Paused => "Game paused".into(),
            Session::Resuming => "Resuming…".into(),
            Session::Closing => "Closing game…".into(),
            Session::Idle if self.searching => "Type to search · Enter to finish".into(),
            Session::Idle if self.controller_name.is_none() => "Enter  Open     +  Game options     Esc  Back".into(),
            Session::Idle => "A  Open     +  Game options     B  Back".into(),
        };
        column().px(px(32.0)).py_4().gap_2()
            .when_some(self.message.clone(), |footer, message| {
                footer.child(div().text_color(rgb(style.foreground.0)).child(message))
            })
            .child(div().text_sm().text_color(rgb(style.muted.0)).child(status))
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
        let make_panel = |width| column().w(px(width)).max_w_full().h_full().p_6().gap_4().bg(background);
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
                        .child(self.button(Target::Back, "Back", cx)))
            }
            Overlay::Pause => {
                let title = self.session_path.as_ref().and_then(|path|
                    self.entries().iter().find(|entry| &entry.path == path).map(|entry| entry.title.clone())
                    .or_else(|| path.file_stem().map(|name| name.to_string_lossy().into_owned()))
                ).unwrap_or_default();
                make_panel((width * 0.40).max(360.0))
                    .child(div().text_3xl().font_weight(FontWeight::SEMIBOLD).child("Paused"))
                    .child(div().text_color(rgb(styles.muted.0)).child(title))
                    .child(self.button(Target::Resume, "Resume", cx))
                    .child(self.button(Target::Restart, "Restart", cx))
                    .child(self.button(Target::Stop, "Close Game", cx))
                    .when_some(self.message.clone(), |panel, message| panel.child(div().text_sm().child(message)))
            }
            Overlay::Confirm(action) => {
                let restart = action == Target::Restart;
                column().w(px(theme.layout.dialog_width.into())).max_w_full().p_6().gap_4()
                    .rounded(px(styles.corner_radius.into())).bg(surface)
                    .child(div().text_xl().font_weight(FontWeight::SEMIBOLD).child(if restart { "Restart this game?" } else { "Close this game?" }))
                    .child(div().text_color(rgb(styles.muted.0)).child("Unsaved progress may be lost."))
                    .child(row().justify_end().gap_3()
                        .child(self.button(Target::Back, "Cancel", cx))
                        .child(self.button(Target::Approve, if restart { "Restart" } else { "Close Game" }, cx)))
            }
            Overlay::SavesMods => make_panel(450.0)
                .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child("Saves & Mods"))
                .child(self.button(Target::OpenMods, "Open mod folder", cx))
                .when(self.selected_program_id().is_some(), |view| {
                    view.child(self.button(Target::OpenGameMods, "Open game mod folder", cx))
                })
                .child(self.button(Target::OpenNand, "Open NAND folder", cx))
                .when_some(self.message.clone(), |panel, message| panel.child(div().text_sm().child(message)))
                .child(self.button(Target::Back, "Back", cx)),
            Overlay::Content => make_panel(450.0)
                .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child("Content"))
                .child(self.button(Target::InstallContent, "Install update or DLC", cx))
                .child(self.button(Target::InstallFirmware, "Install firmware", cx))
                .child(self.button(Target::VerifyContent, "Verify installed content", cx))
                .when_some(self.message.clone(), |panel, message| panel.child(div().p_2().child(message)))
                .child(self.button(Target::OpenGame, "Open game file", cx))
                .child(self.button(Target::OpenNand, "Open NAND folder", cx))
                .child(self.button(Target::Back, "Back", cx)),
            Overlay::More => make_panel(420.0)
                .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child("More"))
                .children(self.hidden_shortcuts().map(|item| self.button(Target::Shortcut(item), shortcut_label(item), cx)))
                .child(self.button(Target::Back, "Back", cx)),
            Overlay::None => div(),
        };
        let is_dialog = matches!(self.overlay, Overlay::Confirm(_));
        let layer = row().absolute().size_full().top_0().left_0().bg(gpui::black().opacity(0.46))
            .occlude();
        if is_dialog { layer.items_center().justify_center().child(view) }
        else if justification { layer.justify_end().child(view) }
        else { layer.child(view) }
    }

    fn settings_panel(&self, cx: &Context<Self>) -> impl IntoElement {
        let theme = &self.appearance.package().theme;
        let categories = column().w(relative(0.36)).flex_none().gap_2()
            .children(self.categories().iter().copied().map(|category| {
                let mut button = self.button(Target::Category(category), category.label(), cx);
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
                self.button(Target::Setting(*setting), setting_label(*setting), cx).w_full()
                    .child(div().ml_auto().child(value))
            }));
        }
        settings = match self.category {
            Category::Controllers => settings.child(div().p_2().child(
                self.controller_name.clone().unwrap_or_else(|| "Connect a controller to navigate Home.".into())
            )),
            Category::Paths => settings
                .child(self.button(Target::AddFolder, "Add game folder", cx))
                .child(self.button(Target::OpenNand, "Open NAND folder", cx))
                .child(self.button(Target::OpenMods, "Open mod folder", cx))
                .child(self.button(Target::OpenLogs, "Open logs folder", cx))
                .child(div().pt_2().text_xs().text_color(rgb(theme.style.muted.0)).child(
                    format!("Eglantine GPUI v{} · {}", env!("CARGO_PKG_VERSION"), eglantine_gpui::library::emulator_data_dir().display())
                )),
            Category::Appearance => settings
                .child(self.theme_preview())
                .when_some(self.appearance.message.clone(), |options, message| {
                    options.child(div().text_sm().child(message))
                })
                .children(ThemeAction::ALL.iter().map(|action| {
                    self.button(Target::Theme(*action), action.label(), cx).w_full()
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

fn setting_label(setting: EmulatorSetting) -> &'static str {
    match setting {
        EmulatorSetting::Renderer => "Renderer",
        EmulatorSetting::Resolution => "Resolution scale",
        EmulatorSetting::Vsync => "VSync",
        EmulatorSetting::GameDisplay => "Game display",
        EmulatorSetting::Volume => "Volume",
        EmulatorSetting::CpuAccuracy => "CPU accuracy",
        EmulatorSetting::UserProfile => "User profile",
        EmulatorSetting::HomeKey => "Home key (keyboard)",
        EmulatorSetting::HomeButton => "Home button (controller)",
    }
}

fn highlight(style: &ThemeStyle) -> Background {
    linear_gradient(
        90.0,
        linear_color_stop(rgb(style.focus_start.0), 0.0),
        linear_color_stop(rgb(style.focus_end.0), 1.0),
    )
}
