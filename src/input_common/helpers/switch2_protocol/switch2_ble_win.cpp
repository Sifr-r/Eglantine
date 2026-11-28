// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <utility>

#include "common/common_types.h"
#include "input_common/helpers/switch2_protocol/switch2_transport.h"

#ifdef _WIN32

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "common/logging.h"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#ifdef _MSC_VER
#pragma comment(lib, "windowsapp")
#endif

namespace InputCommon::Switch2 {
namespace {

namespace wf = winrt::Windows::Foundation;
namespace wdb = winrt::Windows::Devices::Bluetooth;
namespace wdba = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace wdbg = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace wss = winrt::Windows::Storage::Streams;

constexpr winrt::hresult HrRpcEChangedMode{static_cast<s32>(0x80010106U)};
constexpr winrt::hresult HrTimeout{static_cast<s32>(0x800705B4U)};

constexpr std::chrono::milliseconds GattOpTimeout{10000};
constexpr std::chrono::milliseconds VibrationOpTimeout{1000};
constexpr std::chrono::milliseconds PairCommandTimeout{3000};

class WinrtApartmentGuard final {
public:
    WinrtApartmentGuard() noexcept {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            m_initialized = true;
        } catch (const winrt::hresult_error& err) {
            if (err.code() == HrRpcEChangedMode) {
                m_initialized = false;
            } else {
                LOG_WARNING(Input, "WinRT apartment initialization failed: 0x{:08X}",
                            static_cast<u32>(err.code().value));
            }
        } catch (...) {
            LOG_WARNING(Input, "WinRT apartment initialization failed with unknown error");
        }
    }

    ~WinrtApartmentGuard() {
        if (m_initialized) {
            winrt::uninit_apartment();
        }
    }

    WinrtApartmentGuard(const WinrtApartmentGuard&) = delete;
    WinrtApartmentGuard& operator=(const WinrtApartmentGuard&) = delete;

private:
    bool m_initialized{false};
};

template <typename AsyncOp>
auto AwaitAsync(const AsyncOp& op, std::chrono::milliseconds timeout) {
    struct AwaitState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
    };

    auto state = std::make_shared<AwaitState>();
    op.Completed([state](const AsyncOp&, wf::AsyncStatus) {
        {
            std::lock_guard lock(state->mutex);
            state->done = true;
        }
        state->cv.notify_one();
    });

    std::unique_lock lock(state->mutex);
    if (!state->cv.wait_for(lock, timeout, [&state] { return state->done; })) {
        lock.unlock();
        try {
            op.Cancel();
        } catch (const winrt::hresult_error& err) {
            LOG_DEBUG(Input, "Cancelling a timed-out WinRT operation failed: 0x{:08X}",
                      static_cast<u32>(err.code().value));
        } catch (...) {
            LOG_DEBUG(Input, "Cancelling a timed-out WinRT operation failed with an unknown error");
        }
        throw winrt::hresult_error(HrTimeout);
    }

    return op.GetResults();
}

std::string FormatBluetoothAddress(u64 addr) {
    return fmt::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                       static_cast<u32>((addr >> 40) & 0xFFULL),
                       static_cast<u32>((addr >> 32) & 0xFFULL),
                       static_cast<u32>((addr >> 24) & 0xFFULL),
                       static_cast<u32>((addr >> 16) & 0xFFULL),
                       static_cast<u32>((addr >> 8) & 0xFFULL),
                       static_cast<u32>(addr & 0xFFULL));
}

std::optional<winrt::guid> VibrationUuidForProduct(ProductId pid) {
    switch (pid) {
    case ProductId::JoyCon2Left:
        return winrt::guid{VibrationJoyConLUuid};
    case ProductId::JoyCon2Right:
        return winrt::guid{VibrationJoyConRUuid};
    case ProductId::ProController2:
    case ProductId::NsoGameCube:
        return winrt::guid{VibrationProControllerUuid};
    case ProductId::None:
    default:
        return std::nullopt;
    }
}

wss::IBuffer BytesToBuffer(std::span<const u8> data) {
    wss::DataWriter writer;
    if (!data.empty()) {
        writer.WriteBytes(winrt::array_view<const uint8_t>{data.data(), data.data() + data.size()});
    }
    return writer.DetachBuffer();
}

std::vector<u8> BufferToBytes(const wss::IBuffer& buffer) {
    if (!buffer) {
        return {};
    }
    const u32 length = buffer.Length();
    if (length == 0) {
        return {};
    }
    std::vector<u8> result(length);
    const auto reader = wss::DataReader::FromBuffer(buffer);
    reader.ReadBytes(winrt::array_view<uint8_t>{result.data(), result.data() + result.size()});
    return result;
}

struct PendingDevice {
    u64 bluetooth_addr{};
    BleAdvertisementInfo adv{};
};

struct ScannerState {
    std::atomic<bool> running{false};
    std::atomic<u64> host_mac{0};

    std::mutex callback_mutex;
    TransportDiscoveredCallback on_discovered;

    std::mutex addresses_mutex;
    std::unordered_set<u64> active_addresses;
    std::unordered_set<u64> logged_rejected_addresses;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::vector<PendingDevice> pending_queue;

    void ReleaseAddress(u64 addr) {
        std::lock_guard lock(addresses_mutex);
        active_addresses.erase(addr);
    }
};

