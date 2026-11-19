// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/param_package.h"
#include "common/settings_input.h"
#include "input_common/drivers/switch2.h"
#include "input_common/drivers/switch2_auto_pair.h"
#include "input_common/helpers/switch2_protocol/switch2_types.h"
#include "input_common/main.h"

using namespace InputCommon::Switch2;

namespace {

/// Maps the driver slot a controller sits in onto its stable device id.
using DeviceIdMap = std::vector<std::pair<std::size_t, std::string>>;

constexpr std::size_t ButtonIndex(Settings::NativeButton::Values button) {
    return static_cast<std::size_t>(button);
}

constexpr std::size_t AnalogIndex(Settings::NativeAnalog::Values analog) {
    return static_cast<std::size_t>(analog);
}

constexpr std::size_t MotionIndex(Settings::NativeMotion::Values motion) {
    return static_cast<std::size_t>(motion);
}

/// Builds the guid the driver hands out for a controller in a driver slot.
std::string MakeGuid(std::size_t slot, ControllerType type) {
    std::array<u8, 16> guid{};
    guid[14] = static_cast<u8>(slot);
    guid[15] = static_cast<u8>(type);
    constexpr char hex_digits[] = "0123456789abcdef";
    std::string guid_string;
    guid_string.reserve(guid.size() * 2U);
    for (const u8 byte : guid) {
        guid_string.push_back(hex_digits[byte >> 4]);
        guid_string.push_back(hex_digits[byte & 0x0FU]);
    }
    return guid_string;
}

/// Builds the parameter package that addresses a controller.
Common::ParamPackage MakeDeviceParams(std::size_t slot, ControllerType type,
                                       const std::string& device_id) {
    return Common::ParamPackage{
        {"engine", "switch2"},
        {"guid", MakeGuid(slot, type)},
        {"port", std::to_string(slot)},
        {"pad", std::to_string(static_cast<int>(type))},
        {"device_id", device_id},
    };
}

/// Builds the parameter package of a single button binding.
Common::ParamPackage MakeBindingParams(std::size_t slot, ControllerType type,
                                       const std::string& device_id, int button) {
    Common::ParamPackage params = MakeDeviceParams(slot, type, device_id);
    params.Set("button", button);
    return params;
}

/// Builds the serialized binding of the A button of a controller that owns a player slot.
std::string MakeOwnedBinding(std::size_t slot, ControllerType type,
                             const std::string& device_id) {
    return MakeBindingParams(slot, type, device_id, static_cast<int>(PadButton::A)).Serialize();
}

/// Builds the parameter package of a keyboard button, as the user remapping leaves it.
std::string MakeKeyboardBinding() {
    Common::ParamPackage params{};
    params.Set("engine", "keyboard");
    params.Set("button", 4);
    return params.Serialize();
}

ProductId ProductIdFor(ControllerType type) {
    switch (type) {
    case ControllerType::Left:
        return ProductId::JoyCon2Left;
    case ControllerType::Right:
        return ProductId::JoyCon2Right;
    case ControllerType::GameCube:
        return ProductId::NsoGameCube;
    default:
        return ProductId::ProController2;
    }
}

/// Builds a controller that the driver found.
InputCommon::Switch2DeviceBinding MakeDevice(std::size_t slot, ControllerType type,
                                             const std::string& device_id,
                                             const std::string& display_name) {
    InputCommon::Switch2DeviceBinding binding{};
    binding.info.slot_index = slot;
    binding.info.type = type;
    binding.info.product_id = ProductIdFor(type);
    binding.info.transport = TransportType::Usb;
    binding.info.address = "usb:" + std::to_string(slot);
    binding.info.display_name = display_name;
    binding.params = MakeDeviceParams(slot, type, device_id);
    return binding;
}

/// Builds an empty player slot.
Settings::PlayerInput MakePlayer() {
    Settings::PlayerInput player{};
    player.connected = false;
    player.controller_type = Settings::ControllerType::ProController;
    player.vibration_enabled = true;
    player.vibration_strength = 100;
    player.body_color_left = 0U;
    player.body_color_right = 0U;
    player.button_color_left = 0U;
    player.button_color_right = 0U;
    player.profile_name = "default";
    player.use_system_vibrator = false;
    return player;
}

/// Copies the parameters of a controller and makes sure they carry the device id.
Common::ParamPackage MappingParams(const Common::ParamPackage& params,
                                    const DeviceIdMap& device_ids) {
    Common::ParamPackage result = params;
    if (!result.Has("device_id")) {
        const auto port = static_cast<std::size_t>(result.Get("port", 0));
        for (const auto& [id_port, device_id] : device_ids) {
            if (id_port == port) {
                result.Set("device_id", device_id);
                break;
            }
        }
    }
    return result;
}

/// Builds the mapping provider of the input engine.
///
/// The provider covers the A and B buttons, the two sticks and the two motion sensors.
/// A partial provider covers the left stick and the left motion sensor only, like the
/// mapping of a single Joy-Con does.
DeviceMappings MakeMappings(const DeviceIdMap& device_ids, bool partial = false) {
    DeviceMappings mappings{};

    mappings.buttons = [device_ids](const Common::ParamPackage& params) {
        InputCommon::ButtonMapping mapping{};
        Common::ParamPackage a_params = MappingParams(params, device_ids);
        a_params.Set("button", static_cast<int>(PadButton::A));
        mapping.insert_or_assign(Settings::NativeButton::A, std::move(a_params));
        Common::ParamPackage b_params = MappingParams(params, device_ids);
        b_params.Set("button", static_cast<int>(PadButton::B));
        mapping.insert_or_assign(Settings::NativeButton::B, std::move(b_params));
        return mapping;
    };

    mappings.analogs = [device_ids, partial](const Common::ParamPackage& params) {
        InputCommon::AnalogMapping mapping{};
        Common::ParamPackage left_params = MappingParams(params, device_ids);
        left_params.Set("axis_x", static_cast<int>(PadAxes::LeftStickX));
        left_params.Set("axis_y", static_cast<int>(PadAxes::LeftStickY));
        mapping.insert_or_assign(Settings::NativeAnalog::LStick, std::move(left_params));
        if (!partial) {
            Common::ParamPackage right_params = MappingParams(params, device_ids);
            right_params.Set("axis_x", static_cast<int>(PadAxes::RightStickX));
            right_params.Set("axis_y", static_cast<int>(PadAxes::RightStickY));
            mapping.insert_or_assign(Settings::NativeAnalog::RStick, std::move(right_params));
        }
        return mapping;
    };

    mappings.motions = [device_ids, partial](const Common::ParamPackage& params) {
        InputCommon::MotionMapping mapping{};
        Common::ParamPackage left_params = MappingParams(params, device_ids);
        left_params.Set("motion", static_cast<int>(PadMotion::LeftMotion));
        mapping.insert_or_assign(Settings::NativeMotion::MotionLeft, std::move(left_params));
        if (!partial) {
            Common::ParamPackage right_params = MappingParams(params, device_ids);
            right_params.Set("motion", static_cast<int>(PadMotion::RightMotion));
            mapping.insert_or_assign(Settings::NativeMotion::MotionRight, std::move(right_params));
        }
        return mapping;
    };

    return mappings;
}

/// Returns true when the pass reported the given player slot for the given controller.
bool AnyPlayer(const std::vector<AutoPairPlayer>& result, std::size_t player_index,
               const std::string& device_id) {
    for (const auto& player : result) {
        if (player.player_index == player_index && player.device_id == device_id) {
            return true;
        }
    }
    return false;
}

/// Returns the device id of the Switch 2 controller a player slot is bound to.
std::string DeviceIdOf(const Settings::PlayerInput& player) {
    for (const auto& button : player.buttons) {
        const Common::ParamPackage params{button};
        if (params.Get("engine", "") == "switch2") {
            return params.Get("device_id", "");
        }
    }
    return {};
}

} // namespace

