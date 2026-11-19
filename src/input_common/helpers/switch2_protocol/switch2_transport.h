// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "input_common/helpers/switch2_protocol/switch2_types.h"

namespace InputCommon::Switch2 {

class ISwitch2Transport {
public:
    virtual ~ISwitch2Transport() = default;

    [[nodiscard]] virtual bool IsConnected() const = 0;
    [[nodiscard]] virtual TransportType GetTransportType() const = 0;
    [[nodiscard]] virtual std::string GetAddress() const = 0;
    [[nodiscard]] virtual ProductId GetProductId() const = 0;

    virtual bool WriteCommand(std::span<const u8> packet, std::vector<u8>& out_response,
                              std::chrono::milliseconds timeout) = 0;
    virtual bool WriteVibration(std::span<const u8> report) = 0;
    virtual void SetInputCallback(std::function<void(std::span<const u8>)> callback) = 0;
    /// Registers the callback invoked when the device is lost unexpectedly. An explicit
    /// Disconnect() never triggers it.
    virtual void SetDisconnectCallback(std::function<void()> callback) = 0;
    virtual void Disconnect() = 0;
};

using TransportDiscoveredCallback = std::function<void(std::shared_ptr<ISwitch2Transport>)>;

class ISwitch2Scanner {
public:
    virtual ~ISwitch2Scanner() = default;
    virtual void Start(TransportDiscoveredCallback on_discovered) = 0;
    virtual void Stop() = 0;
};

[[nodiscard]] std::unique_ptr<ISwitch2Scanner> CreateBleScanner();
[[nodiscard]] std::unique_ptr<ISwitch2Scanner> CreateUsbScanner();

} // namespace InputCommon::Switch2
