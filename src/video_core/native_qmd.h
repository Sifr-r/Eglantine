// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include "video_core/nx2_perf.h"

namespace Tegra {

// Semaphore reduction ops (ADD, MIN, MAX, INC, DEC, AND, OR, XOR) shared by QMD releases and the
// compute report semaphore.
inline std::uint32_t ReduceSemaphore(std::uint32_t op, bool is_signed, std::uint32_t value,
                                     std::uint32_t payload) {
    switch (op) {
    case 0: return value + payload;
    case 1: return is_signed ? std::uint32_t((std::min)(std::int32_t(value), std::int32_t(payload)))
                             : (std::min)(value, payload);
    case 2: return is_signed ? std::uint32_t((std::max)(std::int32_t(value), std::int32_t(payload)))
                             : (std::max)(value, payload);
    case 3: return value >= payload ? 0 : value + 1;
    case 4: return value == 0 || value > payload ? payload : value - 1;
    case 5: return value & payload;
    case 6: return value | payload;
    default: return value ^ payload;
    }
}

struct NativeQmdState {
    std::array<std::uint32_t, 64> descriptor{};
    std::int32_t dependencies{};
    std::int32_t deferred{};
    bool loaded{};
    bool scheduled{};
    bool completed{};
};

struct NativeQmdStateTable {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, NativeQmdState> states;
};

// Native QMD 3.0 dependency scheduling; nonempty grids require a dispatch backend.
class NativeQmdScheduler {
public:
    using Word = std::uint32_t;
    using Address = std::uint64_t;
    using Descriptor = std::array<Word, 64>;
    struct RejectShaderDispatch { bool operator()(const Descriptor&) const { return false; } };
    // Optional diagnostics: called with "load"/"count"/"defer"/"run" events.
    std::function<void(std::string_view, Address, std::int32_t)> trace;
    // Optional diagnostics: called with the QMD, slot and raw words of a release it skips.
    std::function<void(Address, std::size_t, std::span<const Word, 4>)> skipped_release;
    bool nx2_perf = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::QmdScratch);

    void SetStateTable(std::shared_ptr<NativeQmdStateTable> table) {
        if (table) {
            state_table = std::move(table);
        } else {
            state_table = std::make_shared<NativeQmdStateTable>();
        }
    }

    void Clear() {
        std::scoped_lock lock{state_table->mutex};
        state_table->states.clear();
    }

