// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <limits>

#include "common/bit_field.h"
#include "common/common_funcs.h"
#include "common/elf.h"
#include "core/arm/symbols.h"
#include "core/core.h"
#include "core/memory.h"

using namespace Common::ELF;

namespace Core {
namespace Symbols {

struct ModuleHeaderLocation {
    u32 version;
    u32 header_offset;
    u32 version_offset;
};
static_assert(sizeof(ModuleHeaderLocation) == 0x0C);
struct ModuleHeader {
    u32 signature;
    u32 dynamic_offset;
    u32 bss_start_offset;
    u32 bss_end_offset;
    u32 exception_info_start_offset;
    u32 exception_info_end_offset;
    u32 module_offset;
    u32 relro_start_offset;
    u32 full_relro_end_offset;
    u32 nx_debug_link_start_offset;
    u32 nx_debug_link_end_offset;
    u32 note_gnu_build_id_start_offset;
    u32 note_gnu_build_id_end_offset;
};
static_assert(sizeof(ModuleHeader) == 0x34);

struct Mod32 {
    using Sym = Elf32_Sym;
    using Dyn = Elf32_Dyn;
};
struct Mod64 {
    using Sym = Elf64_Sym;
    using Dyn = Elf64_Dyn;
};

template <typename M, typename F>
static Symbols GetSymbols(F&& ReadBytes) {
    const auto Read8 = [&](u64 index, u8& out_byte) {
        return ReadBytes(&out_byte, index, sizeof(u8));
    };
    ModuleHeaderLocation loc{};
    if (!ReadBytes(&loc, 0, sizeof(ModuleHeaderLocation))) {
        return {};
    }
    ModuleHeader hdr{};
    if (!ReadBytes(&hdr, loc.header_offset, sizeof(ModuleHeader))) {
        return {};
    }
    if (hdr.signature != Common::MakeMagic('M', 'O', 'D', '0')) {
        return {};
    }
    if (u64{loc.header_offset} > std::numeric_limits<u64>::max() - hdr.dynamic_offset) {
        return {};
    }

    VAddr strtab_offs{};
    VAddr symtab_offs{};
    u64 strtab_size{};
    u64 syment_size = sizeof(typename M::Sym);
    VAddr dynamic_offset = u64{loc.header_offset} + hdr.dynamic_offset;

    constexpr size_t MaxDynamicEntries = 4096;
    bool found_null = false;
    for (size_t i = 0; i < MaxDynamicEntries; ++i) {
        typename M::Dyn dyn{};
        if (!ReadBytes(&dyn, dynamic_offset, sizeof(typename M::Dyn))) {
            return {};
        }
        if (dynamic_offset > std::numeric_limits<u64>::max() - sizeof(typename M::Dyn)) {
            return {};
        }
        dynamic_offset += sizeof(typename M::Dyn);
        if (dyn.d_tag == ElfDtNull) {
            found_null = true;
            break;
        }
        if (dyn.d_tag == ElfDtStrtab) {
            strtab_offs = dyn.d_un.d_ptr;
        } else if (dyn.d_tag == ElfDtSymtab) {
            symtab_offs = dyn.d_un.d_ptr;
        } else if (dyn.d_tag == ElfDtStrsz) {
            strtab_size = dyn.d_un.d_val;
        } else if (dyn.d_tag == ElfDtSyment) {
            syment_size = dyn.d_un.d_val;
        }
    }
    if (!found_null || syment_size < sizeof(typename M::Sym)) {
        return {};
    }
    if (strtab_offs > 0 && symtab_offs > 0 && symtab_offs < strtab_offs &&
        strtab_offs >= sizeof(typename M::Sym)) {
        constexpr size_t MaxSymbolNameLength = 4096;
        const u64 str_end =
            strtab_size > 0 && strtab_offs <= std::numeric_limits<u64>::max() - strtab_size
                ? strtab_offs + strtab_size
                : std::numeric_limits<u64>::max();
        Symbols out;
        VAddr symbol_index = symtab_offs;
        while (symbol_index <= strtab_offs - sizeof(typename M::Sym)) {
            typename M::Sym symbol{};
            if (!ReadBytes(&symbol, symbol_index, sizeof(typename M::Sym))) {
                break;
            }
            if ((strtab_size == 0 || symbol.st_name < strtab_size) &&
                strtab_offs <= std::numeric_limits<u64>::max() - symbol.st_name) {
                VAddr offs = strtab_offs + symbol.st_name;
                std::string name{};
                u8 c = 0;
                bool terminated = false;
                while (offs < str_end && name.size() < MaxSymbolNameLength && Read8(offs, c)) {
                    if (c == 0) {
                        terminated = true;
                        break;
                    }
                    name += static_cast<char>(c);
                    if (offs == std::numeric_limits<u64>::max()) {
                        break;
                    }
                    ++offs;
                }
                if (terminated) {
                    out[name] = std::make_pair(symbol.st_value, symbol.st_size);
                }
            }
            if (symbol_index > std::numeric_limits<u64>::max() - syment_size) {
                break;
            }
            symbol_index += syment_size;
        }
        return out;
    }
    return {};
}

Symbols GetSymbols(VAddr base, Core::Memory::Memory& memory, bool is_64) {
    const auto f = [base, &memory](void* ptr, size_t offset, size_t size) -> bool {
        if (base > std::numeric_limits<VAddr>::max() - offset) {
            return false;
        }
        return memory.ReadBlock(base + offset, ptr, size);
    };
    return is_64 ? GetSymbols<Mod64>(f) : GetSymbols<Mod32>(f);
}

Symbols GetSymbols(std::span<const u8> data, bool is_64) {
    const auto f = [data](void* ptr, size_t offset, size_t size) -> bool {
        if (offset > data.size() || size > data.size() - offset) {
            return false;
        }
        if (size > 0) {
            std::memcpy(ptr, data.data() + offset, size);
        }
        return true;
    };
    return is_64 ? GetSymbols<Mod64>(f) : GetSymbols<Mod32>(f);
}

std::optional<std::string> GetSymbolName(const Symbols& symbols, VAddr addr) {
    const auto it = std::find_if(symbols.cbegin(), symbols.cend(), [addr](const auto& e) {
        auto const [start, size] = e.second;
        auto const end = start + size;
        return addr >= start && addr < end;
    });
    return it != symbols.cend() ? std::optional<std::string>{it->first} : std::nullopt;
}

} // namespace Symbols
} // namespace Core
