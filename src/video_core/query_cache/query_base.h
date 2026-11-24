// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_funcs.h"
#include "common/common_types.h"

namespace VideoCommon {

enum class QueryFlagBits : u32 {
    HasTimestamp = 1 << 0,       ///< Indicates if this query has a timestamp.
    IsFinalValueSynced = 1 << 1, ///< Indicates if the query has been synced in the host
    IsHostSynced = 1 << 2,       ///< Indicates if the query has been synced in the host
    IsGuestSynced = 1 << 3,      ///< Indicates if the query has been synced with the guest.
    IsHostManaged = 1 << 4,      ///< Indicates if this query points to a host query
    IsRewritten = 1 << 5,        ///< Indicates if this query was rewritten by another query
    IsInvalidated = 1 << 6,      ///< Indicates the value of th query has been nullified.
    IsOrphan = 1 << 7,           ///< Indicates the query has not been set by a guest query.
    IsFence = 1 << 8,            ///< Indicates the query is a fence.
};
DECLARE_ENUM_FLAG_OPERATORS(QueryFlagBits)

class AtomicQueryFlags {
public:
    AtomicQueryFlags() noexcept = default;
    AtomicQueryFlags(QueryFlagBits initial) noexcept : bits{static_cast<u32>(initial)} {}
    AtomicQueryFlags(const AtomicQueryFlags& other) noexcept
        : bits{other.bits.load(std::memory_order_acquire)} {}
    AtomicQueryFlags(AtomicQueryFlags&& other) noexcept
        : bits{other.bits.load(std::memory_order_acquire)} {}
    ~AtomicQueryFlags() = default;

    AtomicQueryFlags& operator=(const AtomicQueryFlags& other) noexcept {
        bits.store(other.bits.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }

    AtomicQueryFlags& operator=(AtomicQueryFlags&& other) noexcept {
        bits.store(other.bits.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }

    AtomicQueryFlags& operator=(QueryFlagBits value) noexcept {
        bits.store(static_cast<u32>(value), std::memory_order_release);
        return *this;
    }

    operator QueryFlagBits() const noexcept {
        return static_cast<QueryFlagBits>(bits.load(std::memory_order_acquire));
    }

    AtomicQueryFlags& operator|=(QueryFlagBits value) noexcept {
        bits.fetch_or(static_cast<u32>(value), std::memory_order_acq_rel);
        return *this;
    }

    AtomicQueryFlags& operator&=(QueryFlagBits value) noexcept {
        bits.fetch_and(static_cast<u32>(value), std::memory_order_acq_rel);
        return *this;
    }

    AtomicQueryFlags& operator^=(QueryFlagBits value) noexcept {
        bits.fetch_xor(static_cast<u32>(value), std::memory_order_acq_rel);
        return *this;
    }

    friend QueryFlagBits operator&(const AtomicQueryFlags& lhs, QueryFlagBits rhs) noexcept {
        return static_cast<QueryFlagBits>(lhs) & rhs;
    }

    friend QueryFlagBits operator&(QueryFlagBits lhs, const AtomicQueryFlags& rhs) noexcept {
        return lhs & static_cast<QueryFlagBits>(rhs);
    }

    friend QueryFlagBits operator|(const AtomicQueryFlags& lhs, QueryFlagBits rhs) noexcept {
        return static_cast<QueryFlagBits>(lhs) | rhs;
    }

    friend QueryFlagBits operator|(QueryFlagBits lhs, const AtomicQueryFlags& rhs) noexcept {
        return lhs | static_cast<QueryFlagBits>(rhs);
    }

    friend QueryFlagBits operator&(const AtomicQueryFlags& lhs,
                                   const AtomicQueryFlags& rhs) noexcept {
        return static_cast<QueryFlagBits>(lhs) & static_cast<QueryFlagBits>(rhs);
    }

    friend QueryFlagBits operator|(const AtomicQueryFlags& lhs,
                                   const AtomicQueryFlags& rhs) noexcept {
        return static_cast<QueryFlagBits>(lhs) | static_cast<QueryFlagBits>(rhs);
    }

    friend bool operator==(const AtomicQueryFlags& lhs, QueryFlagBits rhs) noexcept {
        return static_cast<QueryFlagBits>(lhs) == rhs;
    }

    friend bool operator==(const AtomicQueryFlags& lhs, const AtomicQueryFlags& rhs) noexcept {
        return static_cast<QueryFlagBits>(lhs) == static_cast<QueryFlagBits>(rhs);
    }

private:
    std::atomic<u32> bits{0};
};

class QueryBase {
public:
    DAddr guest_address{};
    AtomicQueryFlags flags{};
    u64 value{};

protected:
    // Default constructor
    QueryBase() = default;

    // Parameterized constructor
    QueryBase(DAddr address, QueryFlagBits flags_, u64 value_)
        : guest_address(address), flags(flags_), value{value_} {}
};

class GuestQuery : public QueryBase {
public:
    // Parameterized constructor
    GuestQuery(bool isLong, VAddr address, u64 queryValue)
        : QueryBase(address, QueryFlagBits::IsFinalValueSynced, queryValue) {
        if (isLong) {
            flags |= QueryFlagBits::HasTimestamp;
        }
    }
};

class HostQueryBase : public QueryBase {
public:
    // Default constructor
    HostQueryBase() : QueryBase(0, QueryFlagBits::IsHostManaged | QueryFlagBits::IsOrphan, 0) {}

    // Parameterized constructor
    HostQueryBase(bool has_timestamp, VAddr address)
        : QueryBase(address, QueryFlagBits::IsHostManaged, 0), start_bank_id{}, size_banks{},
          start_slot{}, size_slots{} {
        if (has_timestamp) {
            flags |= QueryFlagBits::HasTimestamp;
        }
    }

    u32 start_bank_id{};
    u32 size_banks{};
    size_t start_slot{};
    size_t size_slots{};
};

} // namespace VideoCommon
