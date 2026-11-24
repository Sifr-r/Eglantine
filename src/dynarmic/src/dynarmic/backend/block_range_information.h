// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include <boost/icl/interval_set.hpp>
#include "common/container/unordered_set.h"

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename P>
[[nodiscard]] boost::icl::discrete_interval<P> MakeClosedCacheRange(P start_address, std::size_t length) {
    static_assert(std::is_unsigned_v<P>);
    if (length == 0) {
        return {};
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(length) - 1;
    const P max_addr = (std::numeric_limits<P>::max)();
    const std::uint64_t max_offset = static_cast<std::uint64_t>(max_addr - start_address);
    const P end_address = offset > max_offset ? max_addr : static_cast<P>(start_address + offset);
    return boost::icl::discrete_interval<P>::closed(start_address, end_address);
}

template<typename P>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location);
    void ClearCache();
    ::Common::unordered_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<P>& invalidated);

private:
    /// The guest code [first, last] a block was translated from. Code is invalidated rarely (when
    /// it is unmapped or reprotected), so a list scanned then costs least: an interval map of
    /// block sets held ~300 bytes per block.
    struct Range {
        P first;
        P last;
        IR::LocationDescriptor location;
    };
    std::vector<Range> ranges;
};

}  // namespace Dynarmic::Backend