TEST_CASE("Switch2AutoPair claims the lowest free player slot", "[switch2][auto_pair]") {
    std::vector<Settings::PlayerInput> players(std::size_t{3}, MakePlayer());
    const std::vector<InputCommon::Switch2DeviceBinding> devices{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});

    const auto result = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

    REQUIRE(result.size() == std::size_t{1});
    REQUIRE(result[0].player_index == std::size_t{0});
    REQUIRE(result[0].device_id == "HAC-001");
    REQUIRE_FALSE(result[0].reconnected);
    REQUIRE_FALSE(result[0].disconnected);

    REQUIRE(players[0].connected);
    REQUIRE(players[0].controller_type == Settings::ControllerType::ProController);
    REQUIRE_FALSE(players[1].connected);
    REQUIRE_FALSE(players[2].connected);
}

TEST_CASE("Switch2AutoPair writes the mappings and clears the uncovered entries",
          "[switch2][auto_pair]") {
    std::vector<Settings::PlayerInput> players(std::size_t{1}, MakePlayer());
    const std::vector<InputCommon::Switch2DeviceBinding> devices{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };

    SECTION("The button, analog and motion mappings end up in the player slot") {
        const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});
        REQUIRE(Switch2AutoPair::ApplyToPlayers(devices, mappings, players).size() ==
                std::size_t{1});

        const Common::ParamPackage button_a{
            players[0].buttons[ButtonIndex(Settings::NativeButton::A)]};
        REQUIRE(button_a.Get("engine", "") == "switch2");
        REQUIRE(button_a.Get("guid", "") == MakeGuid(std::size_t{0}, ControllerType::Pro));
        REQUIRE(button_a.Get("port", "") == "0");
        REQUIRE(button_a.Get("pad", "") == "3");
        REQUIRE(button_a.Get("device_id", "") == "HAC-001");
        REQUIRE(button_a.Get("button", -1) == static_cast<int>(PadButton::A));

        const Common::ParamPackage analog_left{
            players[0].analogs[AnalogIndex(Settings::NativeAnalog::LStick)]};
        REQUIRE(analog_left.Get("engine", "") == "switch2");
        REQUIRE(analog_left.Get("axis_x", -1) == static_cast<int>(PadAxes::LeftStickX));
        REQUIRE(analog_left.Get("axis_y", -1) == static_cast<int>(PadAxes::LeftStickY));

        const Common::ParamPackage motion_left{
            players[0].motions[MotionIndex(Settings::NativeMotion::MotionLeft)]};
        REQUIRE(motion_left.Get("engine", "") == "switch2");
        REQUIRE(motion_left.Get("motion", -1) == static_cast<int>(PadMotion::LeftMotion));
    }

    SECTION("The entries the provider does not cover are cleared") {
        const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}}, true);
        REQUIRE(Switch2AutoPair::ApplyToPlayers(devices, mappings, players).size() ==
                std::size_t{1});

        REQUIRE(players[0].buttons[ButtonIndex(Settings::NativeButton::X)] ==
                Common::ParamPackage{}.Serialize());
        REQUIRE(players[0].analogs[AnalogIndex(Settings::NativeAnalog::RStick)] ==
                Common::ParamPackage{}.Serialize());
        REQUIRE(players[0].motions[MotionIndex(Settings::NativeMotion::MotionRight)] ==
                Common::ParamPackage{}.Serialize());
    }
}

TEST_CASE("Switch2AutoPair reports no change on a second pass", "[switch2][auto_pair]") {
    std::vector<Settings::PlayerInput> players(std::size_t{2}, MakePlayer());
    const std::vector<InputCommon::Switch2DeviceBinding> devices{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});

    const auto first = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);
    REQUIRE(first.size() == std::size_t{1});

    const std::string button_a = players[0].buttons[ButtonIndex(Settings::NativeButton::A)];
    const std::string analog_left = players[0].analogs[AnalogIndex(Settings::NativeAnalog::LStick)];

    const auto second = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

    REQUIRE(second.empty());
    REQUIRE(players[0].buttons[ButtonIndex(Settings::NativeButton::A)] == button_a);
    REQUIRE(players[0].analogs[AnalogIndex(Settings::NativeAnalog::LStick)] == analog_left);
}

TEST_CASE("Switch2AutoPair recognises the controller that owns a player slot",
          "[switch2][auto_pair]") {
    std::vector<Settings::PlayerInput> players(std::size_t{2}, MakePlayer());
    // The slot already points at the controller, but the controller is gone.
    const std::string owned_binding =
        MakeOwnedBinding(std::size_t{0}, ControllerType::Pro, "HAC-001");
    players[0].buttons[ButtonIndex(Settings::NativeButton::A)] = owned_binding;
    players[0].connected = false;

    const std::vector<InputCommon::Switch2DeviceBinding> devices{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});

    const auto result = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

    REQUIRE(result.size() == std::size_t{1});
    REQUIRE(result[0].player_index == std::size_t{0});
    REQUIRE(result[0].device_id == "HAC-001");
    REQUIRE(result[0].reconnected);
    REQUIRE_FALSE(result[0].disconnected);
    REQUIRE(players[0].connected);
    REQUIRE(players[0].controller_type == Settings::ControllerType::ProController);
}

TEST_CASE("Switch2AutoPair keeps the player slot when the controller changes driver slot",
          "[switch2][auto_pair]") {
    std::vector<Settings::PlayerInput> players(std::size_t{3}, MakePlayer());
    const std::vector<InputCommon::Switch2DeviceBinding> first_devices{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto first_mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});
    REQUIRE(Switch2AutoPair::ApplyToPlayers(first_devices, first_mappings, players).size() ==
            std::size_t{1});

    // The same controller turns up in another driver slot, with the same identity.
    const std::vector<InputCommon::Switch2DeviceBinding> moved_devices{
        MakeDevice(std::size_t{2}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto moved_mappings = MakeMappings({{std::size_t{2}, "HAC-001"}});
    const auto result = Switch2AutoPair::ApplyToPlayers(moved_devices, moved_mappings, players);

    REQUIRE(result.size() == std::size_t{1});
    REQUIRE(result[0].player_index == std::size_t{0});
    REQUIRE(result[0].device_id == "HAC-001");

    const Common::ParamPackage button_a{players[0].buttons[ButtonIndex(Settings::NativeButton::A)]};
    REQUIRE(button_a.Get("port", "") == "2");
    REQUIRE(button_a.Get("guid", "") == MakeGuid(std::size_t{2}, ControllerType::Pro));
    REQUIRE(button_a.Get("device_id", "") == "HAC-001");
    REQUIRE_FALSE(players[1].connected);
    REQUIRE_FALSE(players[2].connected);
}

TEST_CASE("Switch2AutoPair does not steal a player slot that another controller owns",
          "[switch2][auto_pair]") {
    const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}, {std::size_t{1}, "HAC-002"}});
    const std::vector<InputCommon::Switch2DeviceBinding> devices{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
        MakeDevice(std::size_t{1}, ControllerType::Pro, "HAC-002", "Pro Controller"),
    };

    SECTION("The next free player slot is used") {
        std::vector<Settings::PlayerInput> players(std::size_t{2}, MakePlayer());
        const std::string owned_binding =
            MakeOwnedBinding(std::size_t{0}, ControllerType::Pro, "HAC-001");
        players[0].buttons[ButtonIndex(Settings::NativeButton::A)] = owned_binding;
        players[0].connected = true;

        const auto result = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

        REQUIRE(AnyPlayer(result, 1, "HAC-002"));
        REQUIRE(players[1].connected);
        REQUIRE(players[1].controller_type == Settings::ControllerType::ProController);
        REQUIRE(DeviceIdOf(players[0]) == "HAC-001");
        REQUIRE(DeviceIdOf(players[1]) == "HAC-002");
    }

    SECTION("The controller is skipped when no player slot is free") {
        std::vector<Settings::PlayerInput> players(std::size_t{1}, MakePlayer());
        const std::string owned_binding =
            MakeOwnedBinding(std::size_t{0}, ControllerType::Pro, "HAC-001");
        players[0].buttons[ButtonIndex(Settings::NativeButton::A)] = owned_binding;
        players[0].connected = true;

        const auto result = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

        REQUIRE_FALSE(AnyPlayer(result, 0, "HAC-002"));
        REQUIRE_FALSE(AnyPlayer(result, 1, "HAC-002"));
        REQUIRE(DeviceIdOf(players[0]) == "HAC-001");
        REQUIRE(players[0].connected);
    }
}

TEST_CASE("Switch2AutoPair keeps a user remapping when it rewrites a stale binding",
          "[switch2][auto_pair]") {
    std::vector<Settings::PlayerInput> players(std::size_t{2}, MakePlayer());
    const std::vector<InputCommon::Switch2DeviceBinding> first_devices{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto first_mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});
    REQUIRE(Switch2AutoPair::ApplyToPlayers(first_devices, first_mappings, players).size() ==
            std::size_t{1});

    // The user remaps the A button to the keyboard.
    const std::string keyboard_binding = MakeKeyboardBinding();
    players[0].buttons[ButtonIndex(Settings::NativeButton::A)] = keyboard_binding;

    const std::vector<InputCommon::Switch2DeviceBinding> moved_devices{
        MakeDevice(std::size_t{2}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto moved_mappings = MakeMappings({{std::size_t{2}, "HAC-001"}});
    const auto result = Switch2AutoPair::ApplyToPlayers(moved_devices, moved_mappings, players);

    REQUIRE(result.size() == std::size_t{1});
    REQUIRE(result[0].player_index == std::size_t{0});
    REQUIRE(players[0].buttons[ButtonIndex(Settings::NativeButton::A)] == keyboard_binding);

    // The binding that is not remapped is rewritten with the new driver slot.
    const Common::ParamPackage button_b{players[0].buttons[ButtonIndex(Settings::NativeButton::B)]};
    REQUIRE(button_b.Get("port", "") == "2");
    REQUIRE(button_b.Get("device_id", "") == "HAC-001");
}

TEST_CASE("Switch2AutoPair recognises a player that is bound to a Switch 2 controller",
          "[switch2][auto_pair]") {
    Settings::PlayerInput switch2_player = MakePlayer();
    switch2_player.buttons[ButtonIndex(Settings::NativeButton::A)] =
        MakeOwnedBinding(std::size_t{0}, ControllerType::Pro, "HAC-001");
    REQUIRE(Switch2AutoPair::IsSwitch2Player(switch2_player));

    Settings::PlayerInput keyboard_player = MakePlayer();
    keyboard_player.buttons[ButtonIndex(Settings::NativeButton::A)] = MakeKeyboardBinding();
    REQUIRE_FALSE(Switch2AutoPair::IsSwitch2Player(keyboard_player));

    REQUIRE_FALSE(Switch2AutoPair::IsSwitch2Player(MakePlayer()));
}

TEST_CASE("Switch2AutoPair maps the controller type onto the emulated controller type",
          "[switch2][auto_pair]") {
    REQUIRE(Switch2AutoPair::ControllerTypeFor(ControllerType::Pro) ==
            Settings::ControllerType::ProController);
    REQUIRE(Switch2AutoPair::ControllerTypeFor(ControllerType::Dual) ==
            Settings::ControllerType::DualJoyconDetached);
    REQUIRE(Switch2AutoPair::ControllerTypeFor(ControllerType::Left) ==
            Settings::ControllerType::LeftJoycon);
    REQUIRE(Switch2AutoPair::ControllerTypeFor(ControllerType::Right) ==
            Settings::ControllerType::RightJoycon);
    REQUIRE(Switch2AutoPair::ControllerTypeFor(ControllerType::GameCube) ==
            Settings::ControllerType::GameCube);
}

TEST_CASE("Switch2AutoPair reports a controller that stayed away as disconnected",
          "[switch2][auto_pair]") {
    // The pass keeps the state of the controllers between the runs: a controller is only
    // reported as gone after it was missing for several passes in a row. A pass that runs on a
    // fresh object every time starts with an empty state and never reports a controller as
    // gone.
    Switch2AutoPair auto_pair;
    const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});
    std::vector<Settings::PlayerInput> players{MakePlayer()};

    // The first pass binds the controller to the player slot.
    const std::vector<InputCommon::Switch2DeviceBinding> present{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto bound = auto_pair.ApplyDevices(present, mappings, players);
    REQUIRE(bound.size() == std::size_t{1});
    REQUIRE(bound[0].player_index == std::size_t{0});
    REQUIRE_FALSE(bound[0].disconnected);
    REQUIRE(players[0].connected);
    REQUIRE(DeviceIdOf(players[0]) == "HAC-001");

    // The controller goes away. The first passes do not report it yet, so that a controller
    // that comes back right away keeps its player slot.
    for (u32 pass = 1; pass < Switch2AutoPair::MissingPassesBeforeDisconnect; ++pass) {
        const auto missing = auto_pair.ApplyDevices({}, mappings, players);
        REQUIRE(missing.empty());
        REQUIRE(players[0].connected);
    }

    // The controller has been missing for long enough now.
    const auto gone = auto_pair.ApplyDevices({}, mappings, players);
    REQUIRE(gone.size() == std::size_t{1});
    REQUIRE(gone[0].player_index == std::size_t{0});
    REQUIRE(gone[0].disconnected);
    REQUIRE_FALSE(players[0].connected);

    // The controller comes back: it keeps its player slot and is reported as reconnected.
    const auto back = auto_pair.ApplyDevices(present, mappings, players);
    REQUIRE(back.size() == std::size_t{1});
    REQUIRE(back[0].player_index == std::size_t{0});
    REQUIRE(back[0].reconnected);
    REQUIRE_FALSE(back[0].disconnected);
    REQUIRE(players[0].connected);
}

TEST_CASE("Switch2AutoPair keeps the player slot of a controller that came back",
          "[switch2][auto_pair]") {
    Switch2AutoPair auto_pair;
    const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});
    std::vector<Settings::PlayerInput> players{MakePlayer()};

    const std::vector<InputCommon::Switch2DeviceBinding> present{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    REQUIRE(auto_pair.ApplyDevices(present, mappings, players).size() == std::size_t{1});
    REQUIRE(players[0].connected);

    // The controller is away for one pass only, then it is back. The slot was never marked as
    // disconnected, so the pass reports no change.
    REQUIRE(auto_pair.ApplyDevices({}, mappings, players).empty());
    REQUIRE(players[0].connected);
    REQUIRE(auto_pair.ApplyDevices(present, mappings, players).empty());
    REQUIRE(players[0].connected);

    // The counter of the missing passes was reset by the pass that saw the controller, so the
    // controller is not reported as gone when it disappears again right away.
    REQUIRE(auto_pair.ApplyDevices({}, mappings, players).empty());
    REQUIRE(players[0].connected);
}

TEST_CASE("Switch2AutoPair preserves an actively connected non-Switch 2 Player 1",
          "[switch2][auto_pair]") {
    const std::vector<InputCommon::Switch2DeviceBinding> devices{
        MakeDevice(std::size_t{0}, ControllerType::Pro, "HAC-001", "Pro Controller"),
    };
    const auto mappings = MakeMappings({{std::size_t{0}, "HAC-001"}});
    const std::string keyboard_binding = MakeKeyboardBinding();

    SECTION("An unconnected lower slot is preferred over an active non-Switch 2 Player 1") {
        std::vector<Settings::PlayerInput> players(std::size_t{3}, MakePlayer());
        players[0].connected = true;
        players[0].controller_type = Settings::ControllerType::ProController;
        players[0].buttons[ButtonIndex(Settings::NativeButton::A)] = keyboard_binding;

        const auto result = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

        REQUIRE(result.size() == std::size_t{1});
        REQUIRE(result[0].player_index == std::size_t{1});
        REQUIRE(result[0].device_id == "HAC-001");

        REQUIRE(players[0].connected);
        REQUIRE(players[0].buttons[ButtonIndex(Settings::NativeButton::A)] == keyboard_binding);
        REQUIRE_FALSE(Switch2AutoPair::IsSwitch2Player(players[0]));

        REQUIRE(players[1].connected);
        REQUIRE(DeviceIdOf(players[1]) == "HAC-001");
        REQUIRE_FALSE(players[2].connected);
    }

    SECTION("An unconnected non-Switch 2 Player 1 is still claimed") {
        std::vector<Settings::PlayerInput> players(std::size_t{2}, MakePlayer());
        players[0].connected = false;
        players[0].buttons[ButtonIndex(Settings::NativeButton::A)] = keyboard_binding;

        const auto result = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

        REQUIRE(result.size() == std::size_t{1});
        REQUIRE(result[0].player_index == std::size_t{0});
        REQUIRE(players[0].connected);
        REQUIRE(DeviceIdOf(players[0]) == "HAC-001");
        REQUIRE_FALSE(players[1].connected);
    }

    SECTION("A stale Switch 2 slot is reused before overwriting an active non-Switch 2 Player 1") {
        std::vector<Settings::PlayerInput> players(std::size_t{2}, MakePlayer());
        players[0].connected = true;
        players[0].buttons[ButtonIndex(Settings::NativeButton::A)] = keyboard_binding;

        players[1].connected = false;
        players[1].buttons[ButtonIndex(Settings::NativeButton::A)] =
            MakeOwnedBinding(std::size_t{0}, ControllerType::Pro, "HAC-OLD");

        const auto result = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

        REQUIRE(result.size() == std::size_t{1});
        REQUIRE(result[0].player_index == std::size_t{1});
        REQUIRE(players[0].connected);
        REQUIRE(players[0].buttons[ButtonIndex(Settings::NativeButton::A)] == keyboard_binding);
        REQUIRE(players[1].connected);
        REQUIRE(DeviceIdOf(players[1]) == "HAC-001");
    }

    SECTION("Falls back to a connected non-Switch 2 slot when no other slot is available") {
        std::vector<Settings::PlayerInput> players(std::size_t{1}, MakePlayer());
        players[0].connected = true;
        players[0].buttons[ButtonIndex(Settings::NativeButton::A)] = keyboard_binding;

        const auto result = Switch2AutoPair::ApplyToPlayers(devices, mappings, players);

        REQUIRE(result.size() == std::size_t{1});
        REQUIRE(result[0].player_index == std::size_t{0});
        REQUIRE(players[0].connected);
        REQUIRE(DeviceIdOf(players[0]) == "HAC-001");
    }
}
