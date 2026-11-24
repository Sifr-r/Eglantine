// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/settings.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "ui_configure_vibration.h"
#include "yuzu/configuration/configure_vibration.h"

ConfigureVibration::ConfigureVibration(QWidget* parent, Core::HID::HIDCore& hid_core_)
    : QDialog(parent), ui(std::make_unique<Ui::ConfigureVibration>()), hid_core{hid_core_} {
    ui->setupUi(this);

    qRegisterMetaType<Core::HID::ControllerTriggerType>("Core::HID::ControllerTriggerType");
    connect(this, &ConfigureVibration::VibrationUpdateRequested, this,
            &ConfigureVibration::VibrateController, Qt::QueuedConnection);

    vibration_groupboxes = {
        ui->vibrationGroupPlayer1, ui->vibrationGroupPlayer2, ui->vibrationGroupPlayer3,
        ui->vibrationGroupPlayer4, ui->vibrationGroupPlayer5, ui->vibrationGroupPlayer6,
        ui->vibrationGroupPlayer7, ui->vibrationGroupPlayer8,
    };

    vibration_spinboxes = {
        ui->vibrationSpinPlayer1, ui->vibrationSpinPlayer2, ui->vibrationSpinPlayer3,
        ui->vibrationSpinPlayer4, ui->vibrationSpinPlayer5, ui->vibrationSpinPlayer6,
        ui->vibrationSpinPlayer7, ui->vibrationSpinPlayer8,
    };

    const auto& players = Settings::values.players.GetValue();

    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        auto controller = hid_core.GetEmulatedControllerByIndex(i);
        Core::HID::ControllerUpdateCallback engine_callback{
            .on_change =
                [this, i](Core::HID::ControllerTriggerType type) {
                    if (type == Core::HID::ControllerTriggerType::Button) {
                        emit VibrationUpdateRequested(type, static_cast<int>(i));
                    }
                },
            .is_npad_service = false,
        };
        controller_callback_key[i] = controller->SetCallback(engine_callback);
        vibration_groupboxes[i]->setChecked(players[i].vibration_enabled);
        vibration_spinboxes[i]->setValue(players[i].vibration_strength);
    }

    ui->checkBoxAccurateVibration->setChecked(
        Settings::values.enable_accurate_vibrations.GetValue());

    if (!Settings::IsConfiguringGlobal()) {
        ui->checkBoxAccurateVibration->setDisabled(true);
    }

    RetranslateUI();
}

ConfigureVibration::~ConfigureVibration() {
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        auto controller = hid_core.GetEmulatedControllerByIndex(i);
        controller->DeleteCallback(controller_callback_key[i]);
    }

    StopVibrations();
}

void ConfigureVibration::ApplyConfiguration() {
    auto& players = Settings::values.players.GetValue();

    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        players[i].vibration_enabled = vibration_groupboxes[i]->isChecked();
        players[i].vibration_strength = vibration_spinboxes[i]->value();
    }

    Settings::values.enable_accurate_vibrations.SetValue(
        ui->checkBoxAccurateVibration->isChecked());
}

void ConfigureVibration::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigureVibration::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureVibration::VibrateController(Core::HID::ControllerTriggerType type,
                                           int player_index) {
    if (type != Core::HID::ControllerTriggerType::Button) {
        return;
    }
    if (player_index < 0 || static_cast<std::size_t>(player_index) >= NUM_PLAYERS) {
        return;
    }

    const auto idx = static_cast<std::size_t>(player_index);
    auto controller = hid_core.GetEmulatedControllerByIndex(idx);
    const auto buttons = controller->GetButtonsValues();

    const bool button_is_pressed =
        std::any_of(buttons.begin(), buttons.end(), [](const auto& button) { return button.value; });

    if (!button_is_pressed) {
        if (is_vibrating[idx]) {
            StopVibration(idx);
        }
        return;
    }

    auto& player = Settings::values.players.GetValue()[idx];
    const int vibration_strength = vibration_spinboxes[idx]->value();
    const bool old_vibration_enabled = player.vibration_enabled;
    const int old_vibration_strength = player.vibration_strength;
    player.vibration_enabled = true;
    player.vibration_strength = vibration_strength;

    const Core::HID::VibrationValue vibration{
        .low_amplitude = 1.0f,
        .low_frequency = 160.0f,
        .high_amplitude = 1.0f,
        .high_frequency = 320.0f,
    };
    controller->SetVibration(Core::HID::DeviceIndex::Left, vibration);
    controller->SetVibration(Core::HID::DeviceIndex::Right, vibration);
    is_vibrating[idx] = true;

    // Restore previous values
    player.vibration_enabled = old_vibration_enabled;
    player.vibration_strength = old_vibration_strength;
}

void ConfigureVibration::StopVibration(std::size_t player_index) {
    auto controller = hid_core.GetEmulatedControllerByIndex(player_index);
    auto& player = Settings::values.players.GetValue()[player_index];
    const bool old_vibration_enabled = player.vibration_enabled;
    player.vibration_enabled = true;
    controller->SetVibration(Core::HID::DeviceIndex::Left, Core::HID::DEFAULT_VIBRATION_VALUE);
    controller->SetVibration(Core::HID::DeviceIndex::Right, Core::HID::DEFAULT_VIBRATION_VALUE);
    player.vibration_enabled = old_vibration_enabled;
    is_vibrating[player_index] = false;
}

void ConfigureVibration::StopVibrations() {
    for (std::size_t i = 0; i < NUM_PLAYERS; ++i) {
        StopVibration(i);
    }
}
