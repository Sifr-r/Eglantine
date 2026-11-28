// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/gpui_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "input_common/helpers/switch2_protocol/switch2_driver.h"
#include "input_common/helpers/switch2_protocol/switch2_transport.h"
#include "input_common/helpers/switch2_protocol/switch2_types.h"
#include "yuzu_cmd/gpui_session.h"

namespace Gpui {
namespace {

using namespace InputCommon::Switch2;

constexpr auto PollInterval = std::chrono::milliseconds{16};
constexpr float StickEmitDelta = 0.02f;

struct ControllerBridgeState {
    std::mutex mutex;
    bool stopped{false};
    std::vector<std::shared_ptr<Switch2Driver>> drivers;
    std::vector<std::shared_ptr<Switch2Driver>> retired_drivers;
    std::optional<std::string> current_name;
    bool name_dirty{false};
    bool input_dirty{false};
    std::uint32_t last_buttons{0};
    float last_left_x{0.0f};
    float last_left_y{0.0f};
};

std::string DisplayNameForDriver(const Switch2Driver& driver) {
    return std::string{ProductName(driver.GetProductId())};
}

} // namespace

bool RunControllerBridge(Session& session) {
    auto state = std::make_shared<ControllerBridgeState>();

    auto ble_scanner = CreateBleScanner();
    auto usb_scanner = CreateUsbScanner();

    auto on_discovered = [state](std::shared_ptr<ISwitch2Transport> transport) {
        if (!transport) {
            return;
        }

        const std::string address = transport->GetAddress();
        u8 player_number = 1;
        {
            std::scoped_lock lock{state->mutex};
            if (state->stopped) {
                transport->Disconnect();
                return;
            }
            for (const auto& existing : state->drivers) {
                if (existing && existing->GetAddress() == address) {
                    return;
                }
            }
            player_number = static_cast<u8>(std::min<std::size_t>(state->drivers.size() + 1, 8));
        }

        auto driver = std::make_shared<Switch2Driver>(std::move(transport));
        if (driver->Initialize(player_number, false) != Common::Input::DriverResult::Success) {
            driver->Stop();
            return;
        }

        std::weak_ptr<ControllerBridgeState> weak_state = state;
        driver->SetCallbacks(Switch2Callbacks{
            .on_input_report =
                [weak_state, address](const Switch2InputReport& report) {
                    const auto locked_state = weak_state.lock();
                    if (!locked_state) {
                        return;
                    }
                    std::scoped_lock lock{locked_state->mutex};
                    if (locked_state->stopped || locked_state->drivers.empty() ||
                        locked_state->drivers.front()->GetAddress() != address) {
                        return;
                    }
                    const bool buttons_changed = report.buttons != locked_state->last_buttons;
                    const bool stick_changed =
                        std::abs(report.left_x - locked_state->last_left_x) >= StickEmitDelta ||
                        std::abs(report.left_y - locked_state->last_left_y) >= StickEmitDelta ||
                        (report.left_x == 0.0f && locked_state->last_left_x != 0.0f) ||
                        (report.left_y == 0.0f && locked_state->last_left_y != 0.0f);
                    if (buttons_changed || stick_changed) {
                        locked_state->last_buttons = report.buttons;
                        locked_state->last_left_x = report.left_x;
                        locked_state->last_left_y = report.left_y;
                        locked_state->input_dirty = true;
                    }
                },
            .on_controller_info = {},
            .on_disconnect =
                [weak_state, address]() {
                    const auto locked_state = weak_state.lock();
                    if (!locked_state) {
                        return;
                    }
                    std::scoped_lock lock{locked_state->mutex};
                    if (locked_state->stopped) {
                        return;
                    }
                    const bool was_primary =
                        !locked_state->drivers.empty() &&
                        locked_state->drivers.front()->GetAddress() == address;
                    for (auto it = locked_state->drivers.begin(); it != locked_state->drivers.end();) {
                        if (*it && (*it)->GetAddress() == address) {
                            locked_state->retired_drivers.push_back(std::move(*it));
                            it = locked_state->drivers.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    if (was_primary) {
                        locked_state->current_name =
                            locked_state->drivers.empty()
                                ? std::nullopt
                                : std::optional<std::string>{
                                      DisplayNameForDriver(*locked_state->drivers.front())};
                        locked_state->name_dirty = true;
                        if (locked_state->last_buttons != 0 || locked_state->last_left_x != 0.0f ||
                            locked_state->last_left_y != 0.0f) {
                            locked_state->last_buttons = 0;
                            locked_state->last_left_x = 0.0f;
                            locked_state->last_left_y = 0.0f;
                            locked_state->input_dirty = true;
                        }
                    }
                },
        });

        bool accepted = false;
        {
            std::scoped_lock lock{state->mutex};
            if (!state->stopped && driver->IsConnected()) {
                const bool duplicate = std::ranges::any_of(
                    state->drivers, [&address](const auto& existing) {
                        return existing && existing->GetAddress() == address;
                    });
                if (!duplicate) {
                    const bool became_primary = state->drivers.empty();
                    state->drivers.push_back(driver);
                    if (became_primary) {
                        state->current_name = DisplayNameForDriver(*driver);
                        state->name_dirty = true;
                    }
                    accepted = true;
                }
            }
        }

        if (!accepted) {
            driver->Stop();
        }
    };

    if (ble_scanner) {
        ble_scanner->Start(on_discovered);
    }
    if (usb_scanner) {
        usb_scanner->Start(on_discovered);
    }

    session.SetState(SessionState::Running);

    bool stop_requested = false;
    while (session.IsConnected() && !stop_requested) {
        std::vector<std::shared_ptr<Switch2Driver>> to_stop;
        {
            std::scoped_lock lock{state->mutex};
            to_stop = std::exchange(state->retired_drivers, {});
        }
        for (const auto& retired : to_stop) {
            if (retired) {
                retired->Stop();
            }
        }

        std::vector<Request> requests;
        if (!session.Poll(requests)) {
            break;
        }
        for (const auto& request : requests) {
            if (request.command == Command::Stop) {
                session.SetState(SessionState::Stopping);
                session.Reply(request.id);
                stop_requested = true;
                break;
            }
            session.Reply(request.id, "Unsupported command in controller bridge");
        }
        if (stop_requested) {
            break;
        }

        bool emit_name = false;
        std::optional<std::string> name_copy;
        bool emit_input = false;
        std::uint32_t buttons = 0;
        float left_x = 0.0f;
        float left_y = 0.0f;
        {
            std::scoped_lock lock{state->mutex};
            if (state->name_dirty) {
                emit_name = true;
                name_copy = state->current_name;
                state->name_dirty = false;
            }
            if (state->input_dirty) {
                emit_input = true;
                buttons = state->last_buttons;
                left_x = state->last_left_x;
                left_y = state->last_left_y;
                state->input_dirty = false;
            }
        }

        if (emit_name) {
            session.ControllerState(name_copy);
        }
        if (emit_input) {
            session.ControllerInput(buttons, left_x, left_y);
        }

        std::this_thread::sleep_for(PollInterval);
    }

    std::vector<std::shared_ptr<Switch2Driver>> remaining;
    {
        std::scoped_lock lock{state->mutex};
        state->stopped = true;
        remaining = std::exchange(state->drivers, {});
        auto retired = std::exchange(state->retired_drivers, {});
        remaining.insert(remaining.end(), std::make_move_iterator(retired.begin()),
                         std::make_move_iterator(retired.end()));
    }

    if (ble_scanner) {
        ble_scanner->Stop();
    }
    if (usb_scanner) {
        usb_scanner->Stop();
    }
    for (const auto& driver : remaining) {
        if (driver) {
            driver->Stop();
        }
    }

    session.SetState(SessionState::Stopped);
    return true;
}

} // namespace Gpui
