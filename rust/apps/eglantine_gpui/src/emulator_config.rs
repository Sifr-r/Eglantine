// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::library::replace;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, Deserialize, Ord, PartialEq, PartialOrd, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum EmulatorSetting {
    // Emulation ([Core] + [GPUI])
    UseMultiCore,
    MemoryLayout,
    UseSpeedLimit,
    SpeedLimit,
    SlowSpeedLimit,
    TurboSpeedLimit,
    SyncCoreSpeed,
    UserProfile,

    // System ([System], [Services], [Data%20Storage])
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

    // CPU ([Cpu])
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

    // Graphics ([Renderer] + [GPUI])
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

    // Graphics (Advanced) ([Renderer])
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

    // Audio ([Audio] + [System])
    AudioEngine,
    SoundMode,
    Volume,
    AudioMuted,

    // Applets ([LibraryApplet])
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

    // Controllers ([GPUI] + [Controls])
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

    // Debug ([Debugging], [Renderer], [Miscellaneous])
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
}

type Choice = (&'static str, &'static str);

struct SettingInfo {
    section: &'static str,
    key: &'static str,
    label: &'static str,
    choices: &'static [Choice],
    default: usize,
}

const BOOL_CHOICES: &[Choice] = &[("false", "Off"), ("true", "On")];
const APPLET_MODES: &[Choice] = &[("0", "HLE"), ("1", "LLE"), ("2", "Disabled")];
const CLOCK_MODES: &[Choice] = &[("0", "Normal"), ("1", "Boost"), ("2", "Overclock")];
const UNSWIZZLE_LEVELS: &[Choice] = &[
    ("0", "Very Low"),
    ("1", "Low"),
    ("2", "Normal"),
    ("3", "Medium"),
    ("4", "High"),
];

const fn bool_info(
    section: &'static str,
    key: &'static str,
    label: &'static str,
    default_on: bool,
) -> SettingInfo {
    SettingInfo {
        section,
        key,
        label,
        choices: BOOL_CHOICES,
        default: if default_on { 1 } else { 0 },
    }
}

const fn applet_info(key: &'static str, label: &'static str, default_lle: bool) -> SettingInfo {
    SettingInfo {
        section: "LibraryApplet",
        key,
        label,
        choices: APPLET_MODES,
        default: if default_lle { 1 } else { 0 },
    }
}

impl EmulatorSetting {
    pub const ALL: &'static [Self] = &[
        Self::UseMultiCore,
        Self::MemoryLayout,
        Self::UseSpeedLimit,
        Self::SpeedLimit,
        Self::SlowSpeedLimit,
        Self::TurboSpeedLimit,
        Self::SyncCoreSpeed,
        Self::UserProfile,
        Self::ConsoleMode,
        Self::Language,
        Self::Region,
        Self::TimeZone,
        Self::CpuClock,
        Self::GpuClock,
        Self::CustomRtcEnabled,
        Self::RngSeedEnabled,
        Self::AirplaneMode,
        Self::UseVirtualSd,
        Self::ExtContentFromGameDirs,
        Self::CpuBackend,
        Self::CpuAccuracy,
        Self::UseCustomCpuTicks,
        Self::CpuTicks,
        Self::Prefer3dVcache,
        Self::CpuoptFastmem,
        Self::CpuoptFastmemExclusives,
        Self::CpuoptUnsafeHostMmu,
        Self::CpuoptUnsafeUnfuseFma,
        Self::CpuoptUnsafeReduceFpError,
        Self::CpuoptUnsafeIgnoreStandardFpcr,
        Self::CpuoptUnsafeInaccurateNan,
        Self::CpuoptUnsafeFastmemCheck,
        Self::CpuoptUnsafeIgnoreGlobalMonitor,
        Self::CpuoptPageTables,
        Self::CpuoptBlockLinking,
        Self::CpuoptReturnStackBuffer,
        Self::CpuoptFastDispatcher,
        Self::CpuoptContextElimination,
        Self::CpuoptConstProp,
        Self::CpuoptMiscIr,
        Self::CpuoptReduceMisalignChecks,
        Self::CpuoptRecompileExclusives,
        Self::CpuoptIgnoreMemoryAborts,
        Self::Renderer,
        Self::VulkanDevice,
        Self::Resolution,
        Self::Vsync,
        Self::ScalingFilter,
        Self::FsrSharpening,
        Self::AntiAliasing,
        Self::AspectRatio,
        Self::FullscreenMode,
        Self::GameDisplay,
        Self::AsyncGpuEmulation,
        Self::NvidiaDlss,
        Self::NvidiaDlssQuality,
        Self::PostShaderEnabled,
        Self::FrameGen,
        Self::FrameGenType,
        Self::FrameGenMultiplier,
        Self::FrameGenTargetRate,
        Self::FrameGenFlowScaleAuto,
        Self::FrameGenFlowScale,
        Self::FrameGenQueueTarget,
        Self::GpuAccuracy,
        Self::DmaAccuracy,
        Self::GpuFenceBehavior,
        Self::VramUsageMode,
        Self::NvdecEmulation,
        Self::MaxAnisotropy,
        Self::AccelerateAstc,
        Self::AstcRecompression,
        Self::FramePacingMode,
        Self::UseDiskShaderCache,
        Self::UseVulkanDriverPipelineCache,
        Self::UseMeshShaders,
        Self::PrivateDriverShaderCache,
        Self::DriverShaderCacheSize,
        Self::ColdHintMode,
        Self::UseReactiveFlushing,
        Self::BarrierFeedbackLoops,
        Self::SyncMemoryOperations,
        Self::ForceMaxClock,
        Self::EnableComputePipelines,
        Self::UseVideoFramerate,
        Self::EnableBufferHistory,
        Self::UseAsyncShaders,
        Self::AsyncPresentation,
        Self::SkipCpuInnerInvalidation,
        Self::FixBloomEffects,
        Self::EmulateBgr565,
        Self::GpuUnswizzleEnabled,
        Self::GpuUnswizzleTextureSize,
        Self::GpuUnswizzleStreamSize,
        Self::GpuUnswizzleChunkSize,
        Self::DynaState,
        Self::SampleShading,
        Self::VertexInputDynamicState,
        Self::Mkw30fps,
        Self::MkwDynamicFps,
        Self::Hyrule30fps,
        Self::HyruleDynamicFps,
        Self::AudioEngine,
        Self::SoundMode,
        Self::Volume,
        Self::AudioMuted,
        Self::CabinetAppletMode,
        Self::ControllerAppletMode,
        Self::DataEraseAppletMode,
        Self::ErrorAppletMode,
        Self::NetConnectAppletMode,
        Self::PlayerSelectAppletMode,
        Self::SwkbdAppletMode,
        Self::MiiEditAppletMode,
        Self::WebAppletMode,
        Self::ShopAppletMode,
        Self::PhotoViewerAppletMode,
        Self::OfflineWebAppletMode,
        Self::LoginShareAppletMode,
        Self::WifiWebAuthAppletMode,
        Self::MyPageAppletMode,
        Self::EnableOverlay,
        Self::HomeKey,
        Self::HomeButton,
        Self::VibrationEnabled,
        Self::AccurateVibrations,
        Self::MotionEnabled,
        Self::ControllerNavigation,
        Self::EnableJoyconDriver,
        Self::EnableProconDriver,
        Self::EnableSwitch2Driver,
        Self::Switch2AutoPair,
        Self::Switch2CombineJoycons,
        Self::Switch2OpticalMouse,
        Self::Switch2GyroMouseMode,
        Self::Switch2GyroActivationMode,
        Self::Switch2GyroSensitivity,
        Self::Switch2MouseSensitivity,
        Self::DisableWgiXinput,
        Self::EnableRawInput,
        Self::EnableUdpController,
        Self::MouseEnabled,
        Self::KeyboardEnabled,
        Self::EmulateAnalogKeyboard,
        Self::EnableRingController,
        Self::EnableIrSensor,
        Self::RandomAmiiboId,
        Self::TasEnable,
        Self::TasLoop,
        Self::PauseTasOnLoad,
        Self::TouchscreenEnabled,
        Self::LogFlushLine,
        Self::CensorUsername,
        Self::UseGdbstub,
        Self::EnableMcpServer,
        Self::McpAllowWrites,
        Self::DumpExefs,
        Self::DumpNso,
        Self::EnableFsAccessLog,
        Self::QuestFlag,
        Self::UseDevKeys,
        Self::UseDebugAsserts,
        Self::UseAutoStub,
        Self::EnableAllControllers,
        Self::PerformVulkanCheck,
        Self::DisableWebApplet,
        Self::DisableMacroJit,
        Self::DisableMacroHle,
        Self::RendererDebug,
        Self::RendererShaderFeedback,
        Self::EnableNsightAftermath,
        Self::DisableShaderLoopSafetyChecks,
        Self::EnableRenderdocHotkey,
        Self::DisableBufferReorder,
        Self::GpuLogLevel,
        Self::GpuLogVulkanCalls,
        Self::GpuLogShaderDumps,
        Self::GpuLogMemoryTracking,
        Self::GpuLogDriverDebug,
        Self::RecordFrameTimes,
    ];

    pub fn label(self) -> &'static str {
        self.info().label
    }

    fn info(self) -> SettingInfo {
        match self {
            // Emulation
            Self::UseMultiCore => bool_info("Core", "use_multi_core", "Multicore emulation", true),
            Self::MemoryLayout => SettingInfo {
                section: "Core",
                key: "memory_layout_mode",
                label: "Memory layout",
                default: 0,
                choices: &[
                    ("0", "4 GB (Default)"),
                    ("1", "6 GB"),
                    ("2", "8 GB"),
                    ("3", "10 GB"),
                    ("4", "12 GB"),
                ],
            },
            Self::UseSpeedLimit => bool_info("Core", "use_speed_limit", "Limit speed", true),
            Self::SpeedLimit => SettingInfo {
                section: "Core",
                key: "speed_limit",
                label: "Speed limit",
                default: 3,
                choices: &[
                    ("25", "25%"),
                    ("50", "50%"),
                    ("75", "75%"),
                    ("100", "100%"),
                    ("125", "125%"),
                    ("150", "150%"),
                    ("200", "200%"),
                    ("300", "300%"),
                    ("400", "400%"),
                ],
            },
            Self::SlowSpeedLimit => SettingInfo {
                section: "Core",
                key: "slow_speed_limit",
                label: "Slow speed limit",
                default: 2,
                choices: &[
                    ("10", "10%"),
                    ("25", "25%"),
                    ("50", "50%"),
                    ("75", "75%"),
                    ("100", "100%"),
                ],
            },
            Self::TurboSpeedLimit => SettingInfo {
                section: "Core",
                key: "turbo_speed_limit",
                label: "Turbo speed limit",
                default: 2,
                choices: &[
                    ("125", "125%"),
                    ("150", "150%"),
                    ("200", "200%"),
                    ("250", "250%"),
                    ("300", "300%"),
                    ("400", "400%"),
                    ("500", "500%"),
                ],
            },
            Self::SyncCoreSpeed => bool_info("Core", "sync_core_speed", "Sync core speed", false),
            Self::UserProfile => SettingInfo {
                section: "GPUI",
                key: "user_profile",
                label: "User profile",
                default: 0,
                choices: &[
                    ("0", "Profile 1"),
                    ("1", "Profile 2"),
                    ("2", "Profile 3"),
                    ("3", "Profile 4"),
                    ("4", "Profile 5"),
                    ("5", "Profile 6"),
                    ("6", "Profile 7"),
                    ("7", "Profile 8"),
                ],
            },

            // System
            Self::ConsoleMode => SettingInfo {
                section: "System",
                key: "use_docked_mode",
                label: "Console mode",
                default: 0,
                choices: &[("0", "Handheld"), ("1", "Docked")],
            },
            Self::Language => SettingInfo {
                section: "System",
                key: "language_index",
                label: "System language",
                default: 1,
                choices: &[
                    ("0", "Japanese"),
                    ("1", "English (US)"),
                    ("2", "French"),
                    ("3", "German"),
                    ("4", "Italian"),
                    ("5", "Spanish"),
                    ("6", "Chinese"),
                    ("7", "Korean"),
                    ("8", "Dutch"),
                    ("9", "Portuguese"),
                    ("10", "Russian"),
                    ("11", "Taiwanese"),
                    ("12", "English (UK)"),
                    ("13", "French (Canada)"),
                    ("14", "Spanish (Latin America)"),
                    ("15", "Chinese (Simplified)"),
                    ("16", "Chinese (Traditional)"),
                    ("17", "Portuguese (Brazil)"),
                    ("18", "Polish"),
                    ("19", "Thai"),
                ],
            },
            Self::Region => SettingInfo {
                section: "System",
                key: "region_index",
                label: "Region",
                default: 1,
                choices: &[
                    ("0", "Japan"),
                    ("1", "USA"),
                    ("2", "Europe"),
                    ("3", "Australia"),
                    ("4", "China"),
                    ("5", "Korea"),
                    ("6", "Taiwan"),
                ],
            },
            Self::TimeZone => SettingInfo {
                section: "System",
                key: "time_zone_index",
                label: "Time zone",
                default: 0,
                choices: &[
                    ("0", "Auto"),
                    ("1", "Default (GMT)"),
                    ("2", "CET"),
                    ("3", "CST6CDT"),
                    ("4", "Cuba"),
                    ("5", "EET"),
                    ("6", "Egypt"),
                    ("7", "Eire"),
                    ("8", "EST"),
                    ("9", "EST5EDT"),
                    ("10", "GB"),
                    ("11", "GB-Eire"),
                    ("12", "GMT"),
                    ("13", "GMT+0"),
                    ("14", "GMT-0"),
                    ("15", "GMT0"),
                    ("16", "Greenwich"),
                    ("17", "Hongkong"),
                    ("18", "HST"),
                    ("19", "Iceland"),
                    ("20", "Iran"),
                    ("21", "Israel"),
                    ("22", "Jamaica"),
                    ("23", "Japan"),
                    ("24", "Kwajalein"),
                    ("25", "Libya"),
                    ("26", "MET"),
                    ("27", "MST"),
                    ("28", "MST7MDT"),
                    ("29", "Navajo"),
                    ("30", "NZ"),
                    ("31", "NZ-CHAT"),
                    ("32", "Poland"),
                    ("33", "Portugal"),
                    ("34", "PRC"),
                    ("35", "PST8PDT"),
                    ("36", "ROC"),
                    ("37", "ROK"),
                    ("38", "Singapore"),
                    ("39", "Turkey"),
                    ("40", "UCT"),
                    ("41", "Universal"),
                    ("42", "UTC"),
                    ("43", "W-SU"),
                    ("44", "WET"),
                    ("45", "Zulu"),
                ],
            },
            Self::CpuClock => SettingInfo {
                section: "System",
                key: "fast_cpu_time",
                label: "CPU clock",
                default: 0,
                choices: CLOCK_MODES,
            },
            Self::GpuClock => SettingInfo {
                section: "System",
                key: "fast_gpu_time",
                label: "GPU clock",
                default: 1,
                choices: CLOCK_MODES,
            },
            Self::CustomRtcEnabled => bool_info("System", "custom_rtc_enabled", "Custom RTC", false),
            Self::RngSeedEnabled => bool_info("System", "rng_seed_enabled", "Custom RNG seed", false),
            Self::AirplaneMode => bool_info("Services", "airplane_mode", "Airplane mode", true),
            Self::UseVirtualSd => bool_info("Data%20Storage", "use_virtual_sd", "Virtual SD card", true),
            Self::ExtContentFromGameDirs => bool_info(
                "Data%20Storage",
                "ext_content_from_game_dirs",
                "Load updates/DLC from game folders",
                true,
            ),

            // CPU
            Self::CpuBackend => SettingInfo {
                section: "Cpu",
                key: "cpu_backend",
                label: "CPU backend",
                default: 0,
                choices: &[("0", "Dynarmic"), ("1", "NCE")],
            },
            Self::CpuAccuracy => SettingInfo {
                section: "Cpu",
                key: "cpu_accuracy",
                label: "CPU accuracy",
                default: 0,
                choices: &[
                    ("0", "Auto"),
                    ("1", "Accurate"),
                    ("2", "Unsafe"),
                    ("3", "Paranoid"),
                    ("4", "Debugging"),
                ],
            },
            Self::UseCustomCpuTicks => bool_info("Cpu", "use_custom_cpu_ticks", "Custom CPU ticks", false),
            Self::CpuTicks => SettingInfo {
                section: "Cpu",
                key: "cpu_ticks",
                label: "CPU ticks",
                default: 3,
                choices: &[
                    ("77", "77"),
                    ("5000", "5000"),
                    ("10000", "10000"),
                    ("16000", "16000"),
                    ("24000", "24000"),
                    ("32000", "32000"),
                    ("48000", "48000"),
                    ("65535", "65535"),
                ],
            },
            Self::Prefer3dVcache => bool_info("Cpu", "prefer_3d_vcache", "Prefer 3D V-Cache cores", true),
            Self::CpuoptFastmem => bool_info("Cpu", "cpuopt_fastmem", "Enable fastmem", true),
            Self::CpuoptFastmemExclusives => bool_info("Cpu", "cpuopt_fastmem_exclusives", "Fastmem exclusives", true),
            Self::CpuoptUnsafeHostMmu => bool_info("Cpu", "cpuopt_unsafe_host_mmu", "Unsafe host MMU", true),
            Self::CpuoptUnsafeUnfuseFma => bool_info("Cpu", "cpuopt_unsafe_unfuse_fma", "Unsafe unfuse FMA", true),
            Self::CpuoptUnsafeReduceFpError => bool_info("Cpu", "cpuopt_unsafe_reduce_fp_error", "Unsafe reduce FP error", true),
            Self::CpuoptUnsafeIgnoreStandardFpcr => bool_info("Cpu", "cpuopt_unsafe_ignore_standard_fpcr", "Unsafe ignore FPCR", true),
            Self::CpuoptUnsafeInaccurateNan => bool_info("Cpu", "cpuopt_unsafe_inaccurate_nan", "Unsafe inaccurate NaN", true),
            Self::CpuoptUnsafeFastmemCheck => bool_info("Cpu", "cpuopt_unsafe_fastmem_check", "Unsafe skip fastmem check", true),
            Self::CpuoptUnsafeIgnoreGlobalMonitor => bool_info("Cpu", "cpuopt_unsafe_ignore_global_monitor", "Unsafe ignore global monitor", true),
            Self::CpuoptPageTables => bool_info("Cpu", "cpuopt_page_tables", "CPU opt: page tables", true),
            Self::CpuoptBlockLinking => bool_info("Cpu", "cpuopt_block_linking", "CPU opt: block linking", true),
            Self::CpuoptReturnStackBuffer => bool_info("Cpu", "cpuopt_return_stack_buffer", "CPU opt: return stack buffer", true),
            Self::CpuoptFastDispatcher => bool_info("Cpu", "cpuopt_fast_dispatcher", "CPU opt: fast dispatcher", true),
            Self::CpuoptContextElimination => bool_info("Cpu", "cpuopt_context_elimination", "CPU opt: context elimination", true),
            Self::CpuoptConstProp => bool_info("Cpu", "cpuopt_const_prop", "CPU opt: constant propagation", true),
            Self::CpuoptMiscIr => bool_info("Cpu", "cpuopt_misc_ir", "CPU opt: misc IR", true),
            Self::CpuoptReduceMisalignChecks => bool_info("Cpu", "cpuopt_reduce_misalign_checks", "CPU opt: reduce misalign checks", true),
            Self::CpuoptRecompileExclusives => bool_info("Cpu", "cpuopt_recompile_exclusives", "CPU opt: recompile exclusives", true),
            Self::CpuoptIgnoreMemoryAborts => bool_info("Cpu", "cpuopt_ignore_memory_aborts", "CPU opt: ignore memory aborts", true),

            // Graphics
            Self::Renderer => SettingInfo {
                section: "Renderer",
                key: "backend",
                label: "Renderer",
                default: 0,
                choices: &[
                    ("1", "Vulkan"),
                    ("0", "OpenGL"),
                    ("3", "OpenGL (GLASM)"),
                    ("4", "OpenGL (SPIR-V)"),
                    ("2", "Null"),
                ],
            },
            Self::VulkanDevice => SettingInfo {
                section: "Renderer",
                key: "vulkan_device",
                label: "Vulkan device",
                default: 0,
                choices: &[
                    ("0", "Device 0"),
                    ("1", "Device 1"),
                    ("2", "Device 2"),
                    ("3", "Device 3"),
                ],
            },
            Self::Resolution => SettingInfo {
                section: "Renderer",
                key: "resolution_setup",
                label: "Resolution scale",
                default: 3,
                choices: &[
                    ("0", "0.25×"),
                    ("1", "0.5×"),
                    ("2", "0.75×"),
                    ("3", "1×"),
                    ("4", "1.25×"),
                    ("5", "1.5×"),
                    ("6", "2×"),
                    ("7", "3×"),
                    ("8", "4×"),
                    ("9", "5×"),
                    ("10", "6×"),
                    ("11", "7×"),
                    ("12", "8×"),
                ],
            },
            Self::Vsync => SettingInfo {
                section: "Renderer",
                key: "use_vsync",
                label: "VSync",
                default: 2,
                choices: &[
                    ("0", "Off"),
                    ("1", "Mailbox"),
                    ("2", "On"),
                    ("3", "Adaptive"),
                ],
            },
            Self::ScalingFilter => SettingInfo {
                section: "Renderer",
                key: "scaling_filter",
                label: "Window adapting filter",
                default: 5,
                choices: &[
                    ("0", "Nearest Neighbor"),
                    ("1", "Bilinear"),
                    ("2", "Bicubic"),
                    ("3", "Gaussian"),
                    ("4", "Lanczos"),
                    ("5", "ScaleForce"),
                    ("6", "FSR 1.0"),
                    ("7", "Area"),
                    ("8", "Zero-Tangent"),
                    ("9", "B-Spline"),
                    ("10", "Mitchell"),
                    ("11", "Spline-1"),
                    ("12", "MMPX"),
                    ("13", "SGSR"),
                    ("14", "SGSR Edge"),
                    ("15", "FSR 3"),
                ],
            },
            Self::FsrSharpening => SettingInfo {
                section: "Renderer",
                key: "fsr_sharpening_slider",
                label: "FSR sharpness",
                default: 2,
                choices: &[
                    ("0", "0%"),
                    ("10", "10%"),
                    ("25", "25%"),
                    ("50", "50%"),
                    ("75", "75%"),
                    ("100", "100%"),
                    ("125", "125%"),
                    ("150", "150%"),
                    ("175", "175%"),
                    ("200", "200%"),
                ],
            },
            Self::AntiAliasing => SettingInfo {
                section: "Renderer",
                key: "anti_aliasing",
                label: "Anti-aliasing",
                default: 1,
                choices: &[("0", "None"), ("1", "FXAA"), ("2", "SMAA")],
            },
            Self::AspectRatio => SettingInfo {
                section: "Renderer",
                key: "aspect_ratio",
                label: "Aspect ratio",
                default: 0,
                choices: &[
                    ("0", "16:9"),
                    ("1", "4:3"),
                    ("2", "21:9"),
                    ("3", "16:10"),
                    ("4", "Stretch"),
                ],
            },
            Self::FullscreenMode => SettingInfo {
                section: "Renderer",
                key: "fullscreen_mode",
                label: "Fullscreen mode",
                default: if cfg!(windows) { 0 } else { 1 },
                choices: &[("0", "Borderless"), ("1", "Exclusive")],
            },
            Self::GameDisplay => SettingInfo {
                section: "GPUI",
                key: "game_fullscreen",
                label: "Game display",
                default: 0,
                choices: &[("false", "Windowed"), ("true", "Fullscreen")],
            },
            Self::AsyncGpuEmulation => bool_info("Renderer", "use_asynchronous_gpu_emulation", "Asynchronous GPU", true),
            Self::NvidiaDlss => bool_info("Renderer", "nvidia_dlss", "NVIDIA DLSS", false),
            Self::NvidiaDlssQuality => SettingInfo {
                section: "Renderer",
                key: "nvidia_dlss_quality",
                label: "DLSS quality",
                default: 2,
                choices: &[
                    ("0", "Performance"),
                    ("1", "Balanced"),
                    ("2", "Quality"),
                    ("3", "DLAA"),
                ],
            },
            Self::PostShaderEnabled => bool_info("Renderer", "post_shader_enabled", "Post-processing shader", true),
            Self::FrameGen => bool_info("Renderer", "frame_gen", "Frame generation", false),
            Self::FrameGenType => SettingInfo {
                section: "Renderer",
                key: "frame_gen_type",
                label: "Frame gen backend",
                default: 1,
                choices: &[("0", "LSFG"), ("1", "FSR 3")],
            },
            Self::FrameGenMultiplier => SettingInfo {
                section: "Renderer",
                key: "frame_gen_multiplier",
                label: "Frame gen multiplier",
                default: 0,
                choices: &[("2", "2×"), ("3", "3×"), ("4", "4×")],
            },
            Self::FrameGenTargetRate => SettingInfo {
                section: "Renderer",
                key: "frame_gen_target_rate",
                label: "Frame gen target FPS",
                default: 0,
                choices: &[
                    ("0", "Auto"),
                    ("60", "60 FPS"),
                    ("90", "90 FPS"),
                    ("120", "120 FPS"),
                    ("144", "144 FPS"),
                    ("165", "165 FPS"),
                    ("240", "240 FPS"),
                ],
            },
            Self::FrameGenFlowScaleAuto => bool_info("Renderer", "frame_gen_flow_scale_auto", "Frame gen auto flow scale", true),
            Self::FrameGenFlowScale => SettingInfo {
                section: "Renderer",
                key: "frame_gen_flow_scale",
                label: "Frame gen flow scale",
                default: 2,
                choices: &[
                    ("25", "25%"),
                    ("50", "50%"),
                    ("75", "75%"),
                    ("100", "100%"),
                ],
            },
            Self::FrameGenQueueTarget => SettingInfo {
                section: "Renderer",
                key: "frame_gen_queue_target",
                label: "Frame gen queue target",
                default: 0,
                choices: &[("0", "0"), ("1", "1"), ("2", "2")],
            },

            // Graphics (Advanced)
            Self::GpuAccuracy => SettingInfo {
                section: "Renderer",
                key: "gpu_accuracy",
                label: "GPU accuracy",
                default: 1,
                choices: &[("0", "Low"), ("1", "High")],
            },
            Self::DmaAccuracy => SettingInfo {
                section: "Renderer",
                key: "dma_accuracy",
                label: "DMA accuracy",
                default: 0,
                choices: &[("0", "Default"), ("1", "Unsafe"), ("2", "Safe")],
            },
            Self::GpuFenceBehavior => SettingInfo {
                section: "Renderer",
                key: "gpu_fence_behavior",
                label: "GPU fence behavior",
                default: 0,
                choices: &[
                    ("0", "Default"),
                    ("1", "Immediate"),
                    ("2", "Balanced"),
                    ("3", "Accurate"),
                ],
            },
            Self::VramUsageMode => SettingInfo {
                section: "Renderer",
                key: "vram_usage_mode",
                label: "VRAM usage mode",
                default: 0,
                choices: &[("0", "Conservative"), ("1", "Aggressive")],
            },
            Self::NvdecEmulation => SettingInfo {
                section: "Renderer",
                key: "nvdec_emulation",
                label: "NVDEC video decoding",
                default: 1,
                choices: &[("0", "Off"), ("1", "CPU"), ("2", "GPU")],
            },
            Self::MaxAnisotropy => SettingInfo {
                section: "Renderer",
                key: "max_anisotropy",
                label: "Anisotropic filtering",
                default: 0,
                choices: &[
                    ("0", "Automatic"),
                    ("1", "Default"),
                    ("2", "2×"),
                    ("3", "4×"),
                    ("4", "8×"),
                    ("5", "16×"),
                ],
            },
            Self::AccelerateAstc => SettingInfo {
                section: "Renderer",
                key: "accelerate_astc",
                label: "ASTC decoding",
                default: 1,
                choices: &[("0", "CPU"), ("1", "GPU"), ("2", "CPU Async")],
            },
            Self::AstcRecompression => SettingInfo {
                section: "Renderer",
                key: "astc_recompression",
                label: "ASTC recompression",
                default: 2,
                choices: &[("0", "Uncompressed"), ("1", "BC1"), ("2", "BC3")],
            },
            Self::FramePacingMode => SettingInfo {
                section: "Renderer",
                key: "frame_pacing_mode",
                label: "Frame pacing",
                default: 0,
                choices: &[
                    ("0", "Auto"),
                    ("1", "30 FPS"),
                    ("2", "60 FPS"),
                    ("3", "90 FPS"),
                    ("4", "120 FPS"),
                ],
            },
            Self::UseDiskShaderCache => bool_info("Renderer", "use_disk_shader_cache", "Disk pipeline cache", true),
            Self::UseVulkanDriverPipelineCache => bool_info("Renderer", "use_vulkan_driver_pipeline_cache", "Vulkan driver pipeline cache", true),
            Self::UseMeshShaders => bool_info("Renderer", "use_mesh_shaders", "Mesh shaders (VK_EXT_mesh_shader)", true),
            Self::PrivateDriverShaderCache => bool_info("Renderer", "private_driver_shader_cache", "Private driver shader cache", true),
            Self::DriverShaderCacheSize => SettingInfo {
                section: "Renderer",
                key: "driver_shader_cache_size",
                label: "Driver shader cache limit",
                default: 3,
                choices: &[
                    ("1", "1 GiB"),
                    ("2", "2 GiB"),
                    ("5", "5 GiB"),
                    ("10", "10 GiB"),
                    ("20", "20 GiB"),
                    ("50", "50 GiB"),
                    ("100", "100 GiB"),
                ],
            },
            Self::ColdHintMode => SettingInfo {
                section: "Renderer",
                key: "cold_hint_mode",
                label: "Cold memory paging",
                default: 0,
                choices: &[("0", "Off"), ("1", "When RAM is low"), ("2", "Always")],
            },
            Self::UseReactiveFlushing => bool_info("Renderer", "use_reactive_flushing", "Reactive flushing", true),
            Self::BarrierFeedbackLoops => bool_info("Renderer", "barrier_feedback_loops", "Barrier feedback loops", true),
            Self::SyncMemoryOperations => bool_info("Renderer", "sync_memory_operations", "Sync memory operations", false),
            Self::ForceMaxClock => bool_info("Renderer", "force_max_clock", "Force maximum clocks", false),
            Self::EnableComputePipelines => bool_info("Renderer", "enable_compute_pipelines", "Asynchronous compute", false),
            Self::UseVideoFramerate => bool_info("Renderer", "use_video_framerate", "Use video framerate", false),
            Self::EnableBufferHistory => bool_info("Renderer", "enable_buffer_history", "Enable buffer history", false),
            Self::UseAsyncShaders => bool_info("Renderer", "use_asynchronous_shaders", "Asynchronous shader building", true),
            Self::AsyncPresentation => bool_info("Renderer", "async_presentation", "Asynchronous presentation", false),
            Self::SkipCpuInnerInvalidation => bool_info("Renderer", "skip_cpu_inner_invalidation", "Skip CPU inner invalidation", false),
            Self::FixBloomEffects => bool_info("Renderer", "fix_bloom_effects", "Fix bloom effects", false),
            Self::EmulateBgr565 => bool_info("Renderer", "emulate_bgr565", "Emulate BGR565", false),
            Self::GpuUnswizzleEnabled => bool_info("Renderer", "gpu_unswizzle_enabled", "GPU texture unswizzling", false),
            Self::GpuUnswizzleTextureSize => SettingInfo {
                section: "Renderer",
                key: "gpu_unswizzle_texture_size",
                label: "Unswizzle texture size",
                default: 3,
                choices: &[
                    ("0", "Very Small"),
                    ("1", "Small"),
                    ("2", "Normal"),
                    ("3", "Large"),
                    ("4", "Very Large"),
                ],
            },
            Self::GpuUnswizzleStreamSize => SettingInfo {
                section: "Renderer",
                key: "gpu_unswizzle_stream_size",
                label: "Unswizzle stream size",
                default: 3,
                choices: UNSWIZZLE_LEVELS,
            },
            Self::GpuUnswizzleChunkSize => SettingInfo {
                section: "Renderer",
                key: "gpu_unswizzle_chunk_size",
                label: "Unswizzle chunk size",
                default: 3,
                choices: UNSWIZZLE_LEVELS,
            },
            Self::DynaState => SettingInfo {
                section: "Renderer",
                key: "dyna_state",
                label: "Extended dynamic state",
                default: 2,
                choices: &[
                    ("0", "Disabled"),
                    ("1", "EDS1"),
                    ("2", "EDS2"),
                    ("3", "EDS3"),
                ],
            },
            Self::SampleShading => SettingInfo {
                section: "Renderer",
                key: "sample_shading_fraction",
                label: "Sample shading",
                default: 0,
                choices: &[
                    ("0", "0%"),
                    ("25", "25%"),
                    ("50", "50%"),
                    ("75", "75%"),
                    ("100", "100%"),
                ],
            },
            Self::VertexInputDynamicState => bool_info("Renderer", "vertex_input_dynamic_state", "Vertex input dynamic state", true),
            Self::Mkw30fps => bool_info("Renderer", "mkw_30fps", "Mario Kart World 30 FPS", false),
            Self::MkwDynamicFps => bool_info("Renderer", "mkw_dynamic_fps", "Mario Kart World dynamic FPS", false),
            Self::Hyrule30fps => bool_info("Renderer", "hyrule_30fps", "Hyrule Warriors 30 FPS", false),
            Self::HyruleDynamicFps => bool_info("Renderer", "hyrule_dynamic_fps", "Hyrule Warriors dynamic FPS", false),

            // Audio
            Self::AudioEngine => SettingInfo {
                section: "Audio",
                key: "output_engine",
                label: "Audio output engine",
                default: 0,
                choices: &[
                    ("0", "Auto"),
                    ("1", "Cubeb"),
                    ("2", "SDL3"),
                    ("3", "Null"),
                ],
            },
            Self::SoundMode => SettingInfo {
                section: "System",
                key: "sound_index",
                label: "Sound mode",
                default: 1,
                choices: &[("0", "Mono"), ("1", "Stereo"), ("2", "Surround")],
            },
            Self::Volume => SettingInfo {
                section: "Audio",
                key: "volume",
                label: "Volume",
                default: 4,
                choices: &[
                    ("0", "0%"),
                    ("25", "25%"),
                    ("50", "50%"),
                    ("75", "75%"),
                    ("100", "100%"),
                    ("125", "125%"),
                    ("150", "150%"),
                    ("175", "175%"),
                    ("200", "200%"),
                ],
            },
            Self::AudioMuted => bool_info("Audio", "audio_muted", "Mute audio", false),

            // Applets
            Self::CabinetAppletMode => applet_info("cabinet_applet_mode", "Amiibo / Cabinet applet", true),
            Self::ControllerAppletMode => applet_info("controller_applet_mode", "Controller applet", false),
            Self::DataEraseAppletMode => applet_info("data_erase_applet_mode", "Data erase applet", false),
            Self::ErrorAppletMode => applet_info("error_applet_mode", "Error applet", true),
            Self::NetConnectAppletMode => applet_info("net_connect_applet_mode", "Network connect applet", true),
            Self::PlayerSelectAppletMode => applet_info("player_select_applet_mode", "Player select applet", true),
            Self::SwkbdAppletMode => applet_info("swkbd_applet_mode", "Software keyboard applet", false),
            Self::MiiEditAppletMode => applet_info("mii_edit_applet_mode", "Mii editor applet", true),
            Self::WebAppletMode => applet_info("web_applet_mode", "Web applet", false),
            Self::ShopAppletMode => applet_info("shop_applet_mode", "Shop applet", false),
            Self::PhotoViewerAppletMode => applet_info("photo_viewer_applet_mode", "Photo viewer applet", true),
            Self::OfflineWebAppletMode => applet_info("offline_web_applet_mode", "Offline web applet", true),
            Self::LoginShareAppletMode => applet_info("login_share_applet_mode", "Login share applet", false),
            Self::WifiWebAuthAppletMode => applet_info("wifi_web_auth_applet_mode", "Wi-Fi web auth applet", false),
            Self::MyPageAppletMode => applet_info("my_page_applet_mode", "My Page applet", true),
            Self::EnableOverlay => bool_info("LibraryApplet", "enable_overlay", "Enable applet overlay", false),

            // Controllers
            Self::HomeKey => SettingInfo {
                section: "GPUI",
                key: "home_key",
                label: "Home key (keyboard)",
                default: 0,
                choices: &[
                    ("F10", "F10"),
                    ("F9", "F9"),
                    ("F8", "F8"),
                    ("Home", "Home"),
                    ("Pause", "Pause"),
                ],
            },
            Self::HomeButton => SettingInfo {
                section: "GPUI",
                key: "home_button",
                label: "Home button (controller)",
                default: 0,
                choices: &[
                    ("guide", "Home / Guide"),
                    ("back", "Back / Select"),
                    ("start", "Start / Plus"),
                    ("leftstick", "Left Stick Click"),
                    ("rightstick", "Right Stick Click"),
                    ("misc1", "Capture / Misc"),
                ],
            },
            Self::VibrationEnabled => bool_info("Controls", "vibration_enabled", "Vibration", true),
            Self::AccurateVibrations => bool_info("Controls", "enable_accurate_vibrations", "Accurate HD rumble", false),
            Self::MotionEnabled => bool_info("Controls", "motion_enabled", "Motion controls", true),
            Self::ControllerNavigation => bool_info("Controls", "controller_navigation", "Controller UI navigation", true),
            Self::EnableJoyconDriver => bool_info("Controls", "enable_joycon_driver", "Direct Joy-Con driver", true),
            Self::EnableProconDriver => bool_info("Controls", "enable_procon_driver", "Direct Pro Controller driver", false),
            Self::EnableSwitch2Driver => bool_info("Controls", "enable_switch2_driver", "Switch 2 controller driver", true),
            Self::Switch2AutoPair => bool_info("Controls", "switch2_auto_pair", "Switch 2 auto-pair", true),
            Self::Switch2CombineJoycons => bool_info("Controls", "switch2_combine_joycons", "Switch 2 combine Joy-Cons", true),
            Self::Switch2OpticalMouse => bool_info("Controls", "switch2_optical_mouse", "Switch 2 optical mouse", true),
            Self::Switch2GyroMouseMode => SettingInfo {
                section: "Controls",
                key: "switch2_gyro_mouse_mode",
                label: "Switch 2 gyro mouse",
                default: 0,
                choices: &[("0", "Off"), ("1", "Yaw"), ("2", "Roll")],
            },
            Self::Switch2GyroActivationMode => SettingInfo {
                section: "Controls",
                key: "switch2_gyro_activation_mode",
                label: "Switch 2 gyro activation",
                default: 0,
                choices: &[("0", "Hold"), ("1", "Toggle")],
            },
            Self::Switch2GyroSensitivity => SettingInfo {
                section: "Controls",
                key: "switch2_gyro_sensitivity",
                label: "Switch 2 gyro sensitivity",
                default: 3,
                choices: &[
                    ("10", "10"),
                    ("20", "20"),
                    ("30", "30"),
                    ("40", "40"),
                    ("50", "50"),
                    ("75", "75"),
                    ("100", "100"),
                    ("150", "150"),
                    ("200", "200"),
                ],
            },
            Self::Switch2MouseSensitivity => SettingInfo {
                section: "Controls",
                key: "switch2_mouse_sensitivity",
                label: "Switch 2 mouse sensitivity",
                default: 2,
                choices: &[
                    ("10", "10"),
                    ("25", "25"),
                    ("50", "50"),
                    ("75", "75"),
                    ("100", "100"),
                    ("150", "150"),
                    ("200", "200"),
                ],
            },
            Self::DisableWgiXinput => bool_info("Controls", "disable_wgi_xinput", "Disable WGI XInput", false),
            Self::EnableRawInput => bool_info("Controls", "enable_raw_input", "Enable raw input", false),
            Self::EnableUdpController => bool_info("Controls", "enable_udp_controller", "Enable UDP motion server", false),
            Self::MouseEnabled => bool_info("Controls", "mouse_enabled", "Emulate mouse", false),
            Self::KeyboardEnabled => bool_info("Controls", "keyboard_enabled", "Emulate keyboard", false),
            Self::EmulateAnalogKeyboard => bool_info("Controls", "emulate_analog_keyboard", "Analog keyboard", false),
            Self::EnableRingController => bool_info("Controls", "enable_ring_controller", "Ring-Con controller", true),
            Self::EnableIrSensor => bool_info("Controls", "enable_ir_sensor", "Joy-Con IR sensor", false),
            Self::RandomAmiiboId => bool_info("Controls", "random_amiibo_id", "Randomize Amiibo ID", false),
            Self::TasEnable => bool_info("Controls", "tas_enable", "Enable TAS", false),
            Self::TasLoop => bool_info("Controls", "tas_loop", "Loop TAS script", false),
            Self::PauseTasOnLoad => bool_info("Controls", "pause_tas_on_load", "Pause TAS on load", true),
            Self::TouchscreenEnabled => bool_info("Controls", "touchscreen_enabled", "Touchscreen input", true),

            // Debug
            Self::LogFlushLine => bool_info("Miscellaneous", "flush_line", "Flush log on every line", false),
            Self::CensorUsername => bool_info("Miscellaneous", "censor_username", "Censor username in logs", true),
            Self::UseGdbstub => bool_info("Debugging", "use_gdbstub", "Enable GDB stub", false),
            Self::EnableMcpServer => bool_info("Debugging", "enable_mcp_server", "Enable MCP server", false),
            Self::McpAllowWrites => bool_info("Debugging", "mcp_allow_writes", "MCP allow writes", false),
            Self::DumpExefs => bool_info("Debugging", "dump_exefs", "Dump ExeFS", false),
            Self::DumpNso => bool_info("Debugging", "dump_nso", "Dump NSO modules", false),
            Self::EnableFsAccessLog => bool_info("Debugging", "enable_fs_access_log", "Log filesystem access", false),
            Self::QuestFlag => bool_info("Debugging", "quest_flag", "Kiosk / Quest flag", false),
            Self::UseDevKeys => bool_info("Debugging", "use_dev_keys", "Use dev keys", false),
            Self::UseDebugAsserts => bool_info("Debugging", "use_debug_asserts", "Enable debug assertions", false),
            Self::UseAutoStub => bool_info("Debugging", "use_auto_stub", "Auto-stub unimplemented services", false),
            Self::EnableAllControllers => bool_info("Debugging", "enable_all_controllers", "Enable all controller types", false),
            Self::PerformVulkanCheck => bool_info("Debugging", "perform_vulkan_check", "Vulkan startup check", true),
            Self::DisableWebApplet => bool_info("Debugging", "disable_web_applet", "Disable web applet", true),
            Self::DisableMacroJit => bool_info("Debugging", "disable_macro_jit", "Disable GPU macro JIT", false),
            Self::DisableMacroHle => bool_info("Debugging", "disable_macro_hle", "Disable GPU macro HLE", false),
            Self::RendererDebug => bool_info("Renderer", "debug", "GPU debug validation", false),
            Self::RendererShaderFeedback => bool_info("Renderer", "shader_feedback", "Shader feedback", false),
            Self::EnableNsightAftermath => bool_info("Renderer", "nsight_aftermath", "NVIDIA Nsight Aftermath", false),
            Self::DisableShaderLoopSafetyChecks => bool_info("Renderer", "disable_shader_loop_safety_checks", "Disable shader loop safety checks", false),
            Self::EnableRenderdocHotkey => bool_info("Renderer", "renderdoc_hotkey", "RenderDoc capture hotkey", false),
            Self::DisableBufferReorder => bool_info("Renderer", "disable_buffer_reorder", "Disable buffer reorder", false),
            Self::GpuLogLevel => SettingInfo {
                section: "Debugging",
                key: "gpu_log_level",
                label: "GPU log level",
                default: 0,
                choices: &[
                    ("0", "Off"),
                    ("1", "Errors"),
                    ("2", "Standard"),
                    ("3", "Verbose"),
                    ("4", "All"),
                ],
            },
            Self::GpuLogVulkanCalls => bool_info("Debugging", "gpu_log_vulkan_calls", "GPU log: Vulkan calls", true),
            Self::GpuLogShaderDumps => bool_info("Debugging", "gpu_log_shader_dumps", "GPU log: shader dumps", false),
            Self::GpuLogMemoryTracking => bool_info("Debugging", "gpu_log_memory_tracking", "GPU log: memory tracking", true),
            Self::GpuLogDriverDebug => bool_info("Debugging", "gpu_log_driver_debug", "GPU log: driver debug", true),
            Self::RecordFrameTimes => bool_info("Debugging", "record_frame_times", "Record frame times", false),
        }
    }
}

