// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <limits>
#include <memory>

#include "common/common_types.h"
#include "core/device_memory_manager.h"

namespace Tegra::Host1x {
class Host1x;
} // namespace Tegra::Host1x

namespace Service::Nvidia::NvCore {

class HeapMapper {
public:
    HeapMapper(VAddr start_vaddress, DAddr start_daddress, size_t size, Core::Asid asid,
               Tegra::Host1x::Host1x& host1x);
    ~HeapMapper();

    bool IsInBounds(VAddr start, size_t size) const {
        return RangeInBounds(start, size, m_vaddress, m_size);
    }

    /// Whether [start, start + size) lies inside [base, base + region_size). A size that makes
    /// the end of the range wrap below its start is not inside anything.
    static bool RangeInBounds(VAddr start, size_t size, VAddr base, size_t region_size) {
        if (size > std::numeric_limits<VAddr>::max() - start) {
            return false;
        }
        const VAddr end = start + size;
        return start >= base && end <= (base + region_size);
    }

    DAddr Map(VAddr start, size_t size);

    void Unmap(VAddr start, size_t size);

    DAddr GetRegionStart() const {
        return m_daddress;
    }

    size_t GetRegionSize() const {
        return m_size;
    }

private:
    struct HeapMapperInternal;
    VAddr m_vaddress;
    DAddr m_daddress;
    size_t m_size;
    Core::Asid m_asid;
    std::unique_ptr<HeapMapperInternal> m_internal;
};

} // namespace Service::Nvidia::NvCore
