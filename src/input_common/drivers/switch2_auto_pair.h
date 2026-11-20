// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/param_package.h"
#include "common/settings_input.h"
#include "input_common/drivers/switch2.h"
#include "input_common/main.h"

namespace InputCommon::Switch2 {

/// A player slot that the auto-pair pass changed.
struct AutoPairPlayer {
    /// Zero based index into Settings::values.players.
    std::size_t player_index{};
    /// Display name of the controller that owns the slot.
    std::string device_name;
    /// Stable identity of the controller: its serial number, or the transport address.
    std::string device_id;
    /// True when the slot already pointed at this controller and only had to be re-enabled.
    bool reconnected{false};
    /// True when the controller went away and the slot was marked as disconnected.
    bool disconnected{false};
};

/// The default mappings of a Switch 2 controller, as produced by the input engine.
struct DeviceMappings {
    std::function<ButtonMapping(const Common::ParamPackage&)> buttons;
    std::function<AnalogMapping(const Common::ParamPackage&)> analogs;
    std::function<MotionMapping(const Common::ParamPackage&)> motions;
};

/// Binds the Switch 2 controllers that the driver found to emulated player slots.
///
/// An unassigned controller prefers Player 1 when that slot uses the factory keyboard mapping.
/// Otherwise, it prefers the lowest available slot. The pass is idempotent:
/// running it again without a device change reports no change. It only writes
/// Settings::values.players; reloading the emulated controllers and persisting the
/// configuration stays with the caller.
class Switch2AutoPair {
public:
    /// Number of consecutive passes a controller may be missing before the player slot it
    /// owned is marked as disconnected.
    static constexpr u32 MissingPassesBeforeDisconnect = 3;

    Switch2AutoPair() = default;

    /// Binds the controllers the engine currently has to player slots and stores the result in
    /// Settings::values.players, in the active as well as in the global configuration.
    /// Returns the player slots that changed.
    [[nodiscard]] std::vector<AutoPairPlayer> Apply(
        Switch2Controllers& engine, bool player_one_uses_default_keyboard = false);

    /// Binds the given controllers to the given player slots, in place, and keeps the state of
    /// the controllers between the calls: a controller that is missing for several passes in a
    /// row has its player slot marked as disconnected.
    ///
    /// The pass has to run on the same object every time. A fresh object starts with an empty
    /// state, and a controller would then never be reported as gone.
    [[nodiscard]] std::vector<AutoPairPlayer> ApplyDevices(
        const std::vector<Switch2DeviceBinding>& devices, const DeviceMappings& mappings,
        std::vector<Settings::PlayerInput>& players, bool player_one_uses_default_keyboard = false);

    /// The pairing policy on its own: applies the given controllers to the given player slots.
    /// The player slots are modified in place. Returns the player slots that changed.
    [[nodiscard]] static std::vector<AutoPairPlayer> ApplyToPlayers(
        const std::vector<Switch2DeviceBinding>& devices, const DeviceMappings& mappings,
        std::vector<Settings::PlayerInput>& players, bool player_one_uses_default_keyboard = false);

    /// Returns true when the player is bound to a Switch 2 controller.
    [[nodiscard]] static bool IsSwitch2Player(const Settings::PlayerInput& player);

    /// Maps the type of a physical controller onto the emulated controller type.
    [[nodiscard]] static Settings::ControllerType ControllerTypeFor(Switch2::ControllerType type);

private:
    /// Maps the identity of a controller onto the player slot that owns it.
    std::unordered_map<std::string, std::size_t> device_players;
    /// Counts the passes a controller has been missing.
    std::unordered_map<std::string, u32> missing_passes;
};

} // namespace InputCommon::Switch2