pub struct EmulatorConfig {
    path: PathBuf,
    ini: Ini,
}

type Overrides = BTreeMap<PathBuf, BTreeMap<EmulatorSetting, String>>;

impl EmulatorConfig {
    pub fn load(dir: &Path) -> Result<Self, String> {
        let path = dir.join("emulator.ini");
        let ini = match read_optional(&path)? {
            Some(ini) => ini,
            None => {
                let mut imported = Ini::default();
                if let Some(qt) = read_optional(&dir.with_file_name("qt-config.ini"))? {
                    for &setting in EmulatorSetting::ALL {
                        let info = setting.info();
                        if info.section == "GPUI" {
                            continue;
                        }
                        if let Some(value) = qt
                            .setting(&info)
                            .filter(|value| info.choices.iter().any(|(code, _)| code == value))
                        {
                            imported.set_setting(&info, value);
                        }
                    }
                }
                replace(&path, imported.as_string().as_bytes())?;
                imported
            }
        };
        Ok(Self { path, ini })
    }

    pub fn value(&self, setting: EmulatorSetting) -> &'static str {
        self.value_index(setting)
            .map_or("Custom", |index| setting.info().choices[index].1)
    }

    pub fn cycle(&mut self, setting: EmulatorSetting, direction: i32) -> Result<(), String> {
        if direction == 0 {
            return Ok(());
        }
        // SDL may have written settings since construction. Preserve every unedited value.
        let mut ini = read_optional(&self.path)?.unwrap_or_default();
        let info = setting.info();
        let index = value_index(&ini, &info).unwrap_or(info.default) as i32;
        let next = (index + direction.signum()).rem_euclid(info.choices.len() as i32) as usize;
        ini.set_setting(&info, info.choices[next].0);
        replace(&self.path, ini.as_string().as_bytes())?;
        self.ini = ini;
        Ok(())
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn game_fullscreen(&self) -> bool {
        self.value_index(EmulatorSetting::GameDisplay) == Some(1)
    }

    pub fn home_key(&self) -> &'static str {
        self.code_value(EmulatorSetting::HomeKey)
    }

    pub fn home_button(&self) -> &'static str {
        self.code_value(EmulatorSetting::HomeButton)
    }

    pub fn user_profile(&self) -> u8 {
        self.code_value(EmulatorSetting::UserProfile)
            .parse()
            .unwrap_or(0)
    }

    fn code_value(&self, setting: EmulatorSetting) -> &'static str {
        let info = setting.info();
        self.value_index(setting)
            .and_then(|index| info.choices.get(index))
            .map_or(info.choices[info.default].0, |(code, _)| *code)
    }

    pub fn game_value(
        &self,
        game: &Path,
        setting: EmulatorSetting,
    ) -> Result<Option<&'static str>, String> {
        Ok(self
            .read_overrides()?
            .get(game)
            .and_then(|settings| settings.get(&setting))
            .map(|value| {
                setting
                    .info()
                    .choices
                    .iter()
                    .find(|(code, _)| code == value)
                    .map_or("Custom", |(_, label)| *label)
            }))
    }

    pub fn cycle_game(
        &self,
        game: &Path,
        setting: EmulatorSetting,
        direction: i32,
    ) -> Result<(), String> {
        if direction == 0 {
            return Ok(());
        }
        let mut overrides = self.read_overrides()?;
        let settings = overrides.entry(game.to_owned()).or_default();
        let info = setting.info();
        let current = settings
            .get(&setting)
            .and_then(|value| info.choices.iter().position(|(code, _)| code == value))
            .or_else(|| self.value_index(setting))
            .unwrap_or(info.default);
        let selected =
            (current as i32 + direction.signum()).rem_euclid(info.choices.len() as i32) as usize;
        settings.insert(setting, info.choices[selected].0.into());
        self.save_overrides(&overrides)
    }

    pub fn clear_game(&self, game: &Path, setting: EmulatorSetting) -> Result<(), String> {
        let mut overrides = self.read_overrides()?;
        if let Some(settings) = overrides.get_mut(game) {
            settings.remove(&setting);
            if settings.is_empty() {
                overrides.remove(game);
            }
            self.save_overrides(&overrides)?;
        }
        Ok(())
    }

    pub fn session_file(&self, game: &Path) -> Result<(bool, Option<tempfile::TempPath>), String> {
        let overrides = self.read_overrides()?;
        let Some(settings) = overrides.get(game) else {
            return Ok((self.game_fullscreen(), None));
        };
        let mut ini = read_optional(&self.path)?.unwrap_or_default();
        for (setting, value) in settings {
            ini.set_setting(&setting.info(), value);
        }
        let fullscreen = value_index(&ini, &EmulatorSetting::GameDisplay.info()) == Some(1);
        let dir = self
            .path
            .parent()
            .ok_or("Config path has no parent directory")?;
        let write_session = || -> io::Result<tempfile::TempPath> {
            let mut file = tempfile::Builder::new()
                .prefix("game-")
                .suffix(".ini")
                .tempfile_in(dir)?;
            file.write_all(ini.as_string().as_bytes())?;
            file.as_file().sync_all()?;
            Ok(file.into_temp_path())
        };
        let file = write_session().map_err(|error| {
            format!("Could not prepare settings for {}: {error}", game.display())
        })?;
        Ok((fullscreen, Some(file)))
    }

    fn override_path(&self) -> PathBuf {
        self.path.with_file_name("game-overrides.json")
    }

    fn read_overrides(&self) -> Result<Overrides, String> {
        let file = self.override_path();
        let overrides: Overrides = match fs::read(&file) {
            Ok(bytes) => serde_json::from_slice(&bytes)
                .map_err(|error| format!("Invalid {}: {error}", file.display()))?,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(BTreeMap::new()),
            Err(error) => return Err(format!("Could not read {}: {error}", file.display())),
        };
        for settings in overrides.values() {
            for (setting, value) in settings {
                if !setting.info().choices.iter().any(|(code, _)| code == value) {
                    return Err(format!("Invalid per-game setting value for {setting:?}"));
                }
            }
        }
        Ok(overrides)
    }

    fn save_overrides(&self, overrides: &Overrides) -> Result<(), String> {
        let content = serde_json::to_vec_pretty(overrides)
            .map_err(|error| format!("Could not encode game settings: {error}"))?;
        replace(&self.override_path(), &content)
    }

    fn value_index(&self, setting: EmulatorSetting) -> Option<usize> {
        value_index(&self.ini, &setting.info())
    }
}

