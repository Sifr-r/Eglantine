# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(gba_core STATIC
    apu.cpp
    apu.h
    bios.cpp
    bios.h
    bus.cpp
    bus.h
    cartridge.cpp
    cartridge.h
    cheats.cpp
    cheats.h
    cpu.cpp
    cpu.h
    cpu_arm.cpp
    cpu_regs.cpp
    cpu_regs.h
    cpu_thumb.cpp
    dma.cpp
    dma.h
    emulator.cpp
    emulator.h
    ppu.cpp
    ppu.h
    rewind.cpp
    rewind.h
    savestate.cpp
    savestate.h
    timers.cpp
    timers.h
    types.h
)

target_include_directories(gba_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
create_target_directory_groups(gba_core)
