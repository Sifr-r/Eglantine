// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: 2022 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include "common/container/unordered_map.h"

#include "core/device_memory_manager.h"
#include "core/hle/service/nvdrv/core/heap_mapper.h"
#include "core/hle/service/nvdrv/nvdata.h"

namespace Kernel {
class KProcess;
}

namespace Tegra::Host1x {
class Host1x;
} // namespace Tegra::Host1x

namespace Service::Nvidia::NvCore {

class NvMap;
class SyncpointManager;

struct ContainerImpl;

struct SessionId {
    size_t id;
};

struct Session {
    Session(SessionId id_, Kernel::KProcess* process_, Core::Asid asid_);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = default;
    Session& operator=(Session&&) = default;

    SessionId id;
    Kernel::KProcess* process;
    Core::Asid asid;
    bool has_preallocated_area{};
    std::unique_ptr<HeapMapper> mapper{};
    bool is_active{};
    s32 ref_count{};
};

class Container {
public:
    explicit Container(Tegra::Host1x::Host1x& host1x);
    ~Container();

    SessionId OpenSession(Kernel::KProcess* process);
    void CloseSession(SessionId id);

    Session* GetSession(SessionId id);

    /// Identifies a session's heap mapper. The mapper itself can go away with the session, so it
    /// is named by its id, not by a pointer that would dangle.
    using HeapMapperId = u64;

    /// The id of a heap mapper, or 0 for none.
    static HeapMapperId IdOf(const HeapMapper* mapper) {
        return mapper != nullptr ? mapper->GetId() : 0;
    }

    /// Runs func on the session while holding session_guard; returns false if the session is gone.
    bool WithSession(SessionId id, const std::function<void(Session&)>& func);

    /// Runs func on the session's heap mapper while the session cannot close, so the mapper stays
    /// alive for the call. Does nothing when the session has no mapper, or when its mapper is not
    /// the one the id names: the session was closed (and maybe opened again) since the id was
    /// taken, and that mapper went with it.
    void WithSessionMapper(SessionId id, HeapMapperId mapper,
                           const std::function<void(HeapMapper&)>& func);

    NvMap& GetNvMapFile();

    const NvMap& GetNvMapFile() const;

    SyncpointManager& GetSyncpointManager();

    const SyncpointManager& GetSyncpointManager() const;

    struct Host1xDeviceFileData {
        std::deque<u32> syncpts_accumulated{};
    };

    Host1xDeviceFileData& Host1xDeviceFile();

    const Host1xDeviceFileData& Host1xDeviceFile() const;

private:
    std::unique_ptr<ContainerImpl> impl;
};

} // namespace Service::Nvidia::NvCore