fn value_index(ini: &Ini, info: &SettingInfo) -> Option<usize> {
    match ini.setting(info) {
        Some(value) => info.choices.iter().position(|(code, _)| *code == value),
        None => Some(info.default),
    }
}

fn read_optional(path: &Path) -> Result<Option<Ini>, String> {
    match fs::read_to_string(path) {
        Ok(content) => Ok(Some(Ini(content
            .trim_start_matches('\u{feff}')
            .lines()
            .map(str::to_owned)
            .collect()))),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(format!("Could not read {}: {error}", path.display())),
    }
}

#[derive(Default)]
struct Ini(Vec<String>);

impl Ini {
    fn setting(&self, info: &SettingInfo) -> Option<&str> {
        let default = self.get(info.section, &format!("{}\\default", info.key));
        if info.section != "GPUI" && !default.is_some_and(|value| matches!(value, "false" | "0")) {
            return None;
        }
        self.get(info.section, info.key)
    }

    fn set_setting(&mut self, info: &SettingInfo, value: &str) {
        self.set(info.section, info.key, value);
        if info.section != "GPUI" {
            self.set(info.section, &format!("{}\\default", info.key), "false");
        }
    }

    fn get(&self, section: &str, key: &str) -> Option<&str> {
        let range = self.range(section)?;
        self.0[range]
            .iter()
            .filter_map(|line| line.split_once('='))
            .find_map(|(name, value)| {
                name.trim()
                    .eq_ignore_ascii_case(key)
                    .then(|| value.trim().trim_matches('"'))
            })
    }

    fn set(&mut self, section: &str, key: &str, value: &str) {
        let line = format!("{key}={value}");
        if let Some(range) = self.range(section) {
            let offset = self.0[range.clone()].iter().position(|line| {
                line.split_once('=')
                    .is_some_and(|(name, _)| name.trim().eq_ignore_ascii_case(key))
            });
            if let Some(offset) = offset {
                self.0[range.start + offset] = line;
            } else {
                self.0.insert(range.end, line);
            }
        } else {
            if !self.0.is_empty() {
                self.0.push(String::new());
            }
            self.0.extend([format!("[{section}]"), line]);
        }
    }

    fn range(&self, section: &str) -> Option<std::ops::Range<usize>> {
        let start = self.0.iter().position(|line| {
            line.trim()
                .strip_prefix('[')
                .and_then(|line| line.strip_suffix(']'))
                .is_some_and(|name| name.eq_ignore_ascii_case(section))
        })? + 1;
        let end = self.0[start..]
            .iter()
            .position(|line| line.trim().starts_with('['))
            .map_or(self.0.len(), |end| start + end);
        Some(start..end)
    }

    fn as_string(&self) -> String {
        let mut text = self.0.join("\n");
        text.push('\n');
        text
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;

    #[test]
    fn imports_shared_values_once_and_preserves_both_config_files() {
        let scratch = tempfile::tempdir().unwrap();
        let qt = scratch.path().join("qt-config.ini");
        let source = "[Renderer]\nbackend=0\nbackend\\default=false\nuse_vsync=0\nuse_vsync\\default=true\n[Core]\nuse_multi_core=false\nuse_multi_core\\default=false\n[Cpu]\ncpu_ticks=24000\ncpu_ticks\\default=false\n[Controls]\nplayer_0_a=qt-only\n";
        fs::write(&qt, source).unwrap();
        let dir = scratch.path().join("gpui");
        let mut config = EmulatorConfig::load(&dir).unwrap();
        assert_eq!(config.value(EmulatorSetting::Renderer), "OpenGL");
        assert_eq!(config.value(EmulatorSetting::Vsync), "On");
        assert_eq!(config.value(EmulatorSetting::UseMultiCore), "Off");
        assert_eq!(config.value(EmulatorSetting::CpuTicks), "24000");
        let mut updated = fs::read_to_string(config.path()).unwrap();
        assert!(!updated.contains("qt-only"));
        updated.push_str("\n[Unknown]\nvalue=preserved\n");
        fs::write(config.path(), updated).unwrap();
        config.cycle(EmulatorSetting::Renderer, -1).unwrap();
        assert_eq!(
            EmulatorConfig::load(&dir)
                .unwrap()
                .value(EmulatorSetting::Renderer),
            "Vulkan"
        );
        assert!(fs::read_to_string(config.path())
            .unwrap()
            .contains("value=preserved"));
        assert_eq!(fs::read_to_string(&qt).unwrap(), source);
    }

    #[test]
    fn title_override_only_changes_the_owned_session_configuration() {
        let scratch = tempfile::tempdir().unwrap();
        let config = EmulatorConfig::load(scratch.path()).unwrap();
        let game = Path::new("synthetic.nro");
        assert!(config.session_file(game).unwrap().1.is_none());
        config
            .cycle_game(game, EmulatorSetting::GameDisplay, 1)
            .unwrap();
        config.cycle_game(game, EmulatorSetting::Vsync, -1).unwrap();
        let (fullscreen, file) = config.session_file(game).unwrap();
        let file = file.unwrap();
        assert!(fullscreen);
        assert_eq!(
            config.game_value(game, EmulatorSetting::Vsync).unwrap(),
            Some("Mailbox")
        );
        assert_eq!(config.value(EmulatorSetting::Vsync), "On");
        let session = read_optional(&file).unwrap().unwrap();
        assert_eq!(session.setting(&EmulatorSetting::Vsync.info()), Some("1"));
        config.clear_game(game, EmulatorSetting::Vsync).unwrap();
        assert_eq!(
            config.game_value(game, EmulatorSetting::Vsync).unwrap(),
            None
        );
    }

    #[test]
    fn home_fallback_settings_persist_and_keep_defaults() {
        let scratch = tempfile::tempdir().unwrap();
        let mut config = EmulatorConfig::load(scratch.path()).unwrap();
        assert_eq!(config.home_key(), "F10");
        assert_eq!(config.home_button(), "guide");
        config.cycle(EmulatorSetting::HomeKey, 1).unwrap();
        config.cycle(EmulatorSetting::HomeButton, 1).unwrap();
        let reloaded = EmulatorConfig::load(scratch.path()).unwrap();
        assert_eq!(reloaded.home_key(), "F9");
        assert_eq!(reloaded.home_button(), "back");
        assert_eq!(
            reloaded.value(EmulatorSetting::HomeButton),
            "Back / Select"
        );
    }

    #[test]
    fn user_profile_defaults_to_zero_and_persists() {
        let scratch = tempfile::tempdir().unwrap();
        let mut config = EmulatorConfig::load(scratch.path()).unwrap();
        assert_eq!(config.user_profile(), 0);
        config.cycle(EmulatorSetting::UserProfile, 2).unwrap();
        let reloaded = EmulatorConfig::load(scratch.path()).unwrap();
        assert_eq!(reloaded.user_profile(), 1);
        assert_eq!(reloaded.value(EmulatorSetting::UserProfile), "Profile 2");
    }

    #[test]
    fn all_settings_have_valid_metadata_and_round_trip() {
        let scratch = tempfile::tempdir().unwrap();
        let mut config = EmulatorConfig::load(scratch.path()).unwrap();
        let mut keys = BTreeSet::new();
        for &setting in EmulatorSetting::ALL {
            let info = setting.info();
            assert!(!info.section.is_empty());
            assert!(!info.key.is_empty());
            assert!(!info.label.is_empty());
            assert!(info.choices.len() >= 2);
            assert!(info.default < info.choices.len());
            assert!(
                keys.insert((info.section, info.key)),
                "Duplicate setting key {}.{}",
                info.section,
                info.key
            );
            let initial = config.value(setting);
            config.cycle(setting, 1).unwrap();
            let cycled = config.value(setting);
            assert_ne!(cycled, initial, "Setting {setting:?} did not advance");
            assert_eq!(EmulatorConfig::load(scratch.path()).unwrap().value(setting), cycled);
        }
    }

    #[test]
    fn special_sections_serialize_with_default_false() {
        let scratch = tempfile::tempdir().unwrap();
        let mut config = EmulatorConfig::load(scratch.path()).unwrap();
        for setting in [
            EmulatorSetting::AirplaneMode,
            EmulatorSetting::UseVirtualSd,
            EmulatorSetting::ConsoleMode,
            EmulatorSetting::SoundMode,
            EmulatorSetting::CabinetAppletMode,
            EmulatorSetting::UseGdbstub,
            EmulatorSetting::LogFlushLine,
        ] {
            config.cycle(setting, 1).unwrap();
        }
        let ini_text = fs::read_to_string(config.path()).unwrap();
        assert!(ini_text.contains("[Services]"));
        assert!(ini_text.contains("airplane_mode=false"));
        assert!(ini_text.contains("airplane_mode\\default=false"));
        assert!(ini_text.contains("[Data%20Storage]"));
        assert!(ini_text.contains("use_virtual_sd=false"));
        assert!(ini_text.contains("[System]"));
        assert!(ini_text.contains("use_docked_mode=1"));
        assert!(ini_text.contains("sound_index=2"));
        assert!(ini_text.contains("[LibraryApplet]"));
        assert!(ini_text.contains("cabinet_applet_mode=2"));
        assert!(ini_text.contains("[Debugging]"));
        assert!(ini_text.contains("use_gdbstub=true"));
        assert!(ini_text.contains("[Miscellaneous]"));
        assert!(ini_text.contains("flush_line=true"));
    }
}
