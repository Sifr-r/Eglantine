// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::ops::Range;

use eglantine_gpui::theme::{ComponentStyle, ThemeComponent};
use eglantine_library::{ConsoleFamily, ConsoleId, LibraryLayout};
use gpui::{Context, Div, FontWeight, MouseButton, ObjectFit, SharedString, Stateful, Window, div, img, prelude::*, px, relative, rgb, uniform_list};

use super::{Shell, Target, column, row};
use crate::shell::Overlay;

impl Shell {
    pub(super) fn console_component(&self, part: ThemeComponent) -> ComponentStyle {
        self.appearance.package().theme.component(self.console, part)
    }

    pub(super) fn console_button(&self, target: Target, label: impl Into<SharedString>, part: ThemeComponent,
        active: bool, cx: &Context<Self>) -> Stateful<Div> {
        self.scoped_console_button("main", target, label, part, active, cx)
    }

    pub(super) fn scoped_console_button(
        &self, scope: &'static str, target: Target, label: impl Into<SharedString>, part: ThemeComponent,
        active: bool, cx: &Context<Self>,
    ) -> Stateful<Div> {
        let style = self.console_component(part);
        let button = self.scoped_button(scope, target, label, cx);
        if style == ComponentStyle::default() {
            return button.when(active && self.target != target, |button| {
                button.border_1().border_color(rgb(self.appearance.package().theme.style.focus_end.0))
            });
        }
        button.when_some(style.radius, |button, value| button.rounded(px(f32::from(value))))
            .when_some(style.padding, |button, value| button.p(px(f32::from(value))))
            .when_some(if active { style.selected_background.or(style.background) } else { style.background },
                |button, value| button.bg(rgb(value.0)))
            .when_some(style.foreground, |button, value| button.text_color(rgb(value.0)))
            .when(active || self.target == target, |button| {
                button.border_1().border_color(rgb(style.focus.or(style.foreground)
                    .unwrap_or(self.appearance.package().theme.style.focus_end).0))
            })
    }

    pub(super) fn console_rail(&self, cx: &Context<Self>) -> Div {
        row().items_center().flex_wrap().gap_2()
            .children(self.rail_consoles().into_iter().map(|console| {
                let title = match console {
                    ConsoleId::GameBoy => "GB",
                    ConsoleId::GameBoyColor => "GBC",
                    ConsoleId::GameBoyAdvance => "GBA",
                    ConsoleId::Ds => "DS",
                    ConsoleId::Nintendo3Ds => "3DS",
                    ConsoleId::Switch => "Switch",
                    ConsoleId::Switch2 => "Switch 2",
                    other => other.name(),
                };
                self.console_button(Target::Console(console), title, ThemeComponent::ConsoleTab,
                    self.console == Some(console), cx)
            }))
            .child(self.console_button(Target::Catalog, "All systems", ThemeComponent::ConsoleTab, false, cx))
            .child(self.console_button(Target::EmulationSettings, "Emulation settings", ThemeComponent::Toolbar, false, cx).ml_auto())
            .child(self.console_button(Target::Appearance, "Appearance", ThemeComponent::Toolbar, false, cx))
    }

