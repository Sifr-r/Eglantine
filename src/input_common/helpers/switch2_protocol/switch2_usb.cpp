// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef ENABLE_LIBUSB
#include <fmt/format.h>
#include <libusb.h>

#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#endif

#include "common/common_types.h"
#include "input_common/helpers/switch2_protocol/switch2_transport.h"
#include "input_common/helpers/switch2_protocol/switch2_types.h"

namespace InputCommon::Switch2 {
namespace {

#ifndef ENABLE_LIBUSB

class NoopUsbScanner final : public ISwitch2Scanner {
public:
    void Start([[maybe_unused]] TransportDiscoveredCallback on_discovered) override {}
    void Stop() override {}
};

#else

constexpr std::chrono::seconds ScanInterval{3};
constexpr unsigned int InputReadTimeoutMs = 100;
constexpr unsigned int InitTransferTimeoutMs = 500;
constexpr unsigned int InitDrainTimeoutMs = 100;
constexpr unsigned int VibrationTimeoutMs = 50;
constexpr u32 FatalErrorThreshold = 5;
constexpr std::size_t InputReportBufferSize = 64;
constexpr std::size_t CommandResponseBufferSize = 512;

struct UsbEndpoints {
    u8 intr_in{};
    u8 intr_out{};
    u8 bulk_in{};
    u8 bulk_out{};
    bool has_interface_0{false};
    bool has_interface_1{false};
};

class LibUsbContext final {
public:
    LibUsbContext() noexcept {
        init_result = libusb_init(&ctx);
        if (init_result != LIBUSB_SUCCESS) {
            ctx = nullptr;
        }
    }

    ~LibUsbContext() noexcept {
        if (ctx != nullptr) {
            libusb_exit(ctx);
            ctx = nullptr;
        }
    }

    LibUsbContext(const LibUsbContext&) = delete;
    LibUsbContext& operator=(const LibUsbContext&) = delete;
    LibUsbContext(LibUsbContext&&) = delete;
    LibUsbContext& operator=(LibUsbContext&&) = delete;

    [[nodiscard]] int InitResult() const noexcept {
        return init_result;
    }

    [[nodiscard]] libusb_context* Get() const noexcept {
        return ctx;
    }

private:
    libusb_context* ctx{nullptr};
    int init_result{LIBUSB_ERROR_OTHER};
};

using DeviceListCount = decltype(libusb_get_device_list(nullptr, nullptr));

class DeviceListGuard final {
public:
    explicit DeviceListGuard(libusb_context* ctx) noexcept {
        if (ctx != nullptr) {
            device_count = libusb_get_device_list(ctx, &devices);
            if (device_count < 0) {
                devices = nullptr;
            }
        }
    }

    ~DeviceListGuard() noexcept {
        if (devices != nullptr) {
            libusb_free_device_list(devices, 1);
            devices = nullptr;
        }
    }

    DeviceListGuard(const DeviceListGuard&) = delete;
    DeviceListGuard& operator=(const DeviceListGuard&) = delete;
    DeviceListGuard(DeviceListGuard&&) = delete;
    DeviceListGuard& operator=(DeviceListGuard&&) = delete;

    [[nodiscard]] DeviceListCount Count() const noexcept {
        return device_count;
    }

    [[nodiscard]] libusb_device* At(DeviceListCount index) const noexcept {
        if (devices == nullptr || index < 0 || index >= device_count) {
            return nullptr;
        }
        return devices[index];
    }

private:
    libusb_device** devices{nullptr};
    DeviceListCount device_count{0};
};

class ConfigDescriptorGuard final {
public:
    explicit ConfigDescriptorGuard(libusb_device* dev) noexcept {
        if (dev == nullptr) {
            return;
        }
        int res = libusb_get_active_config_descriptor(dev, &config);
        if (res != LIBUSB_SUCCESS || config == nullptr) {
            config = nullptr;
            res = libusb_get_config_descriptor(dev, 0, &config);
            if (res != LIBUSB_SUCCESS) {
                config = nullptr;
            }
        }
    }

    ~ConfigDescriptorGuard() noexcept {
        if (config != nullptr) {
            libusb_free_config_descriptor(config);
            config = nullptr;
        }
    }

    ConfigDescriptorGuard(const ConfigDescriptorGuard&) = delete;
    ConfigDescriptorGuard& operator=(const ConfigDescriptorGuard&) = delete;
    ConfigDescriptorGuard(ConfigDescriptorGuard&&) = delete;
    ConfigDescriptorGuard& operator=(ConfigDescriptorGuard&&) = delete;

    [[nodiscard]] const libusb_config_descriptor* Get() const noexcept {
        return config;
    }

private:
    libusb_config_descriptor* config{nullptr};
};

[[nodiscard]] UsbEndpoints InspectEndpoints(const libusb_config_descriptor* config) noexcept {
    UsbEndpoints endpoints{};
    if (config == nullptr || config->interface == nullptr) {
        return endpoints;
    }

    for (u8 iface_idx = 0; iface_idx < config->bNumInterfaces; ++iface_idx) {
        const libusb_interface& iface = config->interface[iface_idx];
        if (iface.altsetting == nullptr) {
            continue;
        }
        for (int alt_idx = 0; alt_idx < iface.num_altsetting; ++alt_idx) {
            const libusb_interface_descriptor& alt_desc = iface.altsetting[alt_idx];
            const u8 iface_num = alt_desc.bInterfaceNumber;
            if (alt_desc.endpoint == nullptr) {
                continue;
            }

            if (iface_num == 0) {
                endpoints.has_interface_0 = true;
            } else if (iface_num == 1) {
                endpoints.has_interface_1 = true;
            }

            for (u8 ep_idx = 0; ep_idx < alt_desc.bNumEndpoints; ++ep_idx) {
                const libusb_endpoint_descriptor& ep = alt_desc.endpoint[ep_idx];
                const int transfer_type = static_cast<int>(ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK);
                const bool is_in = (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;

                if (iface_num == 0 &&
                    transfer_type == static_cast<int>(LIBUSB_TRANSFER_TYPE_INTERRUPT)) {
                    if (is_in && endpoints.intr_in == 0) {
                        endpoints.intr_in = ep.bEndpointAddress;
                    } else if (!is_in && endpoints.intr_out == 0) {
                        endpoints.intr_out = ep.bEndpointAddress;
                    }
                } else if (iface_num == 1 &&
                           transfer_type == static_cast<int>(LIBUSB_TRANSFER_TYPE_BULK)) {
                    if (is_in && endpoints.bulk_in == 0) {
                        endpoints.bulk_in = ep.bEndpointAddress;
                    } else if (!is_in && endpoints.bulk_out == 0) {
                        endpoints.bulk_out = ep.bEndpointAddress;
                    }
                }
            }
        }
    }

    return endpoints;
}

[[nodiscard]] bool ClaimInterfaceCleanly(libusb_device_handle* handle, int interface_number,
                                         const std::string& address) {
    if (handle == nullptr) {
        return false;
    }

    const int kernel_active = libusb_kernel_driver_active(handle, interface_number);
    if (kernel_active == 1) {
        const int detach_res = libusb_detach_kernel_driver(handle, interface_number);
        if (detach_res != LIBUSB_SUCCESS && detach_res != LIBUSB_ERROR_NOT_SUPPORTED) {
            LOG_WARNING(Input, "Switch2 USB {}: failed to detach kernel driver on interface {}: {}",
                        address, interface_number, libusb_error_name(detach_res));
            return false;
        }
    } else if (kernel_active < 0 && kernel_active != LIBUSB_ERROR_NOT_SUPPORTED) {
        LOG_DEBUG(Input, "Switch2 USB {}: libusb_kernel_driver_active({}) returned {}", address,
                  interface_number, libusb_error_name(kernel_active));
    }

    const int claim_res = libusb_claim_interface(handle, interface_number);
    if (claim_res != LIBUSB_SUCCESS) {
        LOG_DEBUG(Input, "Switch2 USB {}: failed to claim interface {}: {}", address,
                  interface_number, libusb_error_name(claim_res));
        return false;
    }

    return true;
}

[[nodiscard]] bool IsAllowedInitError(int err) noexcept {
    return err == LIBUSB_ERROR_TIMEOUT || err == LIBUSB_ERROR_PIPE;
}

[[nodiscard]] bool SendInitPacket(libusb_device_handle* handle, u8 bulk_out_ep, u8 bulk_in_ep,
                                  std::span<const u8> packet, const std::string& address,
                                  const char* stage_name) {
    if (handle == nullptr || bulk_out_ep == 0 || packet.empty()) {
        return true;
    }
    if (packet.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    std::vector<u8> tx_buf(packet.begin(), packet.end());
    int transferred = 0;
    const int write_res =
        libusb_bulk_transfer(handle, bulk_out_ep, tx_buf.data(), static_cast<int>(tx_buf.size()),
                             &transferred, InitTransferTimeoutMs);
    if (write_res == LIBUSB_ERROR_NO_DEVICE) {
        LOG_DEBUG(Input, "Switch2 USB {}: device removed during {} write", address, stage_name);
        return false;
    }
    if (write_res != LIBUSB_SUCCESS) {
        if (IsAllowedInitError(write_res)) {
            LOG_DEBUG(Input, "Switch2 USB {}: {} bulk write returned {} (continuing)", address,
                      stage_name, libusb_error_name(write_res));
        } else {
            LOG_DEBUG(Input, "Switch2 USB {}: {} bulk write error: {}", address, stage_name,
                      libusb_error_name(write_res));
        }
        return true;
    }

    if (bulk_in_ep != 0) {
        std::array<u8, CommandResponseBufferSize> rx_buf{};
        int rx_len = 0;
        const int read_res =
            libusb_bulk_transfer(handle, bulk_in_ep, rx_buf.data(), static_cast<int>(rx_buf.size()),
                                 &rx_len, InitDrainTimeoutMs);
        if (read_res == LIBUSB_ERROR_NO_DEVICE) {
            LOG_DEBUG(Input, "Switch2 USB {}: device removed during {} response drain", address,
                      stage_name);
            return false;
        }
        if (read_res != LIBUSB_SUCCESS && !IsAllowedInitError(read_res)) {
            LOG_DEBUG(Input, "Switch2 USB {}: {} response drain returned {}", address, stage_name,
                      libusb_error_name(read_res));
        }
    }

    return true;
}

class Switch2UsbTransport final : public ISwitch2Transport,
                                  public std::enable_shared_from_this<Switch2UsbTransport> {
public:
    static std::shared_ptr<Switch2UsbTransport> Open(std::shared_ptr<LibUsbContext> ctx_,
                                                     libusb_device* dev, ProductId product_id_,
                                                     std::string address_) {
        if (!ctx_ || ctx_->Get() == nullptr || dev == nullptr) {
            return nullptr;
        }

        const ConfigDescriptorGuard config_guard(dev);
        if (config_guard.Get() == nullptr) {
            LOG_DEBUG(Input, "Switch2 USB {}: failed to read config descriptor", address_);
            return nullptr;
        }

        UsbEndpoints endpoints = InspectEndpoints(config_guard.Get());
        if (endpoints.intr_in == 0 && endpoints.bulk_in == 0) {
            LOG_DEBUG(Input, "Switch2 USB {}: no supported IN endpoint found", address_);
            return nullptr;
        }

        libusb_device_handle* raw_handle = nullptr;
        const int open_res = libusb_open(dev, &raw_handle);
        if (open_res != LIBUSB_SUCCESS || raw_handle == nullptr) {
            LOG_DEBUG(Input, "Switch2 USB {}: libusb_open failed: {}", address_,
                      libusb_error_name(open_res));
            return nullptr;
        }

        bool claimed_0 = false;
        bool claimed_1 = false;

        if (endpoints.has_interface_1 && (endpoints.bulk_in != 0 || endpoints.bulk_out != 0)) {
            claimed_1 = ClaimInterfaceCleanly(raw_handle, 1, address_);
            if (!claimed_1) {
                endpoints.bulk_in = 0;
                endpoints.bulk_out = 0;
            }
        }

        if (endpoints.has_interface_0 && (endpoints.intr_in != 0 || endpoints.intr_out != 0)) {
            claimed_0 = ClaimInterfaceCleanly(raw_handle, 0, address_);
            if (!claimed_0) {
                endpoints.intr_in = 0;
                endpoints.intr_out = 0;
            }
        }

        if (endpoints.intr_in == 0 && endpoints.bulk_in == 0) {
            LOG_DEBUG(Input, "Switch2 USB {}: unable to claim any interface with an IN endpoint",
                      address_);
            if (claimed_0) {
                libusb_release_interface(raw_handle, 0);
            }
            if (claimed_1) {
                libusb_release_interface(raw_handle, 1);
            }
            libusb_close(raw_handle);
            return nullptr;
        }

        if (endpoints.bulk_out != 0) {
            const std::vector<u8> usb_init_pkt = BuildCommandPacket(CommandId::UsbInit, 0x01, {}, false);
            // PROTOCOL.md section 7: 0x94 is the temporary startup mask that is used before
            // the normal controller feature set is selected.
            const std::array<u8, 4> startup_flags{FeatureFlags::Startup, 0x00, 0x00, 0x00};
            // PROTOCOL.md section 38: the physical USB controller keeps streaming its native
            // side report until the common 0x05 report is selected.
            const std::array<u8, 4> report_selector{CommonReportId, 0x00, 0x00, 0x00};
            // PROTOCOL.md section 7: the normal feature set of this controller.
            const std::array<u8, 4> normal_flags{FeatureMaskForProduct(product_id_), 0x00, 0x00,
                                                 0x00};

            const std::vector<u8> feature_init_pkt =
                BuildCommandPacket(CommandId::Feature, SubCommandId::FeatureInit, startup_flags,
                                   false);
            const std::vector<u8> feature_enable_pkt = BuildCommandPacket(
                CommandId::Feature, SubCommandId::FeatureEnable, startup_flags, false);
            // The input report selector uses the same 0x03 command as the USB init command.
            const std::vector<u8> select_report_pkt = BuildCommandPacket(
                CommandId::UsbInit, SubCommandId::SelectInputReport, report_selector, false);
            const std::vector<u8> normal_init_pkt = BuildCommandPacket(
                CommandId::Feature, SubCommandId::FeatureInit, normal_flags, false);
            const std::vector<u8> normal_enable_pkt = BuildCommandPacket(
                CommandId::Feature, SubCommandId::FeatureEnable, normal_flags, false);

            const bool init_ok =
                SendInitPacket(raw_handle, endpoints.bulk_out, endpoints.bulk_in, usb_init_pkt,
                               address_, "UsbInit") &&
                SendInitPacket(raw_handle, endpoints.bulk_out, endpoints.bulk_in, feature_init_pkt,
                               address_, "FeatureInit") &&
                SendInitPacket(raw_handle, endpoints.bulk_out, endpoints.bulk_in, feature_enable_pkt,
                               address_, "FeatureEnable") &&
                SendInitPacket(raw_handle, endpoints.bulk_out, endpoints.bulk_in, select_report_pkt,
                               address_, "SelectInputReport") &&
                SendInitPacket(raw_handle, endpoints.bulk_out, endpoints.bulk_in, normal_init_pkt,
                               address_, "FeatureInitNormal") &&
                SendInitPacket(raw_handle, endpoints.bulk_out, endpoints.bulk_in, normal_enable_pkt,
                               address_, "FeatureEnableNormal");

            if (!init_ok) {
                if (claimed_0) {
                    libusb_release_interface(raw_handle, 0);
                }
                if (claimed_1) {
                    libusb_release_interface(raw_handle, 1);
                }
                libusb_close(raw_handle);
                return nullptr;
            }
        }

        auto transport = std::make_shared<Switch2UsbTransport>(
            PrivateTag{}, std::move(ctx_), raw_handle, product_id_, std::move(address_), endpoints,
            claimed_0, claimed_1);
        transport->StartReader();
        return transport;
    }

    struct PrivateTag {};

    Switch2UsbTransport(PrivateTag, std::shared_ptr<LibUsbContext> ctx_,
                        libusb_device_handle* handle_, ProductId product_id_, std::string address_,
                        UsbEndpoints endpoints_, bool claimed_0_, bool claimed_1_)
        : usb_ctx(std::move(ctx_)), handle(handle_), product_id(product_id_),
          address(std::move(address_)), endpoints(endpoints_), claimed_iface_0(claimed_0_),
          claimed_iface_1(claimed_1_) {}

    ~Switch2UsbTransport() override {
        Disconnect();
    }

    Switch2UsbTransport(const Switch2UsbTransport&) = delete;
    Switch2UsbTransport& operator=(const Switch2UsbTransport&) = delete;
    Switch2UsbTransport(Switch2UsbTransport&&) = delete;
    Switch2UsbTransport& operator=(Switch2UsbTransport&&) = delete;

    [[nodiscard]] bool IsConnected() const override {
        return connected.load();
    }

    [[nodiscard]] TransportType GetTransportType() const override {
        return TransportType::Usb;
    }

    [[nodiscard]] std::string GetAddress() const override {
        return address;
    }

    [[nodiscard]] ProductId GetProductId() const override {
        return product_id;
    }

    bool WriteCommand(std::span<const u8> packet, std::vector<u8>& out_response,
                      std::chrono::milliseconds timeout) override {
        out_response.clear();
        if (!connected.load() || packet.empty()) {
            return false;
        }
        if (packet.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        if (endpoints.bulk_out == 0 && endpoints.intr_out == 0) {
            return false;
        }
        if (endpoints.bulk_in == 0) {
            return false;
        }

        const auto clamped_ms = std::clamp<s64>(timeout.count(), 1, 60000);
        const unsigned int timeout_ms = static_cast<unsigned int>(clamped_ms);

        std::vector<u8> tx_buf(packet.begin(), packet.end());
        std::array<u8, CommandResponseBufferSize> rx_buf{};

        std::scoped_lock cmd_lk{command_mutex};
        if (!connected.load() || handle == nullptr) {
            return false;
        }

        int tx_len = 0;
        int write_res = LIBUSB_ERROR_OTHER;
        if (endpoints.bulk_out != 0) {
            write_res =
                libusb_bulk_transfer(handle, endpoints.bulk_out, tx_buf.data(),
                                     static_cast<int>(tx_buf.size()), &tx_len, timeout_ms);
        } else {
            std::scoped_lock intr_lk{intr_out_mutex};
            write_res =
                libusb_interrupt_transfer(handle, endpoints.intr_out, tx_buf.data(),
                                          static_cast<int>(tx_buf.size()), &tx_len, timeout_ms);
        }

        if (write_res != LIBUSB_SUCCESS || tx_len <= 0) {
            if (write_res == LIBUSB_ERROR_NO_DEVICE) {
                connected.store(false);
                device_lost.store(true);
            }
            LOG_DEBUG(Input, "Switch2 USB {}: WriteCommand send failed: {}", address,
                      libusb_error_name(write_res));
            return false;
        }

        std::scoped_lock bulk_in_lk{bulk_in_mutex};
        if (!connected.load() || handle == nullptr) {
            return false;
        }

        int rx_len = 0;
        const int read_res =
            libusb_bulk_transfer(handle, endpoints.bulk_in, rx_buf.data(),
                                 static_cast<int>(rx_buf.size()), &rx_len, timeout_ms);
        if ((read_res == LIBUSB_SUCCESS || read_res == LIBUSB_ERROR_TIMEOUT) && rx_len > 0) {
            out_response.assign(rx_buf.begin(), rx_buf.begin() + rx_len);
            return true;
        }

        if (read_res == LIBUSB_ERROR_NO_DEVICE) {
            connected.store(false);
            device_lost.store(true);
        }
        LOG_DEBUG(Input, "Switch2 USB {}: WriteCommand response read failed: {}", address,
                  libusb_error_name(read_res));
        return false;
    }

    bool WriteVibration(std::span<const u8> report) override {
        if (!connected.load() || report.empty()) {
            return false;
        }
        if (report.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        if (endpoints.intr_out == 0 && endpoints.bulk_out == 0) {
            return false;
        }

        std::vector<u8> tx_buf(report.begin(), report.end());
        int transferred = 0;
        int write_res = LIBUSB_ERROR_OTHER;

        if (endpoints.intr_out != 0) {
            std::scoped_lock lk{intr_out_mutex};
            if (!connected.load() || handle == nullptr) {
                return false;
            }
            write_res =
                libusb_interrupt_transfer(handle, endpoints.intr_out, tx_buf.data(),
                                          static_cast<int>(tx_buf.size()), &transferred,
                                          VibrationTimeoutMs);
        } else {
            std::scoped_lock lk{command_mutex};
            if (!connected.load() || handle == nullptr) {
                return false;
            }
            write_res = libusb_bulk_transfer(handle, endpoints.bulk_out, tx_buf.data(),
                                             static_cast<int>(tx_buf.size()), &transferred,
                                             VibrationTimeoutMs);
        }

        if (write_res != LIBUSB_SUCCESS) {
            if (write_res == LIBUSB_ERROR_NO_DEVICE) {
                connected.store(false);
                device_lost.store(true);
            }
            LOG_DEBUG(Input, "Switch2 USB {}: WriteVibration failed: {}", address,
                      libusb_error_name(write_res));
            return false;
        }

        return true;
    }

    void SetInputCallback(std::function<void(std::span<const u8>)> callback) override {
        std::scoped_lock lk{callback_mutex};
        input_callback = std::move(callback);
    }

    void SetDisconnectCallback(std::function<void()> callback) override {
        std::scoped_lock lk{callback_mutex};
        disconnect_callback = std::move(callback);
    }

    void Disconnect() override {
        connected.store(false);

        {
            std::scoped_lock cb_lk{callback_mutex};
            input_callback = nullptr;
            disconnect_callback = nullptr;
        }

        std::jthread thread_to_stop;
        {
            std::scoped_lock reader_lk{reader_mutex};
            thread_to_stop = std::move(reader_thread);
        }

        if (thread_to_stop.joinable()) {
            thread_to_stop.request_stop();
            if (thread_to_stop.get_id() != std::this_thread::get_id()) {
                thread_to_stop.join();
            } else {
                thread_to_stop.detach();
            }
        }

        std::scoped_lock lock{command_mutex, intr_out_mutex, bulk_in_mutex};
        if (handle != nullptr) {
            if (claimed_iface_0) {
                libusb_release_interface(handle, 0);
                claimed_iface_0 = false;
            }
            if (claimed_iface_1) {
                libusb_release_interface(handle, 1);
                claimed_iface_1 = false;
            }
            libusb_close(handle);
            handle = nullptr;
        }
    }

private:
    void NotifyDisconnect() {
        if (disconnect_notified.exchange(true)) {
            return;
        }
        std::function<void()> cb;
        {
            std::scoped_lock lk{callback_mutex};
            cb = disconnect_callback;
        }
        if (cb != nullptr) {
            cb();
        }
    }

    void StartReader() {
        std::scoped_lock reader_lk{reader_mutex};
        reader_thread =
            std::jthread([this](std::stop_token stop_token) { ReadLoop(stop_token); });
    }

    void ReadLoop(std::stop_token stop_token) {
        Common::SetCurrentThreadName("Switch2UsbRead");

        const bool use_interrupt = endpoints.intr_in != 0;
        const u8 in_endpoint = use_interrupt ? endpoints.intr_in : endpoints.bulk_in;
        if (in_endpoint == 0) {
            connected.store(false);
            return;
        }

        std::array<u8, InputReportBufferSize> buffer{};
        u32 consecutive_fatal_errors = 0;

        while (!stop_token.stop_requested() && connected.load()) {
            int transferred = 0;
            int res = LIBUSB_ERROR_OTHER;

            if (use_interrupt) {
                res = libusb_interrupt_transfer(handle, in_endpoint, buffer.data(),
                                                static_cast<int>(buffer.size()), &transferred,
                                                InputReadTimeoutMs);
            } else {
                std::scoped_lock bulk_in_lk{bulk_in_mutex};
                if (!connected.load() || handle == nullptr) {
                    break;
                }
                res = libusb_bulk_transfer(handle, in_endpoint, buffer.data(),
                                           static_cast<int>(buffer.size()), &transferred,
                                           InputReadTimeoutMs);
            }

            if (res == LIBUSB_SUCCESS || (res == LIBUSB_ERROR_TIMEOUT && transferred > 0)) {
                consecutive_fatal_errors = 0;
                // PROTOCOL.md sections 5 and 29: a USB input report is a HID report whose
                // byte 0 holds the report ID, while the native payload follows it. The report
                // ID is stripped here so the parser always receives the native layout.
                if (transferred >= static_cast<int>(MinimumInputReportSize + 1) &&
                    transferred <= static_cast<int>(buffer.size())) {
                    const auto native = StripHidReportId(
                        std::span<const u8>{buffer.data(), static_cast<std::size_t>(transferred)});
                    if (native.has_value()) {
                        if (buffer[0] != CommonReportId) {
                            LOG_DEBUG(Input,
                                      "Switch2 USB {}: unexpected input report id 0x{:02X}", address,
                                      buffer[0]);
                        }
                        std::function<void(std::span<const u8>)> cb;
                        {
                            std::scoped_lock lk{callback_mutex};
                            cb = input_callback;
                        }
                        if (cb != nullptr) {
                            cb(*native);
                        }
                    }
                }
                continue;
            }

            if (res == LIBUSB_ERROR_TIMEOUT || res == LIBUSB_ERROR_INTERRUPTED) {
                continue;
            }

            if (res == LIBUSB_ERROR_NO_DEVICE || res == LIBUSB_ERROR_IO) {
                ++consecutive_fatal_errors;
                LOG_DEBUG(Input, "Switch2 USB {}: read fatal error ({}/{}): {}", address,
                          consecutive_fatal_errors, FatalErrorThreshold, libusb_error_name(res));
                if (consecutive_fatal_errors > FatalErrorThreshold) {
                    LOG_INFO(Input, "Switch2 USB {}: disconnecting after consecutive read errors",
                             address);
                    connected.store(false);
                    device_lost.store(true);
                    break;
                }
            } else {
                LOG_DEBUG(Input, "Switch2 USB {}: read returned {}", address,
                          libusb_error_name(res));
            }

            Common::StoppableTimedWait(stop_token, std::chrono::milliseconds{10});
        }

        // The device is gone: tell the driver so it can drop the controller. An explicit
        // Disconnect() request does not set this flag.
        if (device_lost.load()) {
            const auto self = weak_from_this().lock();
            NotifyDisconnect();
        }
    }

    std::shared_ptr<LibUsbContext> usb_ctx;
    libusb_device_handle* handle{nullptr};
    ProductId product_id{ProductId::None};
    std::string address{};
    UsbEndpoints endpoints{};
    bool claimed_iface_0{false};
    bool claimed_iface_1{false};

    std::atomic<bool> connected{true};
    std::atomic<bool> device_lost{false};
    std::atomic<bool> disconnect_notified{false};
    std::mutex command_mutex;
    std::mutex intr_out_mutex;
    std::mutex bulk_in_mutex;
    std::mutex callback_mutex;
    std::mutex reader_mutex;
    std::function<void(std::span<const u8>)> input_callback;
    std::function<void()> disconnect_callback;
    std::jthread reader_thread;
};

class UsbScanner final : public ISwitch2Scanner {
public:
    ~UsbScanner() override {
        Stop();
    }

    void Start(TransportDiscoveredCallback on_discovered) override {
        Stop();

        auto ctx = std::make_shared<LibUsbContext>();
        if (ctx->InitResult() != LIBUSB_SUCCESS || ctx->Get() == nullptr) {
            LOG_ERROR(Input, "Switch2 USB: libusb_init failed with error = {}", ctx->InitResult());
            return;
        }

        {
            std::scoped_lock lk{state_mutex};
            usb_ctx = std::move(ctx);
            discovered_callback = std::move(on_discovered);
            known_transports.clear();
        }

        scan_thread = std::jthread([this](std::stop_token stop_token) { ScanLoop(stop_token); });
    }

    void Stop() override {
        scan_thread = {};

        std::scoped_lock lk{state_mutex};
        known_transports.clear();
        discovered_callback = nullptr;
        usb_ctx.reset();
    }

private:
    void ScanLoop(std::stop_token stop_token) {
        Common::SetCurrentThreadName("Switch2UsbScan");
        do {
            ScanOnce(stop_token);
        } while (Common::StoppableTimedWait(stop_token, ScanInterval));
    }

    void PruneDisconnected() {
        for (auto it = known_transports.begin(); it != known_transports.end();) {
            const auto locked = it->second.lock();
            if (!locked || !locked->IsConnected()) {
                it = known_transports.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ScanOnce(std::stop_token stop_token) {
        std::shared_ptr<LibUsbContext> ctx;
        TransportDiscoveredCallback callback;
        {
            std::scoped_lock lk{state_mutex};
            ctx = usb_ctx;
            callback = discovered_callback;
        }

        if (!ctx || ctx->Get() == nullptr) {
            return;
        }

        const DeviceListGuard device_list(ctx->Get());
        if (device_list.Count() < 0) {
            LOG_DEBUG(Input, "Switch2 USB: libusb_get_device_list failed: {}",
                      libusb_error_name(static_cast<int>(device_list.Count())));
            return;
        }

        {
            std::scoped_lock lk{state_mutex};
            PruneDisconnected();
        }

        for (DeviceListCount i = 0; i < device_list.Count(); ++i) {
            if (stop_token.stop_requested()) {
                break;
            }

            libusb_device* dev = device_list.At(i);
            if (dev == nullptr) {
                continue;
            }

            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) {
                continue;
            }

            if (desc.idVendor != NintendoVendorId || !IsSupportedProductId(desc.idProduct)) {
                continue;
            }

            const u8 bus = libusb_get_bus_number(dev);
            const u8 addr = libusb_get_device_address(dev);
            const std::string address = fmt::format("usb:{}:{}", bus, addr);

            {
                std::scoped_lock lk{state_mutex};
                const auto it = known_transports.find(address);
                if (it != known_transports.end()) {
                    const auto existing = it->second.lock();
                    if (existing && existing->IsConnected()) {
                        continue;
                    }
                }
            }

            auto transport = Switch2UsbTransport::Open(ctx, dev, static_cast<ProductId>(desc.idProduct),
                                                       address);
            if (!transport) {
                continue;
            }

            LOG_INFO(Input, "Switch2 USB: connected {} ({:04x}:{:04x}) at {}",
                     ProductName(transport->GetProductId()), desc.idVendor, desc.idProduct, address);

            {
                std::scoped_lock lk{state_mutex};
                known_transports.insert_or_assign(address, transport);
            }

            if (callback != nullptr) {
                callback(std::move(transport));
            }
        }
    }

    std::mutex state_mutex;
    std::shared_ptr<LibUsbContext> usb_ctx;
    TransportDiscoveredCallback discovered_callback;
    std::unordered_map<std::string, std::weak_ptr<Switch2UsbTransport>> known_transports;
    std::jthread scan_thread;
};

#endif

} // namespace

std::unique_ptr<ISwitch2Scanner> CreateUsbScanner() {
#ifdef ENABLE_LIBUSB
    return std::make_unique<UsbScanner>();
#else
    return std::make_unique<NoopUsbScanner>();
#endif
}

} // namespace InputCommon::Switch2
