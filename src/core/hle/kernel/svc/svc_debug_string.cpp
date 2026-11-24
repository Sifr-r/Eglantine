// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_results.h"
#include "core/memory.h"

namespace Kernel::Svc {

/// Used to output a message on a debug hardware unit - does nothing on a retail unit
Result OutputDebugString(Core::System& system, u64 address, u64 len) {
    R_SUCCEED_IF(len == 0);
    R_UNLESS(address < address + len, ResultInvalidCurrentMemory);
    R_UNLESS(GetCurrentProcess(system.Kernel()).GetPageTable().Contains(address, len),
             ResultInvalidCurrentMemory);

    constexpr u64 MaxDebugStringSize = 64 * 1024;
    const std::size_t read_len = static_cast<std::size_t>((std::min)(len, MaxDebugStringSize));
    std::string msg_buffer(read_len, 0);
    GetCurrentMemory(system.Kernel()).ReadBlock(address, msg_buffer.data(), read_len);
    if (len > MaxDebugStringSize) {
        LOG_INFO(Debug_Emulated, "{}... [truncated {} bytes]", msg_buffer, len - read_len);
    } else {
        LOG_INFO(Debug_Emulated, "{}", msg_buffer);
    }
    R_SUCCEED();
}

Result OutputDebugString64(Core::System& system, uint64_t debug_str, uint64_t len) {
    R_RETURN(OutputDebugString(system, debug_str, len));
}

Result OutputDebugString64From32(Core::System& system, uint32_t debug_str, uint32_t len) {
    R_RETURN(OutputDebugString(system, debug_str, len));
}

} // namespace Kernel::Svc