    pub(super) fn console_library(&self, window: &Window, cx: &Context<Self>) -> Div {
        let console = self.console.expect("console view requires a console");
        let theme = self.appearance.package().theme.style.clone();
        let toolbar = self.console_component(ThemeComponent::Toolbar);
        let layout = self.console_layout().expect("console view requires a layout");
        let grid = layout == LibraryLayout::Grid;
        let columns = self.library_columns();
        let count = self.library_indices.len();
        let viewport_width = f32::from(window.viewport_size().width);
        let wide = viewport_width >= 900.0;
        let preview_ratio = if console == ConsoleId::GameBoy { 0.57 } else { 0.37 };
        let preview_height = (((viewport_width - 48.) * preview_ratio - 28.) / 1.5)
            .min(f32::from(window.viewport_size().height) * 0.55);
        let has_preview = wide && count > 0 && (layout == LibraryLayout::SplitPreview
            || (console == ConsoleId::GameBoy && layout == LibraryLayout::List));
        let label = match self.search.is_empty() {
            true if !self.searching => format!("Search {}", console.name()),
            _ => format!("{}{}", self.search, if self.searching { "▏" } else { "" }),
        };
        let tools = row().flex_wrap().gap_2().items_center()
            .children([(Target::Back, "Home"), (Target::AddFolder, "Add games"),
                (Target::OpenGame, "Open file"), (Target::Refresh, "Rescan")].into_iter().map(|(target, label)|
                self.console_button(target, label, ThemeComponent::Toolbar, false, cx)))
            .child(self.console_button(Target::Search, label, ThemeComponent::Toolbar, self.searching, cx))
            .when(self.has_alphabetical_tools(), |tools| tools.child(self.console_button(Target::SortGames,
                if self.alphabetical { "Title A–Z" } else { "Recently played" }, ThemeComponent::Toolbar, false, cx)))
            .when(self.has_alphabetical_tools() && self.alphabetical && count > 0, |tools| tools.child(
                self.console_button(Target::Alphabet, "A–Z index", ThemeComponent::Toolbar, false, cx)))
            .child(div().child(format!("{count} games")));
        let part = if grid { ThemeComponent::LibraryTile } else { ThemeComponent::LibraryList };
        let collection = self.console_component(part);
        let list = column().flex_1().min_w_0().min_h_0().overflow_hidden()
            .rounded(px(f32::from(collection.radius.unwrap_or(10))))
            .when(!grid, |list| list.bg(rgb(collection.background.unwrap_or(theme.panel).0)))
            .when_some(collection.foreground, |list, value| list.text_color(rgb(value.0)));
        let list = if count == 0 {
            list.child(div().p_4().child(if !self.search.trim().is_empty() {
                "No games match your search."
            } else if Self::is_classic(console) {
                "Add games or rescan a folder to browse your collection here."
            } else {
                "This console is planned."
            }))
        } else {
            list.child(uniform_list("console-games", count.div_ceil(columns),
                cx.processor(move |shell, range: Range<usize>, _, cx| {
                    range.map(|row_index| {
                        let first = row_index * columns;
                        let end = (first + columns).min(shell.library_indices.len());
                        let indices = shell.library_indices.get(first..end).unwrap_or_default();
                        row().w_full().when(grid, |row| row.gap_3().pb_3()).children(indices.iter().map(|&index| {
                            if grid { shell.console_tile(index, columns, cx).into_any_element() }
                            else { shell.console_game_row(index, cx).into_any_element() }
                        }))
                    }).collect()
                })).flex_1().min_h_0().track_scroll(self.console_scroll.clone()))
        };
        let browse = row().flex_1().min_h_0().gap_4().child(list)
            .when(has_preview, |browse| browse.child(self.console_game_preview("preview", preview_height, cx)
                .w(relative(preview_ratio)).flex_none()));
        column().flex_1().min_h_0().p_6().gap_4()
            .child(self.console_rail(cx))
            .child(div().text_2xl().font_weight(FontWeight::SEMIBOLD).child(console.name()))
            .child(tools.when_some(toolbar.background, |tools, value| tools.bg(rgb(value.0)))
                .when_some(toolbar.foreground, |tools, value| tools.text_color(rgb(value.0))))
            .child(browse)
            .when(count > 0, |view| view.child(row().items_center().gap_3()
                .when(!has_preview && self.can_play_selection(), |actions| actions.child(
                    self.console_button(Target::PlaySelected, "Play", ThemeComponent::Play, false, cx)))
                .child(self.console_button(Target::GameDetails, "Game details", ThemeComponent::Toolbar, false, cx))))
    }

