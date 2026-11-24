// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <span>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"

namespace AudioCore {
/**
 * Responsible for allocating up a workbuffer into multiple pieces.
 * Takes in a buffer and size (it does not own them), and allocates up the buffer via Allocate.
 */
class WorkbufferAllocator {
public:
    explicit WorkbufferAllocator(std::span<u8> buffer_, u64 size_)
        : buffer{reinterpret_cast<u64>(buffer_.data())}, size{size_} {}

    /**
     * Allocate the given count of T elements, aligned to alignment.
     *
     * @param count     - The number of elements to allocate.
     * @param alignment - The required starting alignment.
     * @return Non-owning container of allocated elements.
     */
    template <typename T>
    std::span<T> Allocate(u64 count, u64 alignment) {
        if (count == 0) {
            return {};
        }

        constexpr u64 max_count = (std::min<u64>)(
            (std::numeric_limits<u64>::max)() / sizeof(T),
            (std::numeric_limits<std::size_t>::max)());
        if (count > max_count) {
            LOG_ERROR(Service_Audio,
                      "Allocation count overflow.\nAllocator size={:08X}, offset={:08X}.\n"
                      "Attempting to allocate {} elements of size {:08X} with alignment={:02X}",
                      size, offset, count, sizeof(T), alignment);
            return {};
        }

        const u64 byte_size{count * sizeof(T)};
        const u64 pad = AlignmentPadding(alignment);
        const u64 remaining = size - offset;

        if (pad <= remaining && byte_size <= remaining - pad) {
            const u64 out = buffer + offset + pad;
            offset += pad + byte_size;
            return std::span<T>(reinterpret_cast<T*>(out), static_cast<std::size_t>(count));
        }

        LOG_ERROR(Service_Audio,
                  "Allocated buffer was too small to hold new alloc.\nAllocator size={:08X}, "
                  "offset={:08X}.\nAttempting to allocate {:08X} with alignment={:02X}",
                  size, offset, byte_size, alignment);
        return {};
    }

    /**
     * Align the current offset to the given alignment.
     *
     * @param alignment - The required starting alignment.
     */
    void Align(u64 alignment) {
        const u64 pad = AlignmentPadding(alignment);
        const u64 remaining = size - offset;
        offset = pad > remaining ? size : offset + pad;
    }

    /**
     * Get the current buffer offset.
     *
     * @return The current allocating offset.
     */
    u64 GetCurrentOffset() const {
        return offset;
    }

    /**
     * Get the current buffer size.
     *
     * @return The size of the current buffer.
     */
    u64 GetSize() const {
        return size;
    }

    /**
     * Get the remaining size that can be allocated.
     *
     * @return The remaining size left in the buffer.
     */
    u64 GetRemainingSize() const {
        return size - offset;
    }

private:
    u64 AlignmentPadding(u64 alignment) const noexcept {
        const u64 align = alignment > 0 ? alignment : 1;
        const u64 current = buffer + offset;
        const u64 mod = align > 1 ? (current % align) : 0;
        return mod == 0 ? 0 : (align - mod);
    }

    /// The buffer into which we are allocating.
    u64 buffer;
    /// Size of the buffer we're allocating to.
    u64 size;
    /// Current offset into the buffer, an error will be thrown if it exceeds size.
    u64 offset{};
};

} // namespace AudioCore
