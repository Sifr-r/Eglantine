// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include "common/settings.h"
#include "common/random.h"
#include "core/file_sys/kernel_executable.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/kip.h"
#include "core/memory.h"

namespace Loader {

namespace {
constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}
} // Anonymous namespace

AppLoader_KIP::AppLoader_KIP(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), kip(std::make_unique<FileSys::KIP>(file)) {}

AppLoader_KIP::~AppLoader_KIP() = default;

FileType AppLoader_KIP::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32_le magic{};
    if (in_file->GetSize() < sizeof(u32) || in_file->ReadObject(&magic) != sizeof(u32)) {
        return FileType::Error;
    }

    if (magic == Common::MakeMagic('K', 'I', 'P', '1')) {
        return FileType::KIP;
    }

    return FileType::Error;
}

FileType AppLoader_KIP::GetFileType() const {
    return (kip != nullptr && kip->GetStatus() == ResultStatus::Success) ? FileType::KIP
                                                                         : FileType::Error;
}

AppLoader::LoadResult AppLoader_KIP::Load(Kernel::KProcess& process,
                                          [[maybe_unused]] Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (kip == nullptr) {
        return {ResultStatus::ErrorNullFile, {}};
    }

    if (kip->GetStatus() != ResultStatus::Success) {
        return {kip->GetStatus(), {}};
    }

    const auto get_kip_address_space_type = [](const auto& kip_type) {
        return kip_type.Is64Bit()
                   ? (kip_type.Is39BitAddressSpace() ? FileSys::ProgramAddressSpaceType::Is39Bit
                                                     : FileSys::ProgramAddressSpaceType::Is36Bit)
                   : FileSys::ProgramAddressSpaceType::Is32Bit;
    };

    const auto address_space = get_kip_address_space_type(*kip);

    FileSys::ProgramMetadata metadata;
    metadata.LoadManual(kip->Is64Bit(), address_space, kip->GetMainThreadPriority(),
                        kip->GetMainThreadCpuCore(), kip->GetMainThreadStackSize(),
                        kip->GetTitleID(), 0xFFFFFFFFFFFFFFFF, 0, kip->GetKernelCapabilities());

    Kernel::CodeSet codeset;
    constexpr u64 max_u32_aligned =
        (std::numeric_limits<u32>::max)() & ~Core::Memory::YUZU_PAGEMASK;
    const auto page_align_u64 = [max_u32_aligned](u64 sz) -> std::optional<u64> {
        if (sz > max_u32_aligned) {
            return std::nullopt;
        }
        return (sz + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK;
    };
    const auto segment_end = [&](u32 seg_offset, u64 raw_size,
                                 u64 extra_size = 0) -> std::optional<u64> {
        const auto aligned = page_align_u64(raw_size);
        if (!aligned || *aligned > max_u32_aligned - extra_size) {
            return std::nullopt;
        }
        const u64 total_seg_size = *aligned + extra_size;
        if (u64{seg_offset} > max_u32_aligned - total_seg_size) {
            return std::nullopt;
        }
        return u64{seg_offset} + total_seg_size;
    };

    const auto text = kip->GetTextSection();
    const auto rodata = kip->GetRODataSection();
    const auto data = kip->GetDataSection();
    const u32 text_offset = kip->GetTextOffset();
    const u32 rodata_offset = kip->GetRODataOffset();
    const u32 data_offset = kip->GetDataOffset();
    const u32 bss_offset = kip->GetBSSOffset();
    const u32 bss_size = kip->GetBSSSize();

    const auto text_end = segment_end(text_offset, text.size());
    const auto rodata_end = segment_end(rodata_offset, rodata.size());
    const auto data_end = segment_end(data_offset, data.size(), bss_size);
    const auto bss_aligned = page_align_u64(bss_offset);
    if (!text_end || !rodata_end || !data_end || !bss_aligned ||
        *bss_aligned > max_u32_aligned - bss_size) {
        return {ResultStatus::ErrorBadKIPHeader, {}};
    }

    const u64 memory_size =
        (std::max)({*text_end, *rodata_end, *data_end, *bss_aligned + bss_size});
    if (memory_size > codeset.memory.max_size()) {
        return {ResultStatus::ErrorBadKIPHeader, {}};
    }
    try {
        codeset.memory.resize(static_cast<std::size_t>(memory_size));
    } catch (const std::bad_alloc&) {
        return {ResultStatus::ErrorNotInitialized, {}};
    }

    const auto load_segment = [&codeset](Kernel::CodeSet::Segment& segment,
                                         std::span<const u8> seg_data, u32 seg_offset) -> bool {
        if (seg_offset > codeset.memory.size() ||
            seg_data.size() > codeset.memory.size() - seg_offset) {
            return false;
        }
        segment.addr = seg_offset;
        segment.offset = seg_offset;
        segment.size = PageAlignSize(static_cast<u32>(seg_data.size()));
        if (!seg_data.empty()) {
            std::memcpy(codeset.memory.data() + seg_offset, seg_data.data(), seg_data.size());
        }
        return true;
    };
    if (!load_segment(codeset.CodeSegment(), text, text_offset) ||
        !load_segment(codeset.RODataSegment(), rodata, rodata_offset) ||
        !load_segment(codeset.DataSegment(), data, data_offset)) {
        return {ResultStatus::ErrorBadKIPHeader, {}};
    }
    codeset.DataSegment().size += bss_size;

    // TODO: this is bad form of ASLR, it sucks
    std::uintptr_t aslr_offset = ((::Settings::values.rng_seed_enabled.GetValue()
        ? ::Settings::values.rng_seed.GetValue() : Common::Random::Random64(0)) << 12) & 0xfff000;

    // Setup the process code layout
    if (process.LoadFromMetadata(system.Kernel(), metadata, codeset.memory.size(), 0, aslr_offset).IsError()) {
        return {ResultStatus::ErrorNotInitialized, {}};
    }
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    process.LoadModule(system.Kernel(), std::move(codeset), base_address);

    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", kip->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success,
            LoadParameters{kip->GetMainThreadPriority(), kip->GetMainThreadStackSize()}};
}

} // namespace Loader
