// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::{collections::HashMap, path::PathBuf, time::{Duration, Instant}};

use eglantine_gpui::{
    content::ContentOperation,
    emulator_config::{EmulatorConfig, EmulatorSetting},
    library::{self, Library},
    session::{GbaLaunchConfig, SessionClient},
    software_keyboard::SoftwareKeyboardState,
    theme::{ComponentId, HomeMode, LibraryMode, Placement, ThemePackage},
};
use eglantine_library::{ConsoleId, LibraryLayout};
use gpui::{Context, FocusHandle, KeyDownEvent, Task, Window};

#[path = "shell_effects.rs"]
mod effects;
use effects::Picker;
#[path = "console_navigation.rs"]
mod consoles;
use consoles::TabView;

use crate::{
    appearance::{Appearance, ThemeAction},
    controller::{Controller, Input},
    navigation::{Direction, move_focus},
    preferences::Preferences,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Target {
    Fullscreen,
    Game(usize),
    Shortcut(ComponentId),
    AddFolder,
    OpenGame,
    GameOptions,
    PlaySelected,
    Refresh,
    RefreshMetadata,
    ToggleAutoMetadata,
    Search,
    Catalog,
    CatalogSearch,
    Appearance,
    NativeLibrary,
    Console(ConsoleId),
    SortGames,
    Alphabet,
    Letter(char),
    GameDetails,
    Category(Category),
    Setting(EmulatorSetting),
    Theme(ThemeAction),
    OpenNand,
    OpenMods,
    OpenGameMods,
    OpenLogs,
    InstallContent,
    InstallFirmware,
    VerifyContent,
    More,
    Back,
    EmulationSettings,
    PauseGame,
    Resume,
    SaveState,
    LoadState,
    CycleSaveSlot,
    ToggleFastForward,
    RewindStep,
    ToggleGbaColorCorrection,
    ToggleGbaBlending,
    ResetGame,
    PickGbaBios,
    ClearGbaBios,
    ToggleGbaSettingColor,
    ToggleGbaSettingBlend,
    AddGbaCheat,
    ClearGbaCheats,
    Restart,
    Stop,
    Approve,
    SwkbdSubmit,
    SwkbdCancel,
    SwkbdEdit,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Category {
    Emulation,
    System,
    Cpu,
    Graphics,
    GraphicsAdvanced,
    Audio,
    Applets,
    Controllers,
    Debug,
    Paths,
    Appearance,
}

impl Category {
    pub const ALL: [Self; 11] = [
        Self::Emulation,
        Self::System,
        Self::Cpu,
        Self::Graphics,
        Self::GraphicsAdvanced,
        Self::Audio,
        Self::Applets,
        Self::Controllers,
        Self::Debug,
        Self::Paths,
        Self::Appearance,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Self::Emulation => "Emulation",
            Self::System => "System",
            Self::Cpu => "CPU",
            Self::Graphics => "Graphics",
            Self::GraphicsAdvanced => "Graphics (Adv)",
            Self::Audio => "Audio",
            Self::Applets => "Applets",
            Self::Controllers => "Controllers",
            Self::Debug => "Debug",
            Self::Paths => "Paths",
            Self::Appearance => "Appearance",
        }
    }

    pub fn values(self) -> &'static [EmulatorSetting] {
        use EmulatorSetting::*;
        match self {
            Self::Emulation => &[
                UseMultiCore,
                MemoryLayout,
                UseSpeedLimit,
                SpeedLimit,
                SlowSpeedLimit,
                TurboSpeedLimit,
                SyncCoreSpeed,
                UserProfile,
            ],
            Self::System => &[
                ConsoleMode,
                Language,
                Region,
                TimeZone,
                CpuClock,
                GpuClock,
                CustomRtcEnabled,
                RngSeedEnabled,
                AirplaneMode,
                UseVirtualSd,
                ExtContentFromGameDirs,
            ],
            Self::Cpu => &[
                CpuBackend,
                CpuAccuracy,
                UseCustomCpuTicks,
                CpuTicks,
                Prefer3dVcache,
                CpuoptFastmem,
                CpuoptFastmemExclusives,
                CpuoptUnsafeHostMmu,
                CpuoptUnsafeUnfuseFma,
                CpuoptUnsafeReduceFpError,
                CpuoptUnsafeIgnoreStandardFpcr,
                CpuoptUnsafeInaccurateNan,
                CpuoptUnsafeFastmemCheck,
                CpuoptUnsafeIgnoreGlobalMonitor,
                CpuoptPageTables,
                CpuoptBlockLinking,
                CpuoptReturnStackBuffer,
                CpuoptFastDispatcher,
                CpuoptContextElimination,
                CpuoptConstProp,
                CpuoptMiscIr,
                CpuoptReduceMisalignChecks,
                CpuoptRecompileExclusives,
                CpuoptIgnoreMemoryAborts,
            ],
            Self::Graphics => &[
                Renderer,
                VulkanDevice,
                Resolution,
                Vsync,
                ScalingFilter,
                FsrSharpening,
                AntiAliasing,
                AspectRatio,
                FullscreenMode,
                GameDisplay,
                AsyncGpuEmulation,
                NvidiaDlss,
                NvidiaDlssQuality,
                PostShaderEnabled,
                FrameGen,
                FrameGenType,
                FrameGenMultiplier,
                FrameGenTargetRate,
                FrameGenFlowScaleAuto,
                FrameGenFlowScale,
                FrameGenQueueTarget,
            ],
            Self::GraphicsAdvanced => &[
                GpuAccuracy,
                DmaAccuracy,
                GpuFenceBehavior,
                VramUsageMode,
                NvdecEmulation,
                MaxAnisotropy,
                AccelerateAstc,
                AstcRecompression,
                FramePacingMode,
                UseDiskShaderCache,
                UseVulkanDriverPipelineCache,
                UseMeshShaders,
                PrivateDriverShaderCache,
                DriverShaderCacheSize,
                ColdHintMode,
                UseReactiveFlushing,
                BarrierFeedbackLoops,
                SyncMemoryOperations,
                ForceMaxClock,
                EnableComputePipelines,
                UseVideoFramerate,
                EnableBufferHistory,
                UseAsyncShaders,
                AsyncPresentation,
                SkipCpuInnerInvalidation,
                FixBloomEffects,
                EmulateBgr565,
                GpuUnswizzleEnabled,
                GpuUnswizzleTextureSize,
                GpuUnswizzleStreamSize,
                GpuUnswizzleChunkSize,
                DynaState,
                SampleShading,
                VertexInputDynamicState,
                Mkw30fps,
                MkwDynamicFps,
                Hyrule30fps,
                HyruleDynamicFps,
            ],
            Self::Audio => &[AudioEngine, SoundMode, Volume, AudioMuted],
            Self::Applets => &[
                CabinetAppletMode,
                ControllerAppletMode,
                DataEraseAppletMode,
                ErrorAppletMode,
                NetConnectAppletMode,
                PlayerSelectAppletMode,
                SwkbdAppletMode,
                MiiEditAppletMode,
                WebAppletMode,
                ShopAppletMode,
                PhotoViewerAppletMode,
                OfflineWebAppletMode,
                LoginShareAppletMode,
                WifiWebAuthAppletMode,
                MyPageAppletMode,
                EnableOverlay,
            ],
            Self::Controllers => &[
                HomeKey,
                HomeButton,
                VibrationEnabled,
                AccurateVibrations,
                MotionEnabled,
                ControllerNavigation,
                EnableJoyconDriver,
                EnableProconDriver,
                EnableSwitch2Driver,
                Switch2AutoPair,
                Switch2CombineJoycons,
                Switch2OpticalMouse,
                Switch2GyroMouseMode,
                Switch2GyroActivationMode,
                Switch2GyroSensitivity,
                Switch2MouseSensitivity,
                DisableWgiXinput,
                EnableRawInput,
                EnableUdpController,
                MouseEnabled,
                KeyboardEnabled,
                EmulateAnalogKeyboard,
                EnableRingController,
                EnableIrSensor,
                RandomAmiiboId,
                TasEnable,
                TasLoop,
                PauseTasOnLoad,
                TouchscreenEnabled,
            ],
            Self::Debug => &[
                LogFlushLine,
                CensorUsername,
                UseGdbstub,
                EnableMcpServer,
                McpAllowWrites,
                DumpExefs,
                DumpNso,
                EnableFsAccessLog,
                QuestFlag,
                UseDevKeys,
                UseDebugAsserts,
                UseAutoStub,
                EnableAllControllers,
                PerformVulkanCheck,
                DisableWebApplet,
                DisableMacroJit,
                DisableMacroHle,
                RendererDebug,
                RendererShaderFeedback,
                EnableNsightAftermath,
                DisableShaderLoopSafetyChecks,
                EnableRenderdocHotkey,
                DisableBufferReorder,
                GpuLogLevel,
                GpuLogVulkanCalls,
                GpuLogShaderDumps,
                GpuLogMemoryTracking,
                GpuLogDriverDebug,
                RecordFrameTimes,
            ],
            _ => &[],
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Screen {
    Home,
    Library,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Overlay {
    None,
    Settings,
    SavesMods,
    Content,
    More,
    Consoles,
    Alphabet,
    GameDetails,
    Pause,
    SoftwareKeyboard,
    Confirm(Target),
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Session {
    Idle,
    Starting,
    Running,
    Pausing,
    Paused,
    Resuming,
    Closing,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum PauseDestination {
    PauseMenu,
    EmulationSettings,
    ConfirmStop,
}

#[derive(Clone, Copy)]
pub(super) enum PendingGbaAction {
    SaveState(u8),
    LoadState(u8),
    FastForward(bool),
    RewindStep,
    ColorCorrection(bool),
    Blending(bool),
    ResetGame,
}

pub struct Shell {
    pub(crate) dir: PathBuf,
    pub(crate) focus: FocusHandle,
    pub(crate) controller: Controller,
    pub(crate) target: Target,
    pub(crate) return_target: Target,
    pub(crate) selected: usize,
    pub(crate) screen: Screen,
    pub(crate) overlay: Overlay,
    pub(crate) category: Category,
    pub(crate) game_options: Option<PathBuf>,
    pub(crate) session: Session,
    pub(crate) session_path: Option<PathBuf>,
    pub(crate) library: Option<Library>,
    pub(crate) settings: Option<EmulatorConfig>,
    pub(crate) appearance: Appearance,
    pub(crate) search: String,
    pub(crate) searching: bool,
    pub(crate) console: Option<ConsoleId>,
    pub(crate) catalog_query: String,
    pub(crate) alphabetical: bool,
    pub(crate) library_indices: Vec<usize>,
    pub(crate) console_scroll: gpui::UniformListScrollHandle,
    tab_views: HashMap<Option<ConsoleId>, TabView>,
    pub(crate) message: Option<String>,
    pub(crate) home_released: bool,
    pub(crate) controller_name: Option<String>,
    pub(crate) active_ble_handoff: Option<String>,
    pub(crate) ble_handoff_deadline: Option<Instant>,
    pub(crate) grid_columns: usize,
    pub(crate) compact_rail: bool,
    pub(crate) content_busy: bool,
    pub(crate) metadata_busy: bool,
    pub(crate) auto_extract_metadata: bool,
    metadata_request: u64,
    pending_library_selection: Option<PathBuf>,
    user_profile_override: Option<u8>,
    pub(crate) gba_bios_path: Option<String>,
    pub(crate) gba_color_correction: bool,
    pub(crate) gba_interframe_blending: bool,
    pub(crate) gba_save_slot: u8,
    pub(crate) gba_fast_forward: bool,
    pub(crate) gba_cheats: Vec<String>,
    pub(crate) editing_gba_cheat: bool,
    pub(crate) cheat_draft: String,
    pub(super) pending_gba_action: Option<PendingGbaAction>,
    pub(crate) shelf_scroll: gpui::ScrollHandle,
    pub(crate) view_scroll: gpui::ScrollHandle,
    pub(super) pause_destination: PauseDestination,
    pub(super) parent_hwnd: Option<u64>,
    pub(crate) swkbd: Option<SoftwareKeyboardState>,
    client: SessionClient,
    _polling: Task<()>,
}

pub struct ShellConfig {
    pub dir: PathBuf,
    pub preferences: Preferences,
    pub message: Option<String>,
    pub safe_theme: bool,
    pub initial_game: Option<PathBuf>,
    pub user_profile_override: Option<u8>,
}

impl Shell {
    pub fn new(window: &mut Window, cx: &mut Context<Self>, config: ShellConfig) -> Self {
        let ShellConfig {
            dir,
            preferences,
            mut message,
            safe_theme,
            initial_game,
            user_profile_override,
        } = config;
        let library = Library::open(dir.clone()).map_err(|error| message = Some(error)).ok();
        let settings = EmulatorConfig::load(&dir).map_err(|error| message = Some(error)).ok();
        let package = ThemePackage::load_active(&dir, safe_theme)
            .map_err(|error| message = Some(format!("Couldn't open the selected theme: {error}")))
            .unwrap_or_default();
        let appearance = Appearance::new(package, cx);
        let focus = cx.focus_handle();
        window.focus(&focus);
        let shell = cx.weak_entity();
        window.on_window_should_close(cx, move |window, cx| {
            shell.update(cx, |shell, cx| {
                if shell.content_busy {
                    shell.message = Some("Wait for the content operation to finish before closing Eglantine.".into());
                    cx.notify();
                    return false;
                }
                shell.save_preferences(window);
                true
            }).unwrap_or(true)
        });
        let polling = cx.spawn_in(window, async |entity, cx| {
            loop {
                cx.background_executor().timer(Duration::from_millis(24)).await;
                if cx.update(|window, cx| entity.update(cx, |shell, cx| shell.poll(window, cx)))
                    .ok().and_then(Result::ok).is_none()
                {
                    break;
                }
            }
        });
        let controller = Controller::new();
        let controller_name = controller.name();
        let console = preferences.console.as_deref().and_then(ConsoleId::parse);
        let mut shell = Self {
            dir,
            focus,
            controller,
            target: Target::AddFolder,
            return_target: Target::AddFolder,
            selected: 0,
            screen: if console.is_some_and(|console| console.layout() != LibraryLayout::Existing) {
                Screen::Library
            } else { Screen::Home },
            overlay: Overlay::None,
            category: Category::Graphics,
            game_options: None,
            session: Session::Idle,
            session_path: None,
            library,
            settings,
            appearance,
            search: String::new(),
            searching: false,
            console,
            catalog_query: String::new(),
            alphabetical: console.is_some_and(Shell::is_classic),
            library_indices: Vec::new(),
            console_scroll: Default::default(),
            tab_views: HashMap::new(),
            message,
            home_released: true,
            controller_name,
            active_ble_handoff: None,
            ble_handoff_deadline: None,
            grid_columns: 4,
            compact_rail: false,
            content_busy: false,
            metadata_busy: false,
            auto_extract_metadata: preferences.auto_extract_metadata,
            metadata_request: 0,
            pending_library_selection: preferences.selected,
            user_profile_override,
            gba_bios_path: preferences.gba_bios_path,
            gba_color_correction: preferences.gba_color_correction,
            gba_interframe_blending: preferences.gba_interframe_blending,
            gba_save_slot: preferences.gba_save_slot.clamp(1, 9),
            gba_fast_forward: false,
            gba_cheats: preferences.gba_cheats,
            editing_gba_cheat: false,
            cheat_draft: String::new(),
            pending_gba_action: None,
            shelf_scroll: Default::default(),
            view_scroll: Default::default(),
            pause_destination: PauseDestination::PauseMenu,
            parent_hwnd: None,
            swkbd: None,
            client: SessionClient::default(),
            _polling: polling,
        };
        shell.refresh_metadata(false, window, cx);
        if let Some(game) = initial_game {
            shell.launch(game, window);
        }
        shell
    }

    pub(crate) fn selected_program_id(&self) -> Option<&str> {
        self.library_indices
            .contains(&self.selected)
            .then(|| self.entries().get(self.selected))
            .flatten()
            .and_then(|entry| entry.program_id.as_deref())
    }

    pub(crate) fn entries(&self) -> &[library::LibraryEntry] {
        self.library.as_ref().map_or(&[], Library::entries)
    }

    fn library_selection_path(&self) -> Option<PathBuf> {
        self.pending_library_selection.clone()
            .or_else(|| self.library_indices.contains(&self.selected)
                .then(|| self.entries()[self.selected].path.clone()))
    }

    fn set_library_selection(&mut self, index: usize) {
        self.selected = index;
        self.pending_library_selection = None;
    }

    pub(crate) fn visible_entries(&self) -> Vec<usize> {
        self.library_indices.clone()
    }

    pub(crate) fn hidden_shortcuts(&self) -> impl Iterator<Item = ComponentId> + '_ {
        const SHORTCUTS: [ComponentId; 6] = [
            ComponentId::Library, ComponentId::Controllers, ComponentId::SavesMods,
            ComponentId::Content, ComponentId::Settings, ComponentId::Exit,
        ];
        let available = &self.appearance.package().theme.layout.home_shortcuts;
        SHORTCUTS.into_iter().filter(|component| !available.contains(component))
    }

    pub(crate) fn categories(&self) -> &'static [Category] {
        if self.game_options.is_some() { &Category::ALL[..7] } else { &Category::ALL }
    }

    pub(crate) fn session_is_gba(&self) -> bool {
        self.session_path
            .as_ref()
            .and_then(|path| path.extension())
            .is_some_and(|ext| ext.eq_ignore_ascii_case("gba"))
    }

    pub(crate) fn gba_launch_config(&self) -> GbaLaunchConfig {
        GbaLaunchConfig {
            bios_path: self.gba_bios_path.clone(),
            color_correction: self.gba_color_correction,
            interframe_blending: self.gba_interframe_blending,
            cheats: self.gba_cheats.clone(),
        }
    }

    fn settings_targets(&self) -> Vec<Target> {
        let mut targets: Vec<Target> = match self.category {
            Category::Appearance => ThemeAction::ALL.into_iter().map(Target::Theme).collect(),
            Category::Paths => {
                let mut paths = vec![
                    Target::AddFolder,
                    Target::ToggleAutoMetadata,
                    Target::OpenNand,
                    Target::OpenMods,
                    Target::OpenLogs,
                    Target::PickGbaBios,
                ];
                if self.gba_bios_path.is_some() {
                    paths.push(Target::ClearGbaBios);
                }
                paths
            }
            Category::Graphics => {
                let mut items: Vec<Target> =
                    self.category.values().iter().copied().map(Target::Setting).collect();
                if self.game_options.is_none() {
                    items.push(Target::ToggleGbaSettingColor);
                    items.push(Target::ToggleGbaSettingBlend);
                }
                items
            }
            Category::Emulation => {
                let mut items: Vec<Target> =
                    self.category.values().iter().copied().map(Target::Setting).collect();
                if self.game_options.is_none() {
                    items.push(Target::AddGbaCheat);
                    if !self.gba_cheats.is_empty() {
                        items.push(Target::ClearGbaCheats);
                    }
                }
                items
            }
            _ => self.category.values().iter().copied().map(Target::Setting).collect(),
        };
        targets.shrink_to_fit();
        targets
    }

    fn focus_rows(&self) -> Vec<Vec<Target>> {
        match self.overlay {
            Overlay::Settings => {
                let mut rows: Vec<_> = self.settings_targets().into_iter().map(|target| vec![target]).collect();
                rows.push(vec![Target::Back]);
                return rows;
            }
            Overlay::SavesMods => {
                let mut rows = vec![vec![Target::OpenMods]];
                if self.selected_program_id().is_some() {
                    rows.push(vec![Target::OpenGameMods]);
                }
                rows.push(vec![Target::OpenNand]);
                rows.push(vec![Target::Back]);
                return rows;
            }
            Overlay::Content => return vec![
                vec![Target::InstallContent], vec![Target::InstallFirmware], vec![Target::VerifyContent],
                vec![Target::OpenGame], vec![Target::OpenNand], vec![Target::Back],
            ],
            Overlay::More => return self.hidden_shortcuts().map(|component| vec![Target::Shortcut(component)])
                .chain([vec![Target::Back]]).collect(),
            Overlay::Consoles => return [vec![Target::CatalogSearch], vec![Target::NativeLibrary]]
                .into_iter().chain(self.catalog_results().map(|console| vec![Target::Console(console)]))
                .chain([vec![Target::Back]]).collect(),
            Overlay::Alphabet => return self.letter_index().chunks(7)
                .map(|letters| letters.iter().map(|(letter, _)| Target::Letter(*letter)).collect())
                .chain([vec![Target::Back]]).collect(),
            Overlay::GameDetails => return if self.can_play_selection() {
                vec![vec![Target::PlaySelected, Target::RefreshMetadata], vec![Target::Back]]
            } else {
                vec![vec![Target::RefreshMetadata], vec![Target::Back]]
            },
            Overlay::Pause => return if self.session_is_gba() {
                vec![
                    vec![Target::Resume],
                    vec![Target::EmulationSettings],
                    vec![Target::SaveState, Target::LoadState, Target::CycleSaveSlot],
                    vec![Target::RewindStep, Target::ToggleFastForward],
                    vec![Target::ToggleGbaColorCorrection, Target::ToggleGbaBlending],
                    vec![Target::ResetGame],
                    vec![Target::Restart],
                    vec![Target::Stop],
                ]
            } else {
                vec![vec![Target::Resume], vec![Target::EmulationSettings], vec![Target::Restart], vec![Target::Stop]]
            },
            Overlay::SoftwareKeyboard => {
                let mut row = Vec::new();
                if let Some(swkbd) = &self.swkbd {
                    if swkbd.is_confirm_prompt() {
                        row.push(Target::SwkbdEdit);
                    } else if !swkbd.disable_cancel {
                        row.push(Target::SwkbdCancel);
                    }
                }
                row.push(Target::SwkbdSubmit);
                return vec![row];
            }
            Overlay::Confirm(_) => return vec![vec![Target::Back, Target::Approve]],
            Overlay::None => {}
        }
        let games: Vec<_> = self.visible_entries().into_iter().map(Target::Game).collect();
        let theme = &self.appearance.package().theme;
        match self.screen {
            Screen::Home => {
                let mut shortcuts: Vec<_> = theme.layout.home_shortcuts.iter().copied()
                    .map(Target::Shortcut).collect();
                if self.hidden_shortcuts().next().is_some() {
                    shortcuts.push(Target::More);
                }
                let games = if games.is_empty() { vec![Target::AddFolder] } else { games };
                let mut rows = vec![vec![Target::Fullscreen], self.rail_targets()];
                let columns = if theme.layout.home_mode == HomeMode::Shelf {
                    games.len()
                } else {
                    self.grid_columns
                };
                if theme.layout.home_navigation == Placement::Below {
                    rows.extend(games.chunks(columns).map(<[_]>::to_vec));
                    rows.push(shortcuts);
                } else {
                    for (index, chunk) in games.chunks(columns).enumerate() {
                        let mut row = chunk.to_vec();
                        if let Some(&shortcut) = shortcuts.get(index) {
                            if theme.layout.home_navigation == Placement::Left {
                                row.insert(0, shortcut);
                            } else {
                                row.push(shortcut);
                            }
                        }
                        rows.push(row);
                    }
                    rows.extend(shortcuts[games.len().div_ceil(columns).min(shortcuts.len())..].iter().map(|item| vec![*item]));
                }
                rows
            }
            Screen::Library => {
                let width = self.library_columns();
                let mut tools = if self.console_layout().is_some() {
                    vec![Target::Back, Target::AddFolder, Target::OpenGame, Target::Refresh, Target::Search]
                } else {
                    vec![Target::Back, Target::Search, Target::AddFolder, Target::OpenGame, Target::Refresh]
                };
                if self.has_alphabetical_tools() {
                    tools.push(Target::SortGames);
                    if self.alphabetical && !games.is_empty() {
                        tools.push(Target::Alphabet);
                    }
                }
                let mut rows = vec![self.rail_targets(), tools];
                rows.extend(games.chunks(width).map(<[_]>::to_vec));
                if !games.is_empty() {
                    rows.push(if self.native_playback() {
                        vec![Target::PlaySelected, Target::GameOptions, Target::GameDetails]
                    } else if self.can_play_selection() {
                        vec![Target::PlaySelected, Target::GameDetails]
                    } else { vec![Target::GameDetails] });
                }
                rows
            }
        }
    }

    pub(crate) fn on_key(&mut self, event: &KeyDownEvent, window: &mut Window, cx: &mut Context<Self>) {
        let key = event.keystroke.key.as_str();
        if self.appearance.key(event, cx) {
            cx.notify();
            return;
        }
        if self.overlay == Overlay::SoftwareKeyboard {
            match key {
                "enter" if !event.is_held => {
                    if event.keystroke.modifiers.shift
                        && self.swkbd.as_ref().is_some_and(|swkbd| swkbd.multiline)
                    {
                        if let Some(swkbd) = &mut self.swkbd {
                            swkbd.push_str("\n");
                        }
                    } else {
                        self.submit_swkbd();
                    }
                }
                "escape" if !event.is_held => {
                    if self.swkbd.as_mut().is_some_and(SoftwareKeyboardState::dismiss_confirm) {
                        // Returned from confirm prompt to text editing.
                    } else {
                        self.cancel_swkbd();
                    }
                }
                "backspace" => {
                    if let Some(swkbd) = &mut self.swkbd {
                        swkbd.backspace();
                    }
                }
                _ => {
                    if let Some(character) = &event.keystroke.key_char
                        && let Some(swkbd) = &mut self.swkbd
                    {
                        swkbd.push_str(character);
                    }
                }
            }
            cx.notify();
            return;
        }
        if self.editing_gba_cheat {
            match key {
                "enter" => {
                    let code = self.cheat_draft.trim().to_owned();
                    if !code.is_empty() {
                        self.gba_cheats.push(code);
                        self.message = Some(format!("Added GBA cheat ({} active).", self.gba_cheats.len()));
                    }
                    self.cheat_draft.clear();
                    self.editing_gba_cheat = false;
                }
                "escape" => {
                    self.cheat_draft.clear();
                    self.editing_gba_cheat = false;
                }
                "backspace" => {
                    self.cheat_draft.pop();
                }
                _ => {
                    if let Some(character) = &event.keystroke.key_char
                        && self.cheat_draft.len() < 128
                        && !character.chars().any(char::is_control)
                    {
                        self.cheat_draft.push_str(character);
                    }
                }
            }
            cx.notify();
            return;
        }
        if self.searching {
            let in_catalog = self.overlay == Overlay::Consoles;
            let selected = self.library_selection_path();
            let query = if in_catalog { &mut self.catalog_query } else { &mut self.search };
            match key {
                "enter" | "escape" => self.searching = false,
                "backspace" => { query.pop(); }
                _ => {
                    if let Some(character) = &event.keystroke.key_char
                        && query.len() < 4096
                        && !character.chars().any(char::is_control)
                    {
                        query.push_str(character);
                    }
                }
            }
            if !in_catalog {
                self.rebuild_library(selected.as_deref());
                self.scroll_to_game(self.selected);
            }
            cx.notify();
            return;
        }
        let home_key = self.settings.as_ref().map_or("F10", EmulatorConfig::home_key);
        let action = match key {
            "left" => Input::Move(Direction::Left),
            "right" => Input::Move(Direction::Right),
            "up" => Input::Move(Direction::Up),
            "down" => Input::Move(Direction::Down),
            "enter" | "space" if !event.is_held => Input::Accept,
            "escape" | "backspace" if !event.is_held => Input::Back,
            "pageup" if !event.is_held => Input::ConsoleStep(-1),
            "pagedown" if !event.is_held => Input::ConsoleStep(1),
            "+" | "=" if !event.is_held => Input::Options,
            _ if key.eq_ignore_ascii_case(home_key) && !event.is_held => Input::Home,
            "f11" if !event.is_held => {
                window.toggle_fullscreen();
                cx.notify();
                return;
            }
            _ => return,
        };
        self.handle_input(action, window, cx);
    }

    fn handle_input(&mut self, input: Input, window: &mut Window, cx: &mut Context<Self>) {
        if matches!(input, Input::Home) {
            if self.session == Session::Running {
                self.request_pause();
                cx.notify();
            }
            return;
        }
        if !matches!(self.session, Session::Idle | Session::Paused) || !self.home_released {
            return;
        }
        if self.appearance.is_editing() || self.searching || self.editing_gba_cheat {
            if matches!(input, Input::Back) {
                self.appearance.cancel_edit();
                self.searching = false;
                self.editing_gba_cheat = false;
                self.cheat_draft.clear();
                cx.notify();
            }
            return;
        }
        match input {
            Input::Move(direction) => {
                if self.overlay == Overlay::Settings {
                    self.move_settings_focus(direction);
                } else if let Some(next) = move_focus(&self.focus_rows(), self.target, direction) {
                    self.target = next;
                    if let Target::Game(index) = next {
                        self.set_library_selection(index);
                        self.scroll_to_game(index);
                    }
                    if self.overlay == Overlay::Consoles && let Target::Console(console) = next
                        && let Some(index) = self.catalog_position(console)
                    {
                        self.view_scroll.scroll_to_item(index);
                    }
                }
            }
            Input::Accept => self.activate(self.target, window, cx),
            Input::Back if self.overlay == Overlay::Pause => self.activate(Target::Resume, window, cx),
            Input::Back => self.back(cx),
            Input::ConsoleStep(step) if self.overlay == Overlay::None && self.session == Session::Idle => self.cycle_console(step),
            Input::ConsoleStep(_) => {},
            Input::Options => {
                if let (Some(game), Some(settings), Target::Setting(setting)) =
                    (&self.game_options, &mut self.settings, self.target)
                {
                    if let Err(error) = settings.clear_game(game, setting) {
                        self.message = Some(error);
                    }
                } else if self.native_playback() {
                    self.open_game_options();
                } else if self.overlay == Overlay::None && self.library_indices.contains(&self.selected) {
                    self.open_game_details();
                }
            }
            Input::Home => unreachable!(),
        }
        cx.notify();
    }

    fn scroll_to_game(&self, index: usize) {
        let layout = &self.appearance.package().theme.layout;
        if let Some(position) = self.library_indices.iter().position(|entry| *entry == index) {
            if self.screen == Screen::Home && layout.home_mode == HomeMode::Shelf {
                self.shelf_scroll.scroll_to_item(position);
            } else if self.console_layout().is_some() {
                self.console_scroll.scroll_to_item(position / self.library_columns(), gpui::ScrollStrategy::Center);
            } else {
                self.shelf_scroll.scroll_to_item(position / self.library_columns());
            }
        }
    }

    pub(crate) fn library_columns(&self) -> usize {
        match self.console_layout() {
            Some(LibraryLayout::Grid) => self.grid_columns,
            Some(_) => 1,
            None if self.screen == Screen::Library && self.appearance.package().theme.layout.library_mode == LibraryMode::List => 1,
            None => self.grid_columns,
        }
    }

    fn move_settings_focus(&mut self, direction: Direction) {
        if let Target::Category(category) = self.target {
            if matches!(direction, Direction::Right) {
                self.select_category(category);
                return;
            }
            let rows: Vec<_> = self.categories().iter().copied()
                .map(|category| vec![Target::Category(category)]).collect();
            if let Some(target) = move_focus(&rows, self.target, direction) {
                self.target = target;
            }
            return;
        }
        if let Target::Theme(action) = self.target
            && matches!(direction, Direction::Left | Direction::Right)
            && self.appearance.nudge(action, if direction == Direction::Left { -1 } else { 1 })
        {
            return;
        }
        if matches!(direction, Direction::Left) {
            self.target = Target::Category(self.category);
            return;
        }
        if let Target::Setting(setting) = self.target && matches!(direction, Direction::Right) {
            self.cycle_setting(setting, 1);
            return;
        }
        if let Some(target) = move_focus(&self.focus_rows(), self.target, direction) {
            self.target = target;
            if let Some(index) = self.settings_targets().iter().position(|item| *item == target) {
                self.view_scroll.scroll_to_item(index);
            }
        }
    }

    pub(crate) fn select_game(
        &mut self, index: usize, open: bool, options: bool, window: &mut Window, cx: &mut Context<Self>,
    ) {
        if self.session != Session::Idle || self.overlay != Overlay::None || !self.library_indices.contains(&index) {
            return;
        }
        self.set_library_selection(index);
        self.target = Target::Game(index);
        if options {
            self.open_game_options();
        } else if open {
            self.activate(Target::Game(index), window, cx);
        }
        cx.notify();
    }

    pub(crate) fn activate(&mut self, target: Target, window: &mut Window, cx: &mut Context<Self>) {
        let running_action = self.session == Session::Running
            && (matches!(
                target,
                Target::EmulationSettings | Target::PauseGame | Target::Stop | Target::Fullscreen
            ) || (self.overlay == Overlay::SoftwareKeyboard
                && matches!(target, Target::SwkbdSubmit | Target::SwkbdCancel | Target::SwkbdEdit)));
        let starting_fullscreen =
            self.session == Session::Starting && matches!(target, Target::Fullscreen);
        if (!matches!(self.session, Session::Idle | Session::Paused)
            && !running_action
            && !starting_fullscreen)
            || !self.home_released
        {
            return;
        }
        if !matches!(target, Target::Search | Target::CatalogSearch) {
            self.searching = false;
        }
        if !matches!(target, Target::AddGbaCheat) {
            self.editing_gba_cheat = false;
            self.cheat_draft.clear();
        }
        self.target = target;
        if self.overlay == Overlay::More && matches!(target, Target::Shortcut(_)) {
            self.target = Target::More;
            self.overlay = Overlay::None;
        }
        self.message = None;
        match target {
            Target::Fullscreen => window.toggle_fullscreen(),
            Target::Game(index) => {
                if self.library_indices.contains(&index) && let Some(path) = self.entries().get(index).map(|entry| entry.path.clone()) {
                    self.set_library_selection(index);
                    if self.native_playback() {
                        self.launch(path, window);
                    } else {
                        self.open_game_details();
                    }
                }
            }
            Target::Shortcut(ComponentId::Exit) => {
                if self.content_busy {
                    self.message = Some("Wait for the content operation to finish before closing Eglantine.".into());
                    cx.notify();
                    return;
                }
                self.save_preferences(window);
                cx.quit();
            }
            Target::Shortcut(ComponentId::Library) => {
                let selected = self.library_selection_path();
                self.screen = Screen::Library;
                self.rebuild_library(selected.as_deref());
                self.target = Target::Back;
                self.shelf_scroll.set_offset(gpui::point(gpui::px(0.0), gpui::px(0.0)));
                self.scroll_to_game(self.selected);
            }
            Target::Shortcut(ComponentId::Settings) => self.open_settings(Category::Graphics),
            Target::Shortcut(ComponentId::Controllers) => self.open_settings(Category::Controllers),
            Target::Shortcut(ComponentId::SavesMods) => self.open_overlay(Overlay::SavesMods, Target::OpenMods),
            Target::Shortcut(ComponentId::Content) => self.open_overlay(Overlay::Content, Target::InstallContent),
            Target::AddFolder => self.pick(Picker::Folder, window, cx),
            Target::OpenGame => self.pick(Picker::Game, window, cx),
            Target::GameOptions => self.open_game_options(),
            Target::PlaySelected => {
                if self.can_play_selection()
                    && let Some(path) = self.entries().get(self.selected).map(|entry| entry.path.clone()) {
                    self.set_library_selection(self.selected);
                    self.launch(path, window);
                }
            }
            Target::Refresh => self.refresh_metadata(false, window, cx),
            Target::RefreshMetadata => {
                self.message = Some("Refreshing game metadata…".into());
                self.refresh_metadata(true, window, cx);
            }
            Target::ToggleAutoMetadata => {
                self.auto_extract_metadata = !self.auto_extract_metadata;
                self.save_preferences(window);
                self.message = Some(if self.auto_extract_metadata {
                    "Automatic metadata extraction enabled.".into()
                } else {
                    "Automatic metadata extraction disabled.".into()
                });
                if self.auto_extract_metadata {
                    self.refresh_metadata(false, window, cx);
                }
            }
            Target::Search => self.searching = true,
            Target::CatalogSearch => self.searching = true,
            Target::Catalog => {
                self.catalog_query.clear();
                self.open_overlay(Overlay::Consoles, Target::CatalogSearch);
            }
            Target::Appearance => self.open_settings(Category::Appearance),
            Target::Console(console) => self.choose_console(Some(console)),
            Target::NativeLibrary => self.choose_console(None),
            Target::SortGames => {
                let selected = self.library_selection_path();
                self.alphabetical = !self.alphabetical;
                self.rebuild_library(selected.as_deref());
                self.scroll_to_game(self.selected);
            }
            Target::Alphabet => {
                if let Some((letter, _)) = self.letter_index().first() {
                    self.open_overlay(Overlay::Alphabet, Target::Letter(*letter));
                }
            }
            Target::Letter(letter) => self.jump_to_letter(letter),
            Target::GameDetails => self.open_game_details(),
            Target::Category(category) => self.select_category(category),
            Target::Setting(setting) => self.cycle_setting(setting, 1),
            Target::Theme(action) => self.appearance.activate(action, &self.dir, window, cx),
            Target::OpenNand => self.open_directory("nand", cx),
            Target::OpenMods => self.open_directory("load", cx),
            Target::OpenGameMods => {
                if let Some(program_id) = self.selected_program_id().map(str::to_owned) {
                    self.open_directory(&format!("load/{program_id}"), cx);
                }
            }
            Target::OpenLogs => self.open_directory("log", cx),
            Target::InstallContent => self.pick(Picker::Content, window, cx),
            Target::InstallFirmware => self.pick(Picker::Firmware, window, cx),
            Target::VerifyContent => self.run_content(ContentOperation::VerifyContent, None, window, cx),
            Target::More => {
                let component = self.hidden_shortcuts().next();
                if let Some(component) = component {
                    self.open_overlay(Overlay::More, Target::Shortcut(component));
                }
            }
            Target::Back => self.back(cx),
            Target::EmulationSettings if self.session == Session::Running => {
                self.pause_destination = PauseDestination::EmulationSettings;
                self.request_pause();
            }
            Target::EmulationSettings => self.open_settings(Category::Emulation),
            Target::PauseGame if self.session == Session::Running => {
                self.pause_destination = PauseDestination::PauseMenu;
                self.request_pause();
            }
            Target::PauseGame => {}
            Target::Resume if self.session == Session::Paused => {
                match self.client.resume() {
                    Ok(()) => self.session = Session::Resuming,
                    Err(error) => self.message = Some(error),
                }
            }
            Target::SaveState if self.session == Session::Paused && self.session_is_gba() => {
                let slot = self.gba_save_slot;
                match self.client.save_state(slot) {
                    Ok(()) => self.pending_gba_action = Some(PendingGbaAction::SaveState(slot)),
                    Err(error) => self.message = Some(error),
                }
            }
            Target::LoadState if self.session == Session::Paused && self.session_is_gba() => {
                let slot = self.gba_save_slot;
                match self.client.load_state(slot) {
                    Ok(()) => self.pending_gba_action = Some(PendingGbaAction::LoadState(slot)),
                    Err(error) => self.message = Some(error),
                }
            }
            Target::CycleSaveSlot if self.session == Session::Paused => {
                self.gba_save_slot = (self.gba_save_slot % 9) + 1;
            }
            Target::ToggleFastForward if self.session == Session::Paused && self.session_is_gba() => {
                let next = !self.gba_fast_forward;
                match self.client.set_fast_forward(next) {
                    Ok(()) => self.pending_gba_action = Some(PendingGbaAction::FastForward(next)),
                    Err(error) => self.message = Some(error),
                }
            }
            Target::RewindStep if self.session == Session::Paused && self.session_is_gba() => {
                match self.client.rewind_step() {
                    Ok(()) => self.pending_gba_action = Some(PendingGbaAction::RewindStep),
                    Err(error) => self.message = Some(error),
                }
            }
            Target::ToggleGbaColorCorrection if self.session == Session::Paused && self.session_is_gba() => {
                let next = !self.gba_color_correction;
                match self.client.set_video_options(next, self.gba_interframe_blending) {
                    Ok(()) => self.pending_gba_action = Some(PendingGbaAction::ColorCorrection(next)),
                    Err(error) => self.message = Some(error),
                }
            }
            Target::ToggleGbaBlending if self.session == Session::Paused && self.session_is_gba() => {
                let next = !self.gba_interframe_blending;
                match self.client.set_video_options(self.gba_color_correction, next) {
                    Ok(()) => self.pending_gba_action = Some(PendingGbaAction::Blending(next)),
                    Err(error) => self.message = Some(error),
                }
            }
            Target::ResetGame if self.session == Session::Paused && self.session_is_gba() => {
                match self.client.reset_game() {
                    Ok(()) => self.pending_gba_action = Some(PendingGbaAction::ResetGame),
                    Err(error) => self.message = Some(error),
                }
            }
            Target::PickGbaBios => self.pick(Picker::Bios, window, cx),
            Target::ClearGbaBios => {
                self.gba_bios_path = None;
                self.target = Target::PickGbaBios;
                self.message = Some("Cleared custom GBA BIOS (using HLE BIOS).".into());
            }
            Target::ToggleGbaSettingColor => self.gba_color_correction = !self.gba_color_correction,
            Target::ToggleGbaSettingBlend => self.gba_interframe_blending = !self.gba_interframe_blending,
            Target::AddGbaCheat => {
                self.editing_gba_cheat = true;
                self.cheat_draft.clear();
            }
            Target::ClearGbaCheats => {
                self.gba_cheats.clear();
                self.target = Target::AddGbaCheat;
                self.message = Some("Cleared all GBA cheats.".into());
            }
            Target::Stop if self.session == Session::Running => {
                self.pause_destination = PauseDestination::ConfirmStop;
                self.request_pause();
            }
            Target::Restart | Target::Stop if self.session == Session::Paused => {
                self.overlay = Overlay::Confirm(target);
                self.target = Target::Back;
            }
            Target::Approve => {
                if let Overlay::Confirm(action) = self.overlay {
                    if action != Target::Restart
                        && let Some(handoff) = self.active_ble_handoff.as_deref()
                    {
                        self.controller.resume_native_with_handoff(Some(handoff));
                        self.controller.set_native_muted(true);
                    }
                    let result = if action == Target::Restart { self.client.restart() } else { self.client.stop() };
                    match result {
                        Ok(()) => self.session = Session::Closing,
                        Err(error) => self.message = Some(error),
                    }
                }
            }
            Target::SwkbdSubmit => self.submit_swkbd(),
            Target::SwkbdCancel => self.cancel_swkbd(),
            Target::SwkbdEdit => {
                if let Some(swkbd) = &mut self.swkbd {
                    swkbd.dismiss_confirm();
                }
            }
            Target::Resume
            | Target::SaveState
            | Target::LoadState
            | Target::CycleSaveSlot
            | Target::ToggleFastForward
            | Target::RewindStep
            | Target::ToggleGbaColorCorrection
            | Target::ToggleGbaBlending
            | Target::ResetGame
            | Target::Stop
            | Target::Restart => {}
        }
        cx.notify();
    }

    fn save_preferences(&self, window: &Window) {
        let size = window.window_bounds().get_bounds().size;
        let mut preferences = Preferences {
            width: f32::from(size.width).round() as u16,
            height: f32::from(size.height).round() as u16,
            fullscreen: window.is_fullscreen(),
            selected: self.library_selection_path(),
            console: self.console.map(|console| console.as_str().to_owned()),
            gba_bios_path: self.gba_bios_path.clone(),
            gba_color_correction: self.gba_color_correction,
            gba_interframe_blending: self.gba_interframe_blending,
            gba_save_slot: self.gba_save_slot,
            gba_cheats: self.gba_cheats.clone(),
            auto_extract_metadata: self.auto_extract_metadata,
        };
        preferences.clamp_window();
        if let Err(error) = preferences.save(&self.dir) {
            eprintln!("{error}");
        }
    }

    fn back(&mut self, cx: &mut Context<Self>) {
        match self.overlay {
            Overlay::Pause => {},
            Overlay::Confirm(_) => {
                self.overlay = Overlay::Pause;
                self.target = Target::Resume;
            }
            Overlay::None if self.screen == Screen::Library => {
                let selected = self.library_selection_path();
                if !self.native_playback() {
                    self.choose_console(None);
                    return;
                }
                self.screen = Screen::Home;
                self.target = if self.hidden_shortcuts().any(|item| item == ComponentId::Library) {
                    Target::More
                } else {
                    Target::Shortcut(ComponentId::Library)
                };
                self.shelf_scroll.set_offset(gpui::point(gpui::px(0.0), gpui::px(0.0)));
                self.search.clear();
                self.searching = false;
                self.rebuild_library(selected.as_deref());
                self.scroll_to_game(self.selected);
            }
            Overlay::None => {},
            _ => {
                self.appearance.discard(cx);
                self.editing_gba_cheat = false;
                self.cheat_draft.clear();
                self.game_options = None;
                if self.session == Session::Paused {
                    self.overlay = Overlay::Pause;
                    self.target = Target::Resume;
                } else {
                    self.overlay = Overlay::None;
                    self.target = self.return_target;
                }
            }
        }
    }

    fn open_directory(&mut self, name: &str, cx: &mut Context<Self>) {
        let path = library::emulator_data_dir().join(name);
        if path.is_dir() {
            cx.reveal_path(&path);
        } else {
            self.message = Some(format!("The {name} folder does not exist yet."));
        }
    }

    fn open_settings(&mut self, category: Category) {
        self.open_overlay(Overlay::Settings, Target::Category(category));
        self.select_category(category);
    }

    fn open_game_options(&mut self) {
        if self.native_playback() && self.overlay == Overlay::None
            && self.visible_entries().contains(&self.selected)
            && let Some(entry) = self.entries().get(self.selected)
        {
            self.game_options = Some(entry.path.clone());
            self.open_settings(Category::Graphics);
        }
    }

    fn open_game_details(&mut self) {
        let target = if self.can_play_selection() { Target::PlaySelected } else { Target::Back };
        self.open_overlay(Overlay::GameDetails, target);
    }

    fn select_category(&mut self, category: Category) {
        self.category = category;
        self.editing_gba_cheat = false;
        self.cheat_draft.clear();
        if category == Category::Appearance {
            self.appearance.begin();
        }
        self.target = self.settings_targets().first().copied().unwrap_or(Target::Category(category));
        self.view_scroll.set_offset(gpui::point(gpui::px(0.0), gpui::px(0.0)));
    }

    pub(super) fn commit_gba_action(&mut self, action: PendingGbaAction) {
        match action {
            PendingGbaAction::SaveState(slot) => {
                self.message = Some(format!("Saved GBA state to slot {slot}."));
            }
            PendingGbaAction::LoadState(slot) => {
                self.message = Some(format!("Loaded GBA state from slot {slot}."));
            }
            PendingGbaAction::FastForward(enabled) => self.gba_fast_forward = enabled,
            PendingGbaAction::RewindStep => self.message = Some("Rewound 1 frame.".into()),
            PendingGbaAction::ColorCorrection(enabled) => self.gba_color_correction = enabled,
            PendingGbaAction::Blending(enabled) => self.gba_interframe_blending = enabled,
            PendingGbaAction::ResetGame => self.message = Some("Reset GBA console.".into()),
        }
    }

    fn submit_swkbd(&mut self) {
        let Some(swkbd) = &mut self.swkbd else { return };
        let Ok((request_id, text, confirmed)) = swkbd.prepare_submit() else { return };
        if let Err(error) = self.client.submit_software_keyboard(request_id, &text, confirmed) {
            swkbd.clear_submitting_on_error(error);
        }
    }

    fn cancel_swkbd(&mut self) {
        let Some(swkbd) = &mut self.swkbd else { return };
        let Ok(request_id) = swkbd.prepare_cancel() else { return };
        if let Err(error) = self.client.cancel_software_keyboard(request_id) {
            swkbd.clear_submitting_on_error(error);
        }
    }

    fn open_overlay(&mut self, overlay: Overlay, target: Target) {
        self.return_target = self.target;
        self.overlay = overlay;
        self.target = target;
        self.view_scroll.set_offset(gpui::point(gpui::px(0.0), gpui::px(0.0)));
    }

    fn cycle_setting(&mut self, setting: EmulatorSetting, direction: i32) {
        if let Some(settings) = &mut self.settings {
            let result = match &self.game_options {
                Some(path) => settings.cycle_game(path, setting, direction),
                None => settings.cycle(setting, direction),
            };
            if let Err(error) = result {
                self.message = Some(error);
            }
        }
    }


}
