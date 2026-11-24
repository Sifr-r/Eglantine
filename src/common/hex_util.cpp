// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/hex_util.h"

namespace Common {

std::vector<u8> HexStringToVector(std::string_view str, bool little_endian) {
    const std::size_t count = str.size() / 2;
    std::vector<u8> out(count);
    if (count == 0) {
        return out;
    }
    for (std::size_t idx = 0; idx < count; ++idx) {
        const std::size_t str_idx = idx * 2;
        const u8 byte =
            static_cast<u8>((ToHexNibble(str[str_idx]) << 4) | ToHexNibble(str[str_idx + 1]));
        out[little_endian ? (count - 1 - idx) : idx] = byte;
    }
    return out;
}

} // namespace Common