    // QMD V03_00 release flags: 7:0 address high, 19 membar, 22:20 reduction op, 23 enable,
    // 25:24 reduction format, 26 reduction enable, 28:27 non-blocking interrupt, 29 64-bit
    // payload, 31:30 structure size (0 four words with timestamp, 1 one word, 2 two words).
    static bool IsDecodableRelease(Word flags) {
        const Word size = flags >> 30;
        if (flags & (1U << 29)) return false;
        if (flags & (1U << 26)) return size == 1 && ((flags >> 24) & 3) < 2;
        return size <= 1;
    }
    template <typename Read, typename Release>
    std::string_view Signal(Address address, Word argument, Read read, Release release) {
        return Signal(address, argument, read, release, RejectShaderDispatch{});
    }
    template <typename Read, typename Release, typename Dispatch>
    std::string_view Signal(Address address, Word argument, Read read, Release release, Dispatch dispatch) {
        struct Pending { Address address; Word action; Word count; };
        std::string_view error;
        std::array<Pending, 256> pending;
        if (!nx2_perf || VideoCommon::Nx2Perf::Verify()) pending = {};
        std::size_t count = 1;
        pending[0] = {address, argument & 15, ((argument >> 14) & 0x3ff) + 1};
        auto& table = *state_table;
        for (std::size_t i = 0; i < count; ++i) {
            const auto request = pending[i];
            bool need_read_on_load = false;
            bool skip_execution = false;
            std::int32_t trace_value = 0;
            enum class PendingTrace : std::uint8_t { None, Defer, Count };
            PendingTrace pending_trace = PendingTrace::None;
            {
                std::scoped_lock lock{table.mutex};
                if (table.states.size() >= 4096 && !table.states.contains(request.address)) {
                    // Finished QMDs with nothing deferred are only history, drop them instead of stalling.
                    std::erase_if(table.states, [](const auto& entry) {
                        return entry.second.completed && entry.second.deferred == 0;
                    });
                    if (table.states.size() >= 4096) return "QMD cache limit";
                }
                auto& state = table.states[request.address];
                if (request.action == 3 || request.action == 2) {
                    need_read_on_load = request.action == 3 || !state.loaded;
                    if (!need_read_on_load) {
                        state.scheduled = true;
                    }
                } else if (request.action == 7 || request.action == 12) {
                    const auto delta = request.action == 7 ? -std::int32_t(request.count)
                                                           : std::int32_t(request.count);
                    if (!state.loaded || state.completed) {
                        state.deferred += delta;
                        trace_value = state.deferred;
                        pending_trace = PendingTrace::Defer;
                        skip_execution = true;
                    } else {
                        state.dependencies += delta;
                        trace_value = state.dependencies;
                        pending_trace = PendingTrace::Count;
                    }
                } else if (request.action == 0 || request.action == 1) {
                    skip_execution = true;
                } else {
                    return "unsupported PCAS action";
                }
            }
            if (pending_trace == PendingTrace::Defer) {
                if (trace) trace("defer", request.address, trace_value);
            } else if (pending_trace == PendingTrace::Count) {
                if (trace) trace("count", request.address, trace_value);
            }
            if (skip_execution) {
                continue;
            }

            bool just_loaded = false;
            if (need_read_on_load) {
                Descriptor loaded_qmd{};
                if (!read(request.address, loaded_qmd)) return "unmapped QMD";
                if ((loaded_qmd[18] & 0xff) != 0x30 || (loaded_qmd[4] & 0x100)) {
                    return "unsupported QMD version or queue";
                }
                if constexpr (std::is_same_v<Dispatch, RejectShaderDispatch>) {
                    if (loaded_qmd[12] && (loaded_qmd[13] & 0xffff) && (loaded_qmd[14] & 0xffff)) {
                        return "native shader dispatch required";
                    }
                }
                const std::int32_t base_deps = std::int32_t((loaded_qmd[4] >> 14) & 0xffff);
                std::int32_t load_deps = 0;
                {
                    std::scoped_lock lock{table.mutex};
                    auto& state = table.states[request.address];
                    if (request.action == 3 || !state.loaded) {
                        state.descriptor = loaded_qmd;
                        state.dependencies = base_deps + state.deferred;
                        load_deps = state.dependencies;
                        state.deferred = 0;
                        state.loaded = true;
                        state.completed = false;
                        just_loaded = true;
                    }
                    state.scheduled = true;
                }
                if (just_loaded) {
                    if (trace) trace("base", request.address, base_deps);
                    if (trace) trace("load", request.address, load_deps);
                }
            }

            Descriptor qmd{};
            bool ready_to_run = false;
            {
                std::scoped_lock lock{table.mutex};
                auto it = table.states.find(request.address);
                if (it == table.states.end()) continue;
                auto& state = it->second;
                if (!state.loaded || !state.scheduled || state.completed || state.dependencies > 0) {
                    continue;
                }
                if (state.dependencies < 0) return "QMD dependency underflow";
                if (just_loaded) {
                    state.completed = true;
                    qmd = state.descriptor;
                    ready_to_run = true;
                }
            }
            if (!ready_to_run) {
                // The SDK patches release payloads while a scheduled QMD is
                // waiting for its remaining dependencies. Fetch them at execution.
                Descriptor refreshed_qmd{};
                if (!read(request.address, refreshed_qmd)) return "unmapped QMD";
                if ((refreshed_qmd[18] & 0xff) != 0x30 || (refreshed_qmd[4] & 0x100)) {
                    return "unsupported QMD version or queue";
                }
                std::scoped_lock lock{table.mutex};
                auto it = table.states.find(request.address);
                if (it == table.states.end()) continue;
                auto& state = it->second;
                if (!state.loaded || !state.scheduled || state.completed || state.dependencies > 0) {
                    continue;
                }
                if (state.dependencies < 0) return "QMD dependency underflow";
                state.descriptor = refreshed_qmd;
                state.completed = true;
                qmd = refreshed_qmd;
            }

            // A program Eden can't run is skipped, but the QMD still completes: its releases
            // are what other queues and the CPU wait on (the console never fails a launch).
            const bool dispatched = !(qmd[12] && (qmd[13] & 0xffff) && (qmd[14] & 0xffff)) || dispatch(qmd);
            if (!dispatched) error = "native shader dispatch required";
            if (trace) trace("run", request.address, dispatched ? 1 : 0);
            for (std::size_t slot = 0; slot < ReleaseSlots.size(); ++slot) {
                const Word base = ReleaseSlots[slot];
                const Word flags = qmd[base + 1];
                if (!(flags & (1U << 23))) continue;
                // Skip only this slot: the others are what other queues and the CPU wait on.
                if (!IsDecodableRelease(flags)) {
                    if (skipped_release) skipped_release(request.address, slot, std::span<const Word, 4>{&qmd[base], 4});
                    if (error.empty()) error = "unsupported QMD release format";
                    continue;
                }
                const Address destination = (Address{flags & 0xff} << 32) | qmd[base];
                if (!release(destination, qmd[base + 2], flags)) return "unmapped QMD release";
            }
            if (qmd[16] & 1) {
                const Word action = (qmd[16] >> 1) & 7;
                if (action != 4 && action != 3 && action != 1) return "unsupported dependent QMD action";
                if (count == pending.size()) return "QMD chain limit";
                pending[count++] = {Address{qmd[15]} << 8, action == 4 ? 7U : action == 3 ? 3U : 2U, 1};
            }
            if (qmd[16] & (1U << 5)) return "second dependent QMD unsupported";
        }
        return error;
    }
private:
    // Release slots (address low, flags | address high, payload). NVN2 patches a third one at
    // word 52 for queue syncs (e.g. a compute queue releasing a semaphore another queue waits on).
    static constexpr std::array<Word, 3> ReleaseSlots{24, 28, 52};

    std::shared_ptr<NativeQmdStateTable> state_table = std::make_shared<NativeQmdStateTable>();
};
} // namespace Tegra
