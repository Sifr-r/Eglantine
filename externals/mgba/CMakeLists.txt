# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

AddJsonPackage(NAME mgba DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)

set(LIBMGBA_ONLY ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${mgba_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/upstream" EXCLUDE_FROM_ALL)

# The host owns command-line parsing. Upstream's MSVC getopt globals collide with ours.
get_target_property(mgba_sources mgba SOURCES)
list(FILTER mgba_sources EXCLUDE REGEX "(^|/)commandline[.]c$")
set_property(TARGET mgba PROPERTY SOURCES "${mgba_sources}")
# Keep the bundled CRC fallback private from the main program's zlib.
target_compile_definitions(mgba PRIVATE crc32=eglantine_mgba_crc32)

set(mgba_notices "${PROJECT_BINARY_DIR}/classic-notices")
file(MAKE_DIRECTORY "${mgba_notices}")
configure_file("${mgba_SOURCE_DIR}/LICENSE" "${mgba_notices}/mGBA-MPL-2.0.txt" COPYONLY)
configure_file("${mgba_SOURCE_DIR}/src/third-party/blip_buf/license.txt"
               "${mgba_notices}/blip_buf-LGPL-2.1.txt" COPYONLY)
configure_file("${mgba_SOURCE_DIR}/src/third-party/inih/LICENSE.txt"
               "${mgba_notices}/inih-BSD-3-Clause.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/dist/classic_runtime_sources.txt"
               "${mgba_notices}/classic_runtime_sources.txt" COPYONLY)
