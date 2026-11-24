// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "input_common/drivers/switch2_auto_pair.h"

namespace InputCommon::Switch2 {
namespace {

constexpr char EngineKey[] = "engine";
constexpr char Switch2EngineName[] = "switch2";
constexpr char PortKey[] = "port";
constexpr char PadKey[] = "pad";
constexpr char GuidKey[] = "guid";
constexpr char DisplayKey[] = "display";

using Common::ParamPackage;

/// The serialized form of a cleared binding. A configuration never holds an empty string,
/// because some frontend libraries read it as "not set".
const std::string& EmptyBinding() {
    static const std::string empty = Common::ParamPackage{}.Serialize();
    return empty;
}

/// Parses a stored binding. Configurations may hold an empty string, which is treated as a
/// cleared binding instead of an invalid one.
ParamPackage ParseBinding(const std::string& serialized) {
    if (serialized.empty()) {
        return ParamPackage{};
    }
    return ParamPackage(serialized);
}

/// Returns true when the binding belongs to the Switch 2 driver.
bool IsSwitch2Engine(const ParamPackage& params) {
    return params.Get(EngineKey, "") == Switch2EngineName;
}

/// Everything the pairing policy needs to know about one controller.
struct DeviceContext {
    /// Stable identity of the controller: its serial number, or the transport address.
    std::string identity;
    /// Human readable name of the controller.
    std::string name;
    /// The driver slot the controller lives in.
    std::string port;
    /// The pad values the default mapping of this controller uses.
    std::vector<std::string> pads;
    /// The physical type of the controller.
    Switch2::ControllerType type{ControllerType::None};
    /// The default mappings of the controller.
    ButtonMapping buttons;
    AnalogMapping analogs;
    MotionMapping motions;
};

DeviceContext BuildDeviceContext(const Switch2DeviceBinding& device,
                                  const DeviceMappings& mappings) {
    DeviceContext context;
    context.identity = device.params.Get(DeviceIdKey, "");
    if (context.identity.empty()) {
        context.identity = device.info.address;
    }
    context.name = device.info.display_name;
    if (context.name.empty()) {
        context.name = device.params.Get(DisplayKey, "");
    }
    context.port = device.params.Get(PortKey, "");
    context.type = device.info.type;
    context.buttons = mappings.buttons(device.params);
    context.analogs = mappings.analogs(device.params);
    context.motions = mappings.motions(device.params);

    for (const auto& entry : context.buttons) {
        const auto pad = entry.second.Get(PadKey, "");
        if (pad.empty()) {
            continue;
        }
        if (std::find(context.pads.begin(), context.pads.end(), pad) == context.pads.end()) {
            context.pads.push_back(pad);
        }
    }

    return context;
}

std::vector<DeviceContext> BuildDeviceContexts(const std::vector<Switch2DeviceBinding>& devices,
                                                const DeviceMappings& mappings) {
    std::vector<DeviceContext> contexts;
    contexts.reserve(devices.size());
    for (const auto& device : devices) {
        contexts.push_back(BuildDeviceContext(device, mappings));
    }
    return contexts;
}

/// Returns the player slot that already owns the controller, if there is one.
///
/// A binding that carries the identity of the controller owns the slot. Bindings that were
/// written before the identity was stored are matched by the driver slot and the pad the
/// default mapping expects.
std::optional<std::size_t> FindOwner(const std::vector<Settings::PlayerInput>& players,
                                      const DeviceContext& context) {
    if (!context.identity.empty()) {
        for (std::size_t index = 0; index < players.size(); ++index) {
            for (const auto& binding : players[index].buttons) {
                const auto params = ParseBinding(binding);
                if (IsSwitch2Engine(params) && params.Get(DeviceIdKey, "") == context.identity) {
                    return index;
                }
            }
        }
    }

    if (!context.port.empty() && !context.pads.empty()) {
        for (std::size_t index = 0; index < players.size(); ++index) {
            for (const auto& binding : players[index].buttons) {
                const auto params = ParseBinding(binding);
                if (!IsSwitch2Engine(params) || !params.Get(DeviceIdKey, "").empty() ||
                    params.Get(PortKey, "") != context.port) {
                    continue;
                }
                const auto pad = params.Get(PadKey, "");
                const auto found =
                    std::find(context.pads.begin(), context.pads.end(), pad) != context.pads.end();
                if (found) {
                    return index;
                }
            }
        }
    }

    return std::nullopt;
}

/// Returns true when the stored binding no longer matches the default mapping, which happens
/// when the driver moved the controller to another slot.
bool IsStaleBinding(const ParamPackage& stored, const ParamPackage& expected) {
    return stored.Get(PortKey, "") != expected.Get(PortKey, "") ||
           stored.Get(PadKey, "") != expected.Get(PadKey, "") ||
           stored.Get(GuidKey, "") != expected.Get(GuidKey, "");
}

/// Writes the default mapping of a controller into a player slot. Entries the mapping does not
/// provide are cleared, so that the controller fully owns the slot.
template <typename Raw, typename Mapping>
void WriteEntries(Raw& raw, const Mapping& mapping) {
    std::array<bool, std::tuple_size_v<Raw>> provided{};

    for (const auto& entry : mapping) {
        const auto index = static_cast<std::size_t>(entry.first);
        if (index >= raw.size()) {
            continue;
        }
        raw[index] = entry.second.Serialize();
        provided[index] = true;
    }

    for (std::size_t index = 0; index < raw.size(); ++index) {
        if (!provided[index]) {
            raw[index] = EmptyBinding();
        }
    }
}

/// Rewrites the entries of a player slot that still use the Switch 2 driver. An entry the user
/// pointed at the keyboard or at another device is left alone. Returns true when the slot was
/// changed.
template <typename Raw, typename Mapping>
bool RewriteEntries(Raw& raw, const Mapping& mapping, bool restore_missing) {
    using Key = typename Mapping::key_type;
    bool changed = false;

    for (std::size_t index = 0; index < raw.size(); ++index) {
        const auto entry = mapping.find(static_cast<Key>(static_cast<int>(index)));
        if (raw[index].empty() || raw[index] == EmptyBinding()) {
            if (restore_missing && entry != mapping.end()) {
                raw[index] = entry->second.Serialize();
                changed = true;
            }
            continue;
        }

        const auto stored = ParseBinding(raw[index]);
        if (!IsSwitch2Engine(stored)) {
            continue;
        }

        if (entry == mapping.end()) {
            // The controller does not provide this input anymore.
            raw[index] = EmptyBinding();
            changed = true;
            continue;
        }

        if (IsStaleBinding(stored, entry->second)) {
            raw[index] = entry->second.Serialize();
            changed = true;
        }
    }

    return changed;
}

/// Rewrites the Switch 2 entries of a player slot. Returns true when the slot was changed.
bool RewriteSwitch2Entries(Settings::PlayerInput& player, const DeviceContext& context) {
    const bool type_changed =
        player.controller_type != Switch2AutoPair::ControllerTypeFor(context.type);
    bool changed = false;
    changed |= RewriteEntries(player.buttons, context.buttons, type_changed);
    changed |= RewriteEntries(player.analogs, context.analogs, type_changed);
    changed |= RewriteEntries(player.motions, context.motions, type_changed);
    return changed;
}

std::optional<std::size_t> FindTargetSlot(const std::vector<bool>& claimed,
                                           const std::vector<bool>& bound,
                                           const std::vector<bool>& owned_by_present,
                                           const std::vector<Settings::PlayerInput>& players,
                                           bool player_one_uses_default_keyboard) {
    if (player_one_uses_default_keyboard && !claimed.empty() && !claimed.front() &&
        !bound.front()) {
        return 0;
    }

    for (std::size_t index = 0; index < claimed.size(); ++index) {
        if (!claimed[index] && !bound[index] && !players[index].connected) {
            return index;
        }
    }

    for (std::size_t index = 0; index < claimed.size(); ++index) {
        if (!claimed[index] && bound[index] && !owned_by_present[index]) {
            return index;
        }
    }

    for (std::size_t index = 0; index < claimed.size(); ++index) {
        if (!claimed[index] && !bound[index]) {
            return index;
        }
    }

    return std::nullopt;
}

/// The pairing policy on the given controllers and player slots.
std::vector<AutoPairPlayer> ApplyContexts(const std::vector<DeviceContext>& contexts,
                                          std::vector<Settings::PlayerInput>& players,
                                          bool player_one_uses_default_keyboard) {
    std::vector<AutoPairPlayer> changed;

    // The owners are resolved before anything is written, so that a controller that takes over
    // a slot cannot hide the owner of another controller.
    std::vector<std::optional<std::size_t>> owners;
    owners.reserve(contexts.size());
    for (const auto& context : contexts) {
        owners.push_back(FindOwner(players, context));
    }

    std::vector<bool> claimed(players.size(), false);
    std::vector<bool> bound(players.size(), false);
    std::vector<bool> owned_by_present(players.size(), false);
    for (std::size_t index = 0; index < players.size(); ++index) {
        bound[index] = Switch2AutoPair::IsSwitch2Player(players[index]);
    }
    for (const auto& owner : owners) {
        if (owner) {
            owned_by_present[*owner] = true;
        }
    }

    for (std::size_t index = 0; index < contexts.size(); ++index) {
        const auto& context = contexts[index];
        auto owner = owners[index];
        if (owner && claimed[*owner]) {
            owner = std::nullopt;
        }

        if (owner) {
            claimed[*owner] = true;
            auto& player = players[*owner];
            const bool was_connected = player.connected;
            if (RewriteSwitch2Entries(player, context)) {
                player.controller_type = Switch2AutoPair::ControllerTypeFor(context.type);
                player.connected = true;
                changed.push_back({*owner, context.name, context.identity, !was_connected, false});
            } else if (!player.connected) {
                player.controller_type = Switch2AutoPair::ControllerTypeFor(context.type);
                player.connected = true;
                changed.push_back({*owner, context.name, context.identity, true, false});
            }
            continue;
        }

        const auto target =
            FindTargetSlot(claimed, bound, owned_by_present, players, player_one_uses_default_keyboard);
        if (!target) {
            LOG_WARNING(Input, "Switch 2 auto pair: no free player slot for {} ({})", context.name,
                        context.identity);
            continue;
        }

        claimed[*target] = true;
        auto& player = players[*target];
        WriteEntries(player.buttons, context.buttons);
        WriteEntries(player.analogs, context.analogs);
        WriteEntries(player.motions, context.motions);
        player.controller_type = Switch2AutoPair::ControllerTypeFor(context.type);
        player.connected = true;
        changed.push_back({*target, context.name, context.identity, false, false});
    }

    return changed;
}

/// Returns the identity of the controller that a player slot is bound to.
std::string SlotIdentity(const Settings::PlayerInput& player, std::size_t slot,
                         const std::vector<DeviceContext>& contexts,
                         const std::vector<std::optional<std::size_t>>& owners,
                         const std::unordered_map<std::string, std::size_t>& device_players) {
    // The identity that is stored in the binding is the most reliable one.
    for (const auto& binding : player.buttons) {
        const auto params = ParseBinding(binding);
        if (!IsSwitch2Engine(params)) {
            continue;
        }
        const auto device_id = params.Get(DeviceIdKey, "");
        if (!device_id.empty()) {
            return device_id;
        }
    }

    // Then the controller that owns the slot in this pass.
    for (std::size_t index = 0; index < contexts.size(); ++index) {
        if (owners[index] && *owners[index] == slot) {
            return contexts[index].identity;
        }
    }

    if (!Switch2AutoPair::IsSwitch2Player(player)) {
        return {};
    }

    // Then the identity the slot had in an earlier pass.
    for (const auto& entry : device_players) {
        if (entry.second == slot) {
            return entry.first;
        }
    }

    // Finally the driver slot the binding was written for.
    for (const auto& binding : player.buttons) {
        const auto params = ParseBinding(binding);
        if (!IsSwitch2Engine(params)) {
            continue;
        }
        const auto port = params.Get(PortKey, "");
        if (!port.empty()) {
            return "port:" + port;
        }
    }

    return {};
}

} // namespace

std::vector<AutoPairPlayer> Switch2AutoPair::Apply(Switch2Controllers& engine,
                                                 bool player_one_uses_default_keyboard) {
    if (!Settings::values.switch2_auto_pair.GetValue()) {
        return {};
    }

    const auto devices = engine.GetDeviceBindings();

    const auto& active_players = Settings::values.players.GetValue();
    std::vector<Settings::PlayerInput> players(active_players.begin(), active_players.end());

    const DeviceMappings mappings{
        .buttons = [&engine](const Common::ParamPackage& params) {
            return engine.GetButtonMappingForDevice(params);
        },
        .analogs = [&engine](const Common::ParamPackage& params) {
            return engine.GetAnalogMappingForDevice(params);
        },
        .motions = [&engine](const Common::ParamPackage& params) {
            return engine.GetMotionMappingForDevice(params);
        },
    };

    auto changed = ApplyDevices(devices, mappings, players, player_one_uses_default_keyboard);

    // Store the result in the active as well as in the global configuration, so that the running
    // game and the persisted configuration agree.
    auto& active = Settings::values.players.GetValue();
    auto& global = Settings::values.players.GetValue(true);
    for (const auto& change : changed) {
        if (change.player_index >= players.size()) {
            continue;
        }
        active[change.player_index] = players[change.player_index];
        global[change.player_index] = players[change.player_index];
    }

    for (const auto& change : changed) {
        if (change.disconnected) {
            LOG_INFO(Input, "Switch 2 auto pair: {} ({}) left player {}", change.device_name,
                     change.device_id, change.player_index + 1);
        } else if (change.reconnected) {
            LOG_INFO(Input, "Switch 2 auto pair: {} ({}) reconnected to player {}",
                     change.device_name, change.device_id, change.player_index + 1);
        } else {
            LOG_INFO(Input, "Switch 2 auto pair: bound {} ({}) to player {}", change.device_name,
                     change.device_id, change.player_index + 1);
        }
    }

    return changed;
}

std::vector<AutoPairPlayer> Switch2AutoPair::ApplyDevices(
    const std::vector<Switch2DeviceBinding>& devices, const DeviceMappings& mappings,
    std::vector<Settings::PlayerInput>& players, bool player_one_uses_default_keyboard) {
    if (!mappings.buttons || !mappings.analogs || !mappings.motions) {
        LOG_WARNING(Input, "Switch 2 auto pair: the input engine did not provide the mappings");
        return {};
    }

    const auto contexts = BuildDeviceContexts(devices, mappings);
    auto changed = ApplyContexts(contexts, players, player_one_uses_default_keyboard);

    // Rebuild the map of controller identity to player slot.
    std::vector<std::optional<std::size_t>> owners;
    owners.reserve(contexts.size());
    for (const auto& context : contexts) {
        owners.push_back(FindOwner(players, context));
    }
    std::vector<bool> present(players.size(), false);
    for (const auto& owner : owners) {
        if (owner) {
            present[*owner] = true;
        }
    }

    std::unordered_map<std::string, std::size_t> identity_slots;
    for (std::size_t index = 0; index < players.size(); ++index) {
        const auto identity = SlotIdentity(players[index], index, contexts, owners, device_players);
        if (identity.empty()) {
            continue;
        }
        identity_slots[identity] = index;
    }

    // A controller that is seen resets the counter of the passes it was missing.
    for (const auto& context : contexts) {
        missing_passes.erase(context.identity);
    }

    for (const auto& entry : identity_slots) {
        if (present[entry.second]) {
            missing_passes.erase(entry.first);
            continue;
        }

        const auto counter = missing_passes.find(entry.first);
        const u32 count = counter == missing_passes.end() ? 1U : counter->second + 1U;
        if (count < MissingPassesBeforeDisconnect) {
            missing_passes[entry.first] = count;
            continue;
        }

        missing_passes.erase(entry.first);
        if (!players[entry.second].connected) {
            continue;
        }
        players[entry.second].connected = false;
        changed.push_back({entry.second, entry.first, entry.first, false, true});
    }

    // Forget the controllers that are not bound to a player slot anymore.
    for (auto it = missing_passes.begin(); it != missing_passes.end();) {
        if (identity_slots.find(it->first) == identity_slots.end()) {
            it = missing_passes.erase(it);
        } else {
            ++it;
        }
    }
    device_players = identity_slots;

    return changed;
}

std::vector<AutoPairPlayer> Switch2AutoPair::ApplyToPlayers(
    const std::vector<Switch2DeviceBinding>& devices, const DeviceMappings& mappings,
    std::vector<Settings::PlayerInput>& players, bool player_one_uses_default_keyboard) {
    if (!mappings.buttons || !mappings.analogs || !mappings.motions) {
        LOG_WARNING(Input, "Switch 2 auto pair: the input engine did not provide the mappings");
        return {};
    }

    return ApplyContexts(BuildDeviceContexts(devices, mappings), players,
                         player_one_uses_default_keyboard);
}

bool Switch2AutoPair::IsSwitch2Player(const Settings::PlayerInput& player) {
    for (const auto& binding : player.buttons) {
        if (IsSwitch2Engine(ParseBinding(binding))) {
            return true;
        }
    }
    return false;
}

Settings::ControllerType Switch2AutoPair::ControllerTypeFor(Switch2::ControllerType type) {
    switch (type) {
    case Switch2::ControllerType::Pro:
        return Settings::ControllerType::ProController;
    case Switch2::ControllerType::Dual:
        return Settings::ControllerType::DualJoyconDetached;
    case Switch2::ControllerType::Left:
        return Settings::ControllerType::LeftJoycon;
    case Switch2::ControllerType::Right:
        return Settings::ControllerType::RightJoycon;
    case Switch2::ControllerType::GameCube:
        return Settings::ControllerType::GameCube;
    case Switch2::ControllerType::None:
    default:
        return Settings::ControllerType::ProController;
    }
}

} // namespace InputCommon::Switch2
