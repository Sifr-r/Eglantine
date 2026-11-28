# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(classic_cartridge STATIC
    cartridge.cpp
    cartridge.h
)

create_target_directory_groups(classic_cartridge)

if(ENABLE_CLASSIC_RUNTIME)
    add_library(classic_runtime STATIC
        battery_save.cpp
        battery_save.h
        runtime.cpp
        runtime.h
    )
    target_link_libraries(classic_runtime PRIVATE classic_cartridge mgba gba_core)
    target_compile_definitions(classic_runtime PRIVATE BUILD_STATIC)
    create_target_directory_groups(classic_runtime)
endif()
