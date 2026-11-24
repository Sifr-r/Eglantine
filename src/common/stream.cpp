// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdexcept>
#include "common/common_types.h"
#include "common/stream.h"

namespace Common {

Stream::Stream() = default;
Stream::~Stream() = default;

void Stream::Seek(s32 offset, SeekOrigin origin) {
    if (origin == SeekOrigin::SetOrigin) {
        if (offset < 0) {
            position = 0;
        } else if (static_cast<std::size_t>(offset) >= buffer.size()) {
            position = buffer.size();
        } else {
            position = static_cast<std::size_t>(offset);
        }
    } else if (origin == SeekOrigin::FromCurrentPos) {
        const std::size_t current_pos = position <= buffer.size() ? position : buffer.size();
        if (offset < 0) {
            const std::size_t delta = static_cast<std::size_t>(-static_cast<s64>(offset));
            position = (delta > current_pos) ? 0 : (current_pos - delta);
        } else {
            const std::size_t delta = static_cast<std::size_t>(offset);
            position =
                (delta > buffer.size() - current_pos) ? buffer.size() : (current_pos + delta);
        }
    } else if (origin == SeekOrigin::FromEnd) {
        if (offset <= 0) {
            position = buffer.size();
        } else {
            const std::size_t delta = static_cast<std::size_t>(offset);
            position = (delta > buffer.size()) ? 0 : (buffer.size() - delta);
        }
    }
}

u8 Stream::ReadByte() {
    if (position < buffer.size()) {
        return buffer[position++];
    } else {
        throw std::out_of_range("Attempting to read a byte not within the buffer range");
    }
}

void Stream::WriteByte(u8 byte) {
    if (position >= buffer.size()) {
        position = buffer.size();
        buffer.push_back(byte);
        position++;
    } else {
        buffer[position++] = byte;
    }
}

} // namespace Common
