// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "core/hle/service/service.h"

namespace Service {
class Event;
namespace KernelHelpers {
class ServiceContext;
}
} // namespace Service

namespace Service::NS {

class IAsyncResult final : public ServiceFramework<IAsyncResult> {
public:
    explicit IAsyncResult(Core::System& system_,
                          std::shared_ptr<KernelHelpers::ServiceContext> service_context_,
                          std::shared_ptr<Service::Event> event_);
    ~IAsyncResult() override;

private:
    Result Cancel();

    std::shared_ptr<KernelHelpers::ServiceContext> service_context;
    std::shared_ptr<Service::Event> event;
};

} // namespace Service::NS