// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <sysinfoapi.h>
// clang-format on
#else
#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <unistd.h>
#endif
#endif

#include "common/memory_detect.h"

namespace Common {

// Detects the RAM and Swapfile sizes
static MemoryInfo Detect() {
    MemoryInfo mem_info{};

#ifdef _WIN32
    MEMORYSTATUSEX memorystatus{};
    memorystatus.dwLength = sizeof(memorystatus);
    if (GlobalMemoryStatusEx(&memorystatus) != FALSE) {
        mem_info.TotalPhysicalMemory = memorystatus.ullTotalPhys;
        mem_info.TotalSwapMemory =
            memorystatus.ullTotalPageFile > memorystatus.ullTotalPhys
                ? memorystatus.ullTotalPageFile - memorystatus.ullTotalPhys
                : 0;
    }
#elif defined(__APPLE__)
    u64 ramsize{};
    struct xsw_usage vmusage{};
    std::size_t sizeof_ramsize = sizeof(ramsize);
    std::size_t sizeof_vmusage = sizeof(vmusage);
    // hw and vm are defined in sysctl.h
    // https://github.com/apple/darwin-xnu/blob/master/bsd/sys/sysctl.h#L471
    // sysctlbyname(const char *, void *, size_t *, void *, size_t);
    if (sysctlbyname("hw.memsize", &ramsize, &sizeof_ramsize, nullptr, 0) == 0) {
        mem_info.TotalPhysicalMemory = ramsize;
    }
    if (sysctlbyname("vm.swapusage", &vmusage, &sizeof_vmusage, nullptr, 0) == 0) {
        mem_info.TotalSwapMemory = vmusage.xsu_total;
    }
#elif defined(__FreeBSD__)
    u_long physmem{};
    u_long swap_total{};
    std::size_t sizeof_physmem = sizeof(physmem);
    std::size_t sizeof_swap_total = sizeof(swap_total);
    // sysctlbyname(const char *, void *, size_t *, const void *, size_t);
    if (sysctlbyname("hw.physmem", &physmem, &sizeof_physmem, nullptr, 0) == 0) {
        mem_info.TotalPhysicalMemory = physmem;
    }
    if (sysctlbyname("vm.swap_total", &swap_total, &sizeof_swap_total, nullptr, 0) == 0) {
        mem_info.TotalSwapMemory = swap_total;
    }
#elif defined(__linux__)
    struct sysinfo meminfo{};
    if (sysinfo(&meminfo) == 0) {
        const u64 mem_unit = meminfo.mem_unit > 0 ? static_cast<u64>(meminfo.mem_unit) : 1ULL;
        mem_info.TotalPhysicalMemory = static_cast<u64>(meminfo.totalram) * mem_unit;
        mem_info.TotalSwapMemory = static_cast<u64>(meminfo.totalswap) * mem_unit;
    }
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        mem_info.TotalPhysicalMemory = static_cast<u64>(pages) * static_cast<u64>(page_size);
    }
    mem_info.TotalSwapMemory = 0;
#endif

    return mem_info;
}

const MemoryInfo& GetMemInfo() {
    static MemoryInfo mem_info = Detect();
    return mem_info;
}

} // namespace Common