class WinBleTransport final : public ISwitch2Transport,
                              public std::enable_shared_from_this<WinBleTransport> {
public:
    WinBleTransport(u64 bluetooth_addr_, ProductId product_id_,
                    std::weak_ptr<ScannerState> scanner_state_)
        : m_bluetooth_addr{bluetooth_addr_},
          m_product_id{product_id_},
          m_address{FormatBluetoothAddress(bluetooth_addr_)},
          m_scanner_state{std::move(scanner_state_)} {}

    ~WinBleTransport() override {
        Disconnect();
    }

    WinBleTransport(const WinBleTransport&) = delete;
    WinBleTransport& operator=(const WinBleTransport&) = delete;

    bool Connect(u64 host_mac, bool is_pairing_mode) {
        try {
            auto device = AwaitAsync(wdb::BluetoothLEDevice::FromBluetoothAddressAsync(m_bluetooth_addr),
                                     std::chrono::milliseconds{15000});
            if (!device) {
                LOG_WARNING(Input, "BluetoothLEDevice::FromBluetoothAddressAsync returned null for {}",
                            m_address);
                return false;
            }

            std::weak_ptr<WinBleTransport> self_weak = weak_from_this();
            const winrt::event_token conn_token = device.ConnectionStatusChanged(
                [self_weak](const wdb::BluetoothLEDevice& sender, const wf::IInspectable&) {
                    try {
                        if (auto self = self_weak.lock()) {
                            self->OnConnectionStatusChanged(sender);
                        }
                    } catch (const winrt::hresult_error& err) {
                        LOG_WARNING(Input, "ConnectionStatusChanged handler error: 0x{:08X}",
                                    static_cast<u32>(err.code().value));
                    } catch (...) {
                        LOG_WARNING(Input, "ConnectionStatusChanged handler unknown error");
                    }
                });

            {
                std::lock_guard lock(m_device_mutex);
                m_device = device;
                m_conn_status_token = conn_token;
            }

            if (IsScannerStopped()) {
                Disconnect();
                return false;
            }

            ApplyPreferredConnectionParameters(device);

            if (!DiscoverGattCharacteristics(device) || IsScannerStopped()) {
                Disconnect();
                return false;
            }

            if (m_disconnect_requested.load(std::memory_order_acquire)) {
                Disconnect();
                return false;
            }

            m_connected.store(true, std::memory_order_release);

            if (!SubscribeNotifications() || IsScannerStopped()) {
                Disconnect();
                return false;
            }

            if (is_pairing_mode && !PerformPairingHandshake(host_mac)) {
                LOG_WARNING(Input, "Switch 2 BLE pairing handshake failed for {}", m_address);
                Disconnect();
                return false;
            }

            LOG_INFO(Input, "Connected Switch 2 BLE controller {} ({})", ProductName(m_product_id),
                     m_address);
            return true;
        } catch (const winrt::hresult_error& err) {
            LOG_WARNING(Input, "BLE connection failed for {}: 0x{:08X}", m_address,
                        static_cast<u32>(err.code().value));
        } catch (...) {
            LOG_WARNING(Input, "BLE connection failed for {} with unknown error", m_address);
        }

        Disconnect();
        return false;
    }

    [[nodiscard]] bool IsConnected() const override {
        return m_connected.load(std::memory_order_acquire);
    }

    [[nodiscard]] TransportType GetTransportType() const override {
        return TransportType::Ble;
    }

    [[nodiscard]] std::string GetAddress() const override {
        return m_address;
    }

    [[nodiscard]] ProductId GetProductId() const override {
        return m_product_id;
    }

    bool WriteCommand(std::span<const u8> packet, std::vector<u8>& out_response,
                      std::chrono::milliseconds timeout) override {
        if (packet.empty() || !IsConnected()) {
            return false;
        }

        WinrtApartmentGuard apartment;

        const auto clamped_timeout =
            timeout.count() > 0 ? timeout : std::chrono::milliseconds{1};

        std::lock_guard cmd_lock(m_command_mutex);
        if (!IsConnected()) {
            return false;
        }

        wdbg::GattCharacteristic write_char{nullptr};
        {
            std::lock_guard dev_lock(m_device_mutex);
            write_char = m_command_write_char;
        }
        if (!write_char) {
            return false;
        }

        try {
            {
                std::lock_guard resp_lock(m_response_mutex);
                m_pending_response.reset();
                m_awaiting_response = true;
            }

            const auto buffer = BytesToBuffer(packet);
            const auto props = write_char.CharacteristicProperties();
            const bool supports_write_with_response =
                (props & wdbg::GattCharacteristicProperties::Write) !=
                wdbg::GattCharacteristicProperties::None;
            const auto write_option = supports_write_with_response
                                          ? wdbg::GattWriteOption::WriteWithResponse
                                          : wdbg::GattWriteOption::WriteWithoutResponse;

            const auto write_result =
                AwaitAsync(write_char.WriteValueWithResultAsync(buffer, write_option),
                           clamped_timeout);
            if (write_result.Status() != wdbg::GattCommunicationStatus::Success) {
                std::lock_guard resp_lock(m_response_mutex);
                m_awaiting_response = false;
                m_pending_response.reset();
                LOG_WARNING(Input, "BLE WriteCommand failed for {} (status={})", m_address,
                            static_cast<u32>(write_result.Status()));
                return false;
            }

            std::unique_lock resp_lock(m_response_mutex);
            const bool received = m_response_cv.wait_for(resp_lock, clamped_timeout, [this] {
                return m_pending_response.has_value() || !IsConnected();
            });

            m_awaiting_response = false;
            if (!received || !m_pending_response.has_value()) {
                m_pending_response.reset();
                return false;
            }

            out_response = std::move(*m_pending_response);
            m_pending_response.reset();
            return true;
        } catch (const winrt::hresult_error& err) {
            {
                std::lock_guard resp_lock(m_response_mutex);
                m_awaiting_response = false;
                m_pending_response.reset();
            }
            LOG_WARNING(Input, "BLE WriteCommand exception for {}: 0x{:08X}", m_address,
                        static_cast<u32>(err.code().value));
            return false;
        } catch (...) {
            {
                std::lock_guard resp_lock(m_response_mutex);
                m_awaiting_response = false;
                m_pending_response.reset();
            }
            LOG_WARNING(Input, "BLE WriteCommand unknown exception for {}", m_address);
            return false;
        }
    }

    bool WriteVibration(std::span<const u8> report) override {
        if (report.empty() || !IsConnected()) {
            return false;
        }

        WinrtApartmentGuard apartment;

        wdbg::GattCharacteristic vib_char{nullptr};
        {
            std::lock_guard dev_lock(m_device_mutex);
            vib_char = m_vibration_char;
        }
        if (!vib_char) {
            return false;
        }

        try {
            const auto buffer = BytesToBuffer(report);
            const auto write_result = AwaitAsync(
                vib_char.WriteValueWithResultAsync(buffer, wdbg::GattWriteOption::WriteWithoutResponse),
                VibrationOpTimeout);
            return write_result.Status() == wdbg::GattCommunicationStatus::Success;
        } catch (const winrt::hresult_error& err) {
            LOG_WARNING(Input, "BLE WriteVibration exception for {}: 0x{:08X}", m_address,
                        static_cast<u32>(err.code().value));
            return false;
        } catch (...) {
            LOG_WARNING(Input, "BLE WriteVibration unknown exception for {}", m_address);
            return false;
        }
    }

    void SetInputCallback(std::function<void(std::span<const u8>)> callback) override {
        std::lock_guard lock(m_callback_mutex);
        m_input_callback = std::move(callback);
    }

    void SetDisconnectCallback(std::function<void()> callback) override {
        std::lock_guard lock(m_callback_mutex);
        m_disconnect_callback = std::move(callback);
    }

    void Disconnect() override {
        WinrtApartmentGuard apartment;

        m_disconnect_requested.store(true, std::memory_order_release);
        const bool was_connected = m_connected.exchange(false, std::memory_order_acq_rel);

        {
            std::lock_guard cb_lock(m_callback_mutex);
            m_input_callback = nullptr;
            m_disconnect_callback = nullptr;
        }

        {
            std::lock_guard resp_lock(m_response_mutex);
            m_awaiting_response = false;
            m_pending_response.reset();
        }
        m_response_cv.notify_all();

        wdb::BluetoothLEDevice device{nullptr};
        std::vector<wdbg::GattDeviceService> services;
        wdbg::GattCharacteristic cmd_write_char{nullptr};
        wdbg::GattCharacteristic cmd_resp_char{nullptr};
        wdbg::GattCharacteristic input_char{nullptr};
        wdbg::GattCharacteristic vib_char{nullptr};
        winrt::event_token conn_token{};
        winrt::event_token cmd_resp_token{};
        winrt::event_token input_token{};

        {
            std::lock_guard dev_lock(m_device_mutex);
            device = std::exchange(m_device, nullptr);
            services = std::exchange(m_services, {});
            cmd_write_char = std::exchange(m_command_write_char, nullptr);
            cmd_resp_char = std::exchange(m_command_response_char, nullptr);
            input_char = std::exchange(m_input_report_char, nullptr);
            vib_char = std::exchange(m_vibration_char, nullptr);
            conn_token = std::exchange(m_conn_status_token, winrt::event_token{});
            cmd_resp_token = std::exchange(m_cmd_resp_token, winrt::event_token{});
            input_token = std::exchange(m_input_report_token, winrt::event_token{});
        }

        if (device && conn_token.value != 0) {
            try {
                device.ConnectionStatusChanged(conn_token);
            } catch (const winrt::hresult_error& err) {
                LOG_DEBUG(Input, "Removing the connection status handler of {} failed: 0x{:08X}",
                          m_address, static_cast<u32>(err.code().value));
            } catch (...) {
                LOG_DEBUG(Input, "Removing the connection status handler of {} failed with an "
                                 "unknown error", m_address);
            }
        }
        if (cmd_resp_char && cmd_resp_token.value != 0) {
            try {
                cmd_resp_char.ValueChanged(cmd_resp_token);
            } catch (const winrt::hresult_error& err) {
                LOG_DEBUG(Input, "Removing the command response handler of {} failed: 0x{:08X}",
                          m_address, static_cast<u32>(err.code().value));
            } catch (...) {
                LOG_DEBUG(Input, "Removing the command response handler of {} failed with an "
                                 "unknown error", m_address);
            }
        }
        if (input_char && input_token.value != 0) {
            try {
                input_char.ValueChanged(input_token);
            } catch (const winrt::hresult_error& err) {
                LOG_DEBUG(Input, "Removing the input report handler of {} failed: 0x{:08X}",
                          m_address, static_cast<u32>(err.code().value));
            } catch (...) {
                LOG_DEBUG(Input, "Removing the input report handler of {} failed with an unknown "
                                 "error", m_address);
            }
        }

        for (auto& service : services) {
            if (service) {
                try {
                    service.Close();
                } catch (const winrt::hresult_error& err) {
                    LOG_DEBUG(Input, "Closing a GATT service of {} failed: 0x{:08X}", m_address,
                              static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_DEBUG(Input, "Closing a GATT service of {} failed with an unknown error",
                              m_address);
                }
            }
        }
        if (device) {
            try {
                device.Close();
            } catch (const winrt::hresult_error& err) {
                LOG_DEBUG(Input, "Closing the BLE device {} failed: 0x{:08X}", m_address,
                          static_cast<u32>(err.code().value));
            } catch (...) {
                LOG_DEBUG(Input, "Closing the BLE device {} failed with an unknown error",
                          m_address);
            }
        }

        if (auto scanner_state = m_scanner_state.lock()) {
            scanner_state->ReleaseAddress(m_bluetooth_addr);
        }

        if (was_connected) {
            LOG_INFO(Input, "Disconnected Switch 2 BLE controller {}", m_address);
        }
    }

private:
    void NotifyDisconnect() {
        if (m_disconnect_notified.exchange(true)) {
            return;
        }
        std::function<void()> cb;
        {
            std::lock_guard lock(m_callback_mutex);
            cb = m_disconnect_callback;
        }
        if (cb) {
            cb();
        }
    }

    [[nodiscard]] bool IsScannerStopped() const {
        if (const auto state = m_scanner_state.lock()) {
            return !state->running.load(std::memory_order_acquire);
        }
        return true;
    }

    void ApplyPreferredConnectionParameters(const wdb::BluetoothLEDevice& device) {
        try {
            if (auto device6 = device.try_as<wdb::IBluetoothLEDevice6>()) {
                const auto params =
                    wdb::BluetoothLEPreferredConnectionParameters::ThroughputOptimized();
                device6.RequestPreferredConnectionParameters(params);
                LOG_INFO(Input, "Applied ThroughputOptimized connection parameters for {}", m_address);
            }
        } catch (const winrt::hresult_error& err) {
            LOG_WARNING(Input, "ThroughputOptimized unavailable/failed for {}: 0x{:08X}",
                        m_address, static_cast<u32>(err.code().value));
        } catch (...) {
            LOG_WARNING(Input, "ThroughputOptimized failed for {}", m_address);
        }
    }

    bool DiscoverGattCharacteristics(const wdb::BluetoothLEDevice& device) {
        const auto services_result =
            AwaitAsync(device.GetGattServicesAsync(wdb::BluetoothCacheMode::Uncached), GattOpTimeout);
        if (services_result.Status() != wdbg::GattCommunicationStatus::Success) {
            LOG_WARNING(Input, "GetGattServicesAsync failed for {} (status={})", m_address,
                        static_cast<u32>(services_result.Status()));
            return false;
        }

        const winrt::guid cmd_write_guid{CommandWriteUuid};
        const winrt::guid cmd_resp_guid{CommandResponseUuid};
        const winrt::guid input_guid{InputReportUuid};
        const std::optional<winrt::guid> vib_guid = VibrationUuidForProduct(m_product_id);

        std::vector<wdbg::GattDeviceService> discovered_services;
        wdbg::GattCharacteristic found_cmd_write{nullptr};
        wdbg::GattCharacteristic found_cmd_resp{nullptr};
        wdbg::GattCharacteristic found_input{nullptr};
        wdbg::GattCharacteristic found_vib{nullptr};

        for (const auto& service : services_result.Services()) {
            const auto chars_result =
                AwaitAsync(service.GetCharacteristicsAsync(wdb::BluetoothCacheMode::Uncached),
                           GattOpTimeout);
            if (chars_result.Status() != wdbg::GattCommunicationStatus::Success) {
                try {
                    service.Close();
                } catch (const winrt::hresult_error& err) {
                    LOG_DEBUG(Input, "Closing an unreadable GATT service of {} failed: 0x{:08X}",
                              m_address, static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_DEBUG(Input, "Closing an unreadable GATT service of {} failed with an "
                                     "unknown error", m_address);
                }
                continue;
            }

            bool service_used = false;
            for (const auto& ch : chars_result.Characteristics()) {
                const winrt::guid uuid = ch.Uuid();
                if (uuid == cmd_write_guid) {
                    found_cmd_write = ch;
                    service_used = true;
                } else if (uuid == cmd_resp_guid) {
                    found_cmd_resp = ch;
                    service_used = true;
                } else if (uuid == input_guid) {
                    found_input = ch;
                    service_used = true;
                } else if (vib_guid.has_value() && uuid == *vib_guid) {
                    found_vib = ch;
                    service_used = true;
                }
            }

            if (service_used) {
                discovered_services.push_back(service);
            } else {
                try {
                    service.Close();
                } catch (const winrt::hresult_error& err) {
                    LOG_DEBUG(Input, "Closing an unused GATT service of {} failed: 0x{:08X}",
                              m_address, static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_DEBUG(Input, "Closing an unused GATT service of {} failed with an unknown "
                                     "error", m_address);
                }
            }
        }

        if (!found_cmd_write || !found_cmd_resp || !found_input) {
            for (auto& service : discovered_services) {
                try {
                    service.Close();
                } catch (const winrt::hresult_error& err) {
                    LOG_DEBUG(Input, "Closing a GATT service of {} failed: 0x{:08X}", m_address,
                              static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_DEBUG(Input, "Closing a GATT service of {} failed with an unknown error",
                              m_address);
                }
            }
            LOG_WARNING(Input, "Missing required Switch 2 GATT characteristics on {}", m_address);
            return false;
        }

        {
            std::lock_guard lock(m_device_mutex);
            m_services = std::move(discovered_services);
            m_command_write_char = found_cmd_write;
            m_command_response_char = found_cmd_resp;
            m_input_report_char = found_input;
            m_vibration_char = found_vib;
        }
        return true;
    }

    bool EnableNotifyWithRetry(const wdbg::GattCharacteristic& ch, std::string_view label) {
        constexpr int max_attempts = 3;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            if (!IsConnected() || IsScannerStopped()) {
                return false;
            }
            try {
                const auto status = AwaitAsync(
                    ch.WriteClientCharacteristicConfigurationDescriptorAsync(
                        wdbg::GattClientCharacteristicConfigurationDescriptorValue::Notify),
                    GattOpTimeout);
                if (status == wdbg::GattCommunicationStatus::Success) {
                    return true;
                }
                LOG_WARNING(Input, "Enable {} CCCD failed on {} (attempt {}/{}, status={})", label,
                            m_address, attempt + 1, max_attempts, static_cast<u32>(status));
            } catch (const winrt::hresult_error& err) {
                LOG_WARNING(Input, "Enable {} CCCD exception on {} (attempt {}/{}): 0x{:08X}",
                            label, m_address, attempt + 1, max_attempts,
                            static_cast<u32>(err.code().value));
            } catch (...) {
                LOG_WARNING(Input, "Enable {} CCCD unknown exception on {} (attempt {}/{})", label,
                            m_address, attempt + 1, max_attempts);
            }

            if (attempt + 1 < max_attempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds{400});
            }
        }
        return false;
    }

    bool SubscribeNotifications() {
        wdbg::GattCharacteristic cmd_resp_char{nullptr};
        wdbg::GattCharacteristic input_char{nullptr};
        {
            std::lock_guard lock(m_device_mutex);
            cmd_resp_char = m_command_response_char;
            input_char = m_input_report_char;
        }
        if (!cmd_resp_char || !input_char) {
            return false;
        }

        std::weak_ptr<WinBleTransport> self_weak = weak_from_this();

        const winrt::event_token cmd_token = cmd_resp_char.ValueChanged(
            [self_weak](const wdbg::GattCharacteristic&,
                        const wdbg::GattValueChangedEventArgs& args) {
                try {
                    if (auto self = self_weak.lock()) {
                        self->OnCommandResponse(args);
                    }
                } catch (const winrt::hresult_error& err) {
                    LOG_WARNING(Input, "CommandResponse ValueChanged error: 0x{:08X}",
                                static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_WARNING(Input, "CommandResponse ValueChanged unknown error");
                }
            });

        {
            std::lock_guard lock(m_device_mutex);
            m_cmd_resp_token = cmd_token;
        }

        if (!EnableNotifyWithRetry(cmd_resp_char, "CommandResponse")) {
            return false;
        }

        const winrt::event_token input_token = input_char.ValueChanged(
            [self_weak](const wdbg::GattCharacteristic&,
                        const wdbg::GattValueChangedEventArgs& args) {
                try {
                    if (auto self = self_weak.lock()) {
                        self->OnInputReport(args);
                    }
                } catch (const winrt::hresult_error& err) {
                    LOG_WARNING(Input, "InputReport ValueChanged error: 0x{:08X}",
                                static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_WARNING(Input, "InputReport ValueChanged unknown error");
                }
            });

        {
            std::lock_guard lock(m_device_mutex);
            m_input_report_token = input_token;
        }

        return EnableNotifyWithRetry(input_char, "InputReport");
    }

    bool SendPairSubcommand(u8 subcommand_id, std::span<const u8> payload) {
        const std::vector<u8> packet =
            BuildCommandPacket(CommandId::Pair, subcommand_id, payload, true);
        std::vector<u8> response;
        if (!WriteCommand(packet, response, PairCommandTimeout)) {
            return false;
        }
        if (!ParseCommandResponse(response, CommandId::Pair, true)) {
            LOG_WARNING(Input, "Unexpected pairing response for {} (subcmd=0x{:02X}, len={})",
                        m_address, subcommand_id, response.size());
            return false;
        }
        return true;
    }

    bool PerformPairingHandshake(u64 host_mac) {
        if (!SendPairSubcommand(SubCommandId::PairSetMac, BuildPairSetMacPayload(host_mac))) {
            return false;
        }

        constexpr std::array<u8, 17> ltk1{
            0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42, 0xc6,
            0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31,
        };
        if (!SendPairSubcommand(SubCommandId::PairLtk1, ltk1)) {
            return false;
        }

        constexpr std::array<u8, 17> ltk2{
            0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b, 0x41,
            0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73,
        };
        if (!SendPairSubcommand(SubCommandId::PairLtk2, ltk2)) {
            return false;
        }

        constexpr std::array<u8, 1> finish{0x00};
        if (!SendPairSubcommand(SubCommandId::PairFinish, finish)) {
            return false;
        }

        LOG_INFO(Input, "Completed Switch 2 BLE pairing handshake for {}", m_address);
        return true;
    }

    void OnCommandResponse(const wdbg::GattValueChangedEventArgs& args) {
        std::vector<u8> bytes = BufferToBytes(args.CharacteristicValue());
        {
            std::lock_guard lock(m_response_mutex);
            if (!m_awaiting_response) {
                return;
            }
            m_pending_response = std::move(bytes);
            m_awaiting_response = false;
        }
        m_response_cv.notify_one();
    }

    void OnInputReport(const wdbg::GattValueChangedEventArgs& args) {
        std::function<void(std::span<const u8>)> callback_copy;
        {
            std::lock_guard lock(m_callback_mutex);
            if (!IsConnected()) {
                return;
            }
            callback_copy = m_input_callback;
        }
        if (!callback_copy) {
            return;
        }
        const std::vector<u8> bytes = BufferToBytes(args.CharacteristicValue());
        if (!bytes.empty()) {
            callback_copy(bytes);
        }
    }

    void OnConnectionStatusChanged(const wdb::BluetoothLEDevice& sender) {
        const auto status = sender.ConnectionStatus();
        if (status == wdb::BluetoothConnectionStatus::Connected) {
            m_saw_connected.store(true, std::memory_order_release);
        } else if (status == wdb::BluetoothConnectionStatus::Disconnected) {
            const bool was_connected = m_connected.load(std::memory_order_acquire);
            if (was_connected || m_saw_connected.load(std::memory_order_acquire)) {
                if (was_connected && !m_disconnect_requested.load(std::memory_order_acquire)) {
                    // The controller dropped the link on its own; report it to the driver.
                    NotifyDisconnect();
                }
                Disconnect();
            }
        }
    }

    const u64 m_bluetooth_addr;
    const ProductId m_product_id;
    const std::string m_address;
    const std::weak_ptr<ScannerState> m_scanner_state;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_saw_connected{false};
    std::atomic<bool> m_disconnect_requested{false};
    std::atomic<bool> m_disconnect_notified{false};

    std::mutex m_device_mutex;
    wdb::BluetoothLEDevice m_device{nullptr};
    std::vector<wdbg::GattDeviceService> m_services;
    wdbg::GattCharacteristic m_command_write_char{nullptr};
    wdbg::GattCharacteristic m_command_response_char{nullptr};
    wdbg::GattCharacteristic m_input_report_char{nullptr};
    wdbg::GattCharacteristic m_vibration_char{nullptr};
    winrt::event_token m_conn_status_token{};
    winrt::event_token m_cmd_resp_token{};
    winrt::event_token m_input_report_token{};

    std::mutex m_command_mutex;
    std::mutex m_response_mutex;
    std::condition_variable m_response_cv;
    bool m_awaiting_response{false};
    std::optional<std::vector<u8>> m_pending_response;

    std::mutex m_callback_mutex;
    std::function<void(std::span<const u8>)> m_input_callback;
    std::function<void()> m_disconnect_callback;
};

class WinBleScanner final : public ISwitch2Scanner {
public:
    WinBleScanner() : m_state{std::make_shared<ScannerState>()} {}

    ~WinBleScanner() override {
        Stop();
    }

    WinBleScanner(const WinBleScanner&) = delete;
    WinBleScanner& operator=(const WinBleScanner&) = delete;

    void Start(TransportDiscoveredCallback on_discovered) override {
        WinrtApartmentGuard apartment;
        std::lock_guard lifecycle_lock(m_lifecycle_mutex);

        if (m_state->running.load(std::memory_order_acquire)) {
            return;
        }

        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }

        {
            std::lock_guard lock(m_state->callback_mutex);
            m_state->on_discovered = std::move(on_discovered);
        }

        m_state->running.store(true, std::memory_order_release);
        try {
            m_worker_thread = std::thread(&WinBleScanner::WorkerMain, this);
        } catch (...) {
            m_state->running.store(false, std::memory_order_release);
            LOG_WARNING(Input, "Failed to start Switch 2 BLE scanner worker thread");
        }
    }

    void Stop() override {
        std::lock_guard lifecycle_lock(m_lifecycle_mutex);
        const bool was_running = m_state->running.exchange(false, std::memory_order_acq_rel);

        std::vector<PendingDevice> cancelled;
        {
            std::lock_guard lock(m_state->queue_mutex);
            cancelled = std::exchange(m_state->pending_queue, {});
        }
        for (const auto& item : cancelled) {
            m_state->ReleaseAddress(item.bluetooth_addr);
        }
        m_state->queue_cv.notify_all();

        StopWatcher();

        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }

        if (was_running) {
            {
                std::lock_guard lock(m_state->addresses_mutex);
                m_state->logged_rejected_addresses.clear();
            }
            std::lock_guard lock(m_state->callback_mutex);
            m_state->on_discovered = nullptr;
        }
    }

private:
    void StopWatcher() {
        WinrtApartmentGuard apartment;
        wdba::BluetoothLEAdvertisementWatcher watcher{nullptr};
        winrt::event_token received_token{};
        winrt::event_token stopped_token{};

        {
            std::lock_guard lock(m_watcher_mutex);
            watcher = std::exchange(m_watcher, nullptr);
            received_token = std::exchange(m_received_token, winrt::event_token{});
            stopped_token = std::exchange(m_stopped_token, winrt::event_token{});
        }

        if (watcher) {
            if (received_token.value != 0) {
                try {
                    watcher.Received(received_token);
                } catch (const winrt::hresult_error& err) {
                    LOG_DEBUG(Input, "Removing the advertisement watcher handler failed: "
                                     "0x{:08X}", static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_DEBUG(Input, "Removing the advertisement watcher handler failed with an "
                                     "unknown error");
                }
            }
            if (stopped_token.value != 0) {
                try {
                    watcher.Stopped(stopped_token);
                } catch (const winrt::hresult_error& err) {
                    LOG_DEBUG(Input, "Removing the advertisement watcher stopped handler failed: "
                                     "0x{:08X}", static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_DEBUG(Input, "Removing the advertisement watcher stopped handler failed "
                                     "with an unknown error");
                }
            }
            try {
                watcher.Stop();
            } catch (const winrt::hresult_error& err) {
                LOG_DEBUG(Input, "Stopping the BLE advertisement watcher failed: 0x{:08X}",
                          static_cast<u32>(err.code().value));
            } catch (...) {
                LOG_DEBUG(Input, "Stopping the BLE advertisement watcher failed with an unknown "
                                 "error");
            }
        }
    }

    bool InitializeWatcher() {
        try {
            const auto adapter = AwaitAsync(wdb::BluetoothAdapter::GetDefaultAsync(), GattOpTimeout);
            if (!adapter) {
                LOG_WARNING(Input,
                            "No default Bluetooth adapter available; Switch 2 BLE scan disabled");
                return false;
            }

            const u64 adapter_mac = adapter.BluetoothAddress();
            m_state->host_mac.store(adapter_mac, std::memory_order_release);
            LOG_INFO(Input, "Switch 2 BLE scanner using host adapter {}",
                     FormatBluetoothAddress(adapter_mac));

            wdba::BluetoothLEAdvertisementWatcher watcher;
            watcher.ScanningMode(wdba::BluetoothLEScanningMode::Active);

            std::weak_ptr<ScannerState> state_weak = m_state;
            const winrt::event_token recv_token = watcher.Received(
                [state_weak](const wdba::BluetoothLEAdvertisementWatcher&,
                             const wdba::BluetoothLEAdvertisementReceivedEventArgs& args) {
                    try {
                        if (auto state = state_weak.lock()) {
                            HandleAdvertisement(state, args);
                        }
                    } catch (const winrt::hresult_error& err) {
                        LOG_WARNING(Input, "BLE Advertisement Received error: 0x{:08X}",
                                    static_cast<u32>(err.code().value));
                    } catch (...) {
                        LOG_WARNING(Input, "BLE Advertisement Received unknown error");
                    }
                });

            const winrt::event_token stop_token = watcher.Stopped(
                [](const wdba::BluetoothLEAdvertisementWatcher&,
                   const wdba::BluetoothLEAdvertisementWatcherStoppedEventArgs& args) {
                    try {
                        const auto err = args.Error();
                        if (err != wdb::BluetoothError::Success) {
                            LOG_WARNING(Input, "BLE advertisement watcher stopped (error={})",
                                        static_cast<u32>(err));
                        }
                    } catch (const winrt::hresult_error& err) {
                        LOG_DEBUG(Input, "Reading the BLE watcher stop error failed: 0x{:08X}",
                                  static_cast<u32>(err.code().value));
                    } catch (...) {
                        LOG_DEBUG(Input, "Reading the BLE watcher stop error failed with an "
                                         "unknown error");
                    }
                });

            auto revoke_tokens = [&watcher, recv_token, stop_token]() {
                try {
                    watcher.Received(recv_token);
                    watcher.Stopped(stop_token);
                } catch (const winrt::hresult_error& err) {
                    LOG_DEBUG(Input, "Removing the advertisement watcher handlers failed: "
                                     "0x{:08X}", static_cast<u32>(err.code().value));
                } catch (...) {
                    LOG_DEBUG(Input, "Removing the advertisement watcher handlers failed with an "
                                     "unknown error");
                }
            };

            {
                std::lock_guard lock(m_watcher_mutex);
                if (!m_state->running.load(std::memory_order_acquire)) {
                    revoke_tokens();
                    return false;
                }
                try {
                    watcher.Start();
                } catch (...) {
                    revoke_tokens();
                    throw;
                }
                m_watcher = watcher;
                m_received_token = recv_token;
                m_stopped_token = stop_token;
            }

            return true;
        } catch (const winrt::hresult_error& err) {
            LOG_WARNING(Input, "Failed to initialize Switch 2 BLE watcher: 0x{:08X}",
                        static_cast<u32>(err.code().value));
            return false;
        } catch (...) {
            LOG_WARNING(Input, "Failed to initialize Switch 2 BLE watcher with unknown error");
            return false;
        }
    }

    static void HandleAdvertisement(const std::shared_ptr<ScannerState>& state,
                                    const wdba::BluetoothLEAdvertisementReceivedEventArgs& args) {
        if (!state->running.load(std::memory_order_acquire)) {
            return;
        }

        const u64 bluetooth_addr = args.BluetoothAddress();
        {
            std::lock_guard lock(state->addresses_mutex);
            if (state->active_addresses.contains(bluetooth_addr)) {
                return;
            }
        }

        const u64 host_mac = state->host_mac.load(std::memory_order_acquire);
        const auto mfg_data_list = args.Advertisement().ManufacturerData();
        for (const auto& mfg_data : mfg_data_list) {
            if (mfg_data.CompanyId() != NintendoBleManufacturerId) {
                continue;
            }

            const std::vector<u8> payload = BufferToBytes(mfg_data.Data());
            const std::optional<BleAdvertisementInfo> adv = ParseBleManufacturerData(payload);
            if (!adv || !ShouldAcceptBleAdvertisement(*adv, host_mac)) {
                bool should_log = false;
                {
                    std::lock_guard addr_lock(state->addresses_mutex);
                    should_log = state->logged_rejected_addresses.insert(bluetooth_addr).second;
                }
                if (should_log) {
                    if (!adv) {
                        LOG_INFO(Input,
                                 "Ignored Switch 2 BLE advertisement from {}: invalid or short "
                                 "0x0553 payload (len={})",
                                 FormatBluetoothAddress(bluetooth_addr), payload.size());
                    } else {
                        LOG_INFO(Input,
                                 "Ignored Switch 2 BLE {} at {}: not in pairing mode "
                                 "(reconnect_mac={}, host_mac={})",
                                 ProductName(adv->product_id),
                                 FormatBluetoothAddress(bluetooth_addr),
                                 FormatBluetoothAddress(adv->reconnect_mac),
                                 FormatBluetoothAddress(host_mac));
                    }
                }
                continue;
            }

            {
                std::lock_guard addr_lock(state->addresses_mutex);
                if (!state->running.load(std::memory_order_acquire) ||
                    state->active_addresses.contains(bluetooth_addr)) {
                    return;
                }
                state->logged_rejected_addresses.erase(bluetooth_addr);
                state->active_addresses.insert(bluetooth_addr);
            }

            LOG_INFO(Input, "Detected Switch 2 BLE {} at {} ({})",
                     ProductName(adv->product_id), FormatBluetoothAddress(bluetooth_addr),
                     adv->is_pairing_mode ? "pairing" : "reconnect");

            {
                std::lock_guard queue_lock(state->queue_mutex);
                state->pending_queue.push_back(PendingDevice{
                    .bluetooth_addr = bluetooth_addr,
                    .adv = *adv,
                });
            }
            state->queue_cv.notify_one();
            return;
        }
    }

    void WorkerMain() {
        WinrtApartmentGuard apartment;

        if (!InitializeWatcher()) {
            m_state->running.store(false, std::memory_order_release);
            return;
        }

        while (m_state->running.load(std::memory_order_acquire)) {
            PendingDevice next{};
            {
                std::unique_lock lock(m_state->queue_mutex);
                m_state->queue_cv.wait(lock, [this] {
                    return !m_state->running.load(std::memory_order_acquire) ||
                           !m_state->pending_queue.empty();
                });

                if (!m_state->running.load(std::memory_order_acquire)) {
                    break;
                }

                next = m_state->pending_queue.front();
                m_state->pending_queue.erase(m_state->pending_queue.begin());
            }

            auto transport = std::make_shared<WinBleTransport>(
                next.bluetooth_addr, next.adv.product_id, m_state);
            const u64 host_mac = m_state->host_mac.load(std::memory_order_acquire);
            if (!transport->Connect(host_mac, next.adv.is_pairing_mode)) {
                m_state->ReleaseAddress(next.bluetooth_addr);
                continue;
            }

            if (!m_state->running.load(std::memory_order_acquire)) {
                transport->Disconnect();
                continue;
            }

            TransportDiscoveredCallback discovered_cb;
            {
                std::lock_guard cb_lock(m_state->callback_mutex);
                discovered_cb = m_state->on_discovered;
            }

            if (discovered_cb) {
                try {
                    discovered_cb(std::move(transport));
                } catch (...) {
                    LOG_WARNING(Input, "Switch 2 BLE on_discovered callback threw an exception");
                }
            } else {
                transport->Disconnect();
            }
        }
    }

    std::shared_ptr<ScannerState> m_state;
    std::mutex m_lifecycle_mutex;
    std::mutex m_watcher_mutex;
    wdba::BluetoothLEAdvertisementWatcher m_watcher{nullptr};
    winrt::event_token m_received_token{};
    winrt::event_token m_stopped_token{};
    std::thread m_worker_thread;
};

} // namespace

std::unique_ptr<ISwitch2Scanner> CreateBleScanner() {
    return std::make_unique<WinBleScanner>();
}

} // namespace InputCommon::Switch2

#else

namespace InputCommon::Switch2 {
namespace {

class NullBleScanner final : public ISwitch2Scanner {
public:
    void Start(TransportDiscoveredCallback /*on_discovered*/) override {}
    void Stop() override {}
};

} // namespace

std::unique_ptr<ISwitch2Scanner> CreateBleScanner() {
    return std::make_unique<NullBleScanner>();
}

} // namespace InputCommon::Switch2

#endif