    fn console_game_row(&self, index: usize, cx: &Context<Self>) -> Stateful<Div> {
        let style = self.console_component(ThemeComponent::LibraryList);
        let theme = &self.appearance.package().theme.style;
        let selected = self.selected == index;
        let game = &self.entries()[index];
        let subtitle = game.info.summary_line(game.console);
        let background = if selected { style.selected_background.or(style.background) } else { style.background };
        row().id(("console-row", index)).w_full().gap_3().items_center().px_3()
            .min_h(px(56. + f32::from(style.padding.unwrap_or(0))))
            .border_2().border_color(if selected { rgb(style.focus.unwrap_or(theme.focus_end).0).into() }
                else { gpui::transparent_black() })
            .rounded(px(f32::from(style.radius.unwrap_or(4))))
            .bg(rgb(background.unwrap_or(theme.panel).0))
            .text_color(rgb(style.foreground.unwrap_or(theme.foreground).0))
            .cursor_pointer().hover(|view| view.opacity(0.84))
            .when_some(game.cover.clone(), |row, image| row.child(img(image).w(px(76.)).h(px(41.))
                .rounded_sm().object_fit(ObjectFit::Cover)))
            .child(
                column().min_w_0().flex_1()
                    .child(div().truncate().font_weight(FontWeight::SEMIBOLD).child(game.title.clone()))
                    .when_some(subtitle, |col, summary| {
                        col.child(div().text_xs().text_color(rgb(theme.muted.0)).truncate().child(summary))
                    }),
            )
            .on_click(cx.listener(move |shell, event: &gpui::ClickEvent, window, cx| {
                shell.select_game(index, event.click_count() >= 2, false, window, cx);
            }))
            .on_mouse_down(MouseButton::Right, cx.listener(move |shell, _, window, cx| {
                shell.select_game(index, true, false, window, cx);
            }))
    }

    fn console_tile(&self, index: usize, columns: usize, cx: &Context<Self>) -> Stateful<Div> {
        let style = self.console_component(ThemeComponent::LibraryTile);
        let theme = &self.appearance.package().theme.style;
        let game = &self.entries()[index];
        let selected = self.selected == index;
        let background = if selected { style.selected_background.or(style.background) } else { style.background };
        let art = div().h(px(104.)).w_full().overflow_hidden().rounded_sm()
            .bg(rgb(background.unwrap_or(theme.panel).0))
            .when_some(game.cover.clone(), |art, image| art.child(img(image).size_full().object_fit(ObjectFit::Cover)));
        column().id(("console-tile", index)).w(relative(1.0 / columns as f32)).min_w_0().gap_2().p(px(f32::from(style.padding.unwrap_or(5))))
            .rounded(px(f32::from(style.radius.unwrap_or(9))))
            .border_2().border_color(if selected { rgb(style.focus.unwrap_or(theme.focus_end).0).into() }
                else { gpui::transparent_black() })
            .bg(rgb(background.unwrap_or(theme.panel).0))
            .text_color(rgb(style.foreground.unwrap_or(theme.foreground).0))
            .cursor_pointer().hover(|view| view.opacity(0.84))
            .child(art).child(div().min_w_0().truncate().font_weight(FontWeight::SEMIBOLD).child(game.title.clone()))
            .on_click(cx.listener(move |shell, event: &gpui::ClickEvent, window, cx| {
                shell.select_game(index, event.click_count() >= 2, false, window, cx);
            }))
    }

    fn console_game_preview(&self, scope: &'static str, cover_height: f32, cx: &Context<Self>) -> Div {
        let style = self.console_component(ThemeComponent::Preview);
        let theme = &self.appearance.package().theme.style;
        let mut view = column().gap_3().min_h_0().p(px(f32::from(style.padding.unwrap_or(14))))
            .rounded(px(f32::from(style.radius.unwrap_or(10))))
            .bg(rgb(style.background.unwrap_or(theme.panel).0))
            .text_color(rgb(style.foreground.unwrap_or(theme.foreground).0));
        if self.library_indices.contains(&self.selected) && let Some(game) = self.entries().get(self.selected) {
            let subtitle = game.info.summary_line(game.console);
            let details = game.info.detail_rows(game.console, game.program_id.as_deref(), &game.title);
            let placeholder = game.console.map(ConsoleId::name).unwrap_or("No cover art");
            let mut cover_box = div().relative().w_full().h(px(cover_height)).flex_none().rounded_md().overflow_hidden()
                .bg(rgb(style.selected_background.unwrap_or(theme.background).0))
                .flex().items_center().justify_center();
            if let Some(image) = game.cover.clone() {
                cover_box = cover_box.child(img(image).absolute().size_full().object_fit(ObjectFit::Cover));
            } else {
                cover_box = cover_box.child(
                    column().items_center().gap_1().px_4()
                        .child(div().text_sm().font_weight(FontWeight::SEMIBOLD).text_color(rgb(theme.muted.0)).child(placeholder))
                        .when_some(game.info.format.clone(), |col, fmt| {
                            col.child(div().text_xs().text_color(rgb(theme.muted.0)).child(fmt))
                        }),
                );
            }
            let refresh_label = if self.metadata_busy { "Refreshing…" } else { "Refresh metadata" };
            let actions = row().flex_wrap().gap_2().items_center()
                .child(if self.can_play_selection() {
                    self.scoped_console_button(scope, Target::PlaySelected, "Play", ThemeComponent::Play, false, cx).into_any_element()
                } else {
                    div().text_sm().text_color(rgb(theme.muted.0)).child("Playback unavailable in this build").into_any_element()
                })
                .child(self.scoped_console_button(scope, Target::RefreshMetadata, refresh_label, ThemeComponent::Toolbar, false, cx));
            view = view.child(cover_box)
                .child(
                    column().gap_1().min_w_0()
                        .child(div().text_xl().font_weight(FontWeight::SEMIBOLD).truncate().child(game.title.clone()))
                        .when_some(subtitle, |col, summary| {
                            col.child(div().text_xs().text_color(rgb(theme.muted.0)).truncate().child(summary))
                        }),
                )
                .when(!details.is_empty(), |view| {
                    view.child(
                        column().id(SharedString::from(format!("preview-details-{scope}"))).gap_1().max_h(px(210.0)).overflow_y_scroll()
                            .children(details.into_iter().map(|(label, value)| {
                                row().justify_between().gap_3().text_xs()
                                    .child(div().flex_none().text_color(rgb(theme.muted.0)).child(label))
                                    .child(div().min_w_0().truncate().child(value))
                            })),
                    )
                })
                .child(actions);
        }
        view
    }

    pub(super) fn console_overlay(&self, window: &Window, cx: &Context<Self>) -> Div {
        match self.overlay {
            Overlay::Consoles => {
                let query = if self.catalog_query.is_empty() && !self.searching { "Search systems…".into() } else {
                    format!("{}{}", self.catalog_query, if self.searching { "▏" } else { "" })
                };
                let systems = self.catalog_results().collect::<Vec<_>>();
                let mut entries = column().id("console-catalog").flex_1().min_h_0().overflow_y_scroll()
                    .track_scroll(&self.view_scroll).gap_2();
                for (family, name) in [(ConsoleFamily::Home, "Home consoles"), (ConsoleFamily::Handheld, "Handheld consoles")] {
                    let group = systems.iter().copied().filter(|console| console.family() == family).collect::<Vec<_>>();
                    if !group.is_empty() {
                        entries = entries.child(div().text_sm().font_weight(FontWeight::SEMIBOLD).child(name));
                        entries = entries.children(group.into_iter().map(|console| {
                            let label = if console.supported() {
                                console.name().to_owned()
                            } else {
                                format!("{} · Planned", console.name())
                            };
                            self.overlay_button(Target::Console(console), label, cx)
                        }));
                    }
                }
                column().flex_1().min_h_0().gap_3().child(div().text_2xl().child("All systems"))
                    .child(self.overlay_button(Target::CatalogSearch, query, cx))
                    .child(self.overlay_button(Target::NativeLibrary, "Combined Switch library", cx))
                    .child(entries).child(self.overlay_button(Target::Back, "Back", cx))
            }
            Overlay::Alphabet => column().gap_3().child(div().text_xl().child("A–Z index"))
                .child(row().flex_wrap().gap_2().children(self.letter_index().into_iter().map(|(letter, _)|
                    self.overlay_button(Target::Letter(letter), letter.to_string(), cx))))
                .child(self.overlay_button(Target::Back, "Back", cx)),
            Overlay::GameDetails => column().gap_3().child(self.console_game_preview(
                "overlay",
                ((f32::from(window.viewport_size().width).min(500.) - 76.) / 1.5)
                    .min(f32::from(window.viewport_size().height) * 0.60), cx))
                .child(self.overlay_button(Target::Back, "Back", cx)),
            _ => unreachable!("console overlays are selected by their own targets"),
        }
    }
}
