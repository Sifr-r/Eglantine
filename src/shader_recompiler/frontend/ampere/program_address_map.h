// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <span>
#include <vector>
#include "common/common_types.h"

#ifdef EGLANTINE_ENABLE_RUST_AMPERE
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include "eglantine_ampere.h"
#endif

namespace Shader::Ampere {
constexpr u64 NativeAddressMapMagic = 0x50414d4e4544454eULL;
inline bool HasNativeAddressMapCpp(std::span<const u64> code) {
    return code.size() >= 2 && code.back() == NativeAddressMapMagic;
}
inline void AppendNativeAddressMap(std::vector<u64>& code, std::span<const u32> offsets) {
    if (offsets.empty()) return;
    for (u32 index : offsets) code.push_back(0x50U + index * 8 + ((index + 1) / 3) * 8);
    code.push_back(offsets.size());
    code.push_back(NativeAddressMapMagic);
}
inline std::optional<u32> NativeProgramAddressCpp(std::span<const u64> code, u32 address) {
    if (!HasNativeAddressMapCpp(code) || address % 16) return {};
    const u64 count = code[code.size() - 2];
    if (!count || count > 65536 || count + 2 > code.size() || address / 16 >= count) return {};
    const size_t start = code.size() - static_cast<size_t>(count) - 2;
    const u64 target = code[start + address / 16];
    if (target < 0x50 || target >= start * 8 || target % 8 || target % 32 == 0) return {};
    return static_cast<u32>(target);
}

#ifdef EGLANTINE_ENABLE_RUST_AMPERE
namespace detail {
inline bool AddressMapEnvEnabled(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && std::string_view{value} == "1";
}
} // namespace detail

inline bool HasNativeAddressMapRust(std::span<const u64> code) {
    return egl_ampere_has_native_address_map(code.empty() ? nullptr : code.data(), code.size()) !=
           0;
}

inline std::optional<u32> NativeProgramAddressRust(std::span<const u64> code, u32 address) {
    u32 target = 0;
    if (egl_ampere_native_program_address(code.empty() ? nullptr : code.data(), code.size(),
                                          address, &target) != 0) {
        return target;
    }
    return std::nullopt;
}

inline bool HasNativeAddressMap(std::span<const u64> code) {
    if (detail::AddressMapEnvEnabled("EDEN_NX2_RUST_AMPERE_VERIFY")) {
        const bool cpp_has = HasNativeAddressMapCpp(code);
        const bool rust_has = HasNativeAddressMapRust(code);
        if (cpp_has != rust_has) {
            std::fprintf(stderr,
                         "[EDEN_NX2_RUST_AMPERE_VERIFY] HasNativeAddressMap mismatch (len=%zu): "
                         "cpp=%d rust=%d\n",
                         code.size(), cpp_has ? 1 : 0, rust_has ? 1 : 0);
            std::fflush(stderr);
            std::abort();
        }
        return cpp_has;
    }
    if (detail::AddressMapEnvEnabled("EDEN_NX2_RUST_AMPERE")) {
        return HasNativeAddressMapRust(code);
    }
    return HasNativeAddressMapCpp(code);
}

inline std::optional<u32> NativeProgramAddress(std::span<const u64> code, u32 address) {
    if (detail::AddressMapEnvEnabled("EDEN_NX2_RUST_AMPERE_VERIFY")) {
        const std::optional<u32> cpp_addr = NativeProgramAddressCpp(code, address);
        const std::optional<u32> rust_addr = NativeProgramAddressRust(code, address);
        if (cpp_addr != rust_addr) {
            std::fprintf(stderr,
                         "[EDEN_NX2_RUST_AMPERE_VERIFY] NativeProgramAddress mismatch "
                         "(address=%#x): cpp=%d/%#x rust=%d/%#x\n",
                         static_cast<unsigned>(address), cpp_addr.has_value() ? 1 : 0,
                         static_cast<unsigned>(cpp_addr.value_or(0)),
                         rust_addr.has_value() ? 1 : 0,
                         static_cast<unsigned>(rust_addr.value_or(0)));
            std::fflush(stderr);
            std::abort();
        }
        return cpp_addr;
    }
    if (detail::AddressMapEnvEnabled("EDEN_NX2_RUST_AMPERE")) {
        return NativeProgramAddressRust(code, address);
    }
    return NativeProgramAddressCpp(code, address);
}
#else
inline bool HasNativeAddressMap(std::span<const u64> code) {
    return HasNativeAddressMapCpp(code);
}
inline std::optional<u32> NativeProgramAddress(std::span<const u64> code, u32 address) {
    return NativeProgramAddressCpp(code, address);
}
#endif
} // namespace Shader::Ampere
