// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <mutex>

#include <fmt/ranges.h>

#include "common/param_package.h"
#include "input_common/drivers/camera.h"

namespace InputCommon {
constexpr PadIdentifier identifier = {
    .guid = Common::UUID{},
    .port = 0,
    .pad = 0,
};

Camera::Camera(std::string input_engine_) : InputEngine(std::move(input_engine_)) {
    PreSetController(identifier);
}

void Camera::SetCameraData(std::size_t width, std::size_t height, std::span<const u32> data) {
    if (width == 0 || height == 0 ||
        width > (std::numeric_limits<std::size_t>::max)() / height ||
        data.size() < width * height) {
        return;
    }

    Common::Input::CameraStatus camera_status{};
    {
        std::scoped_lock lock{status_mutex};
        const std::size_t desired_width = GetImageWidth(status.format);
        const std::size_t desired_height = GetImageHeight(status.format);
        if (desired_width == 0 || desired_height == 0) {
            return;
        }
        status.data.resize(desired_width * desired_height);

        // Resize image to desired format
        for (std::size_t y = 0; y < desired_height; y++) {
            for (std::size_t x = 0; x < desired_width; x++) {
                const std::size_t pixel_index = y * desired_width + x;
                const std::size_t old_x =
                    (width / desired_width) * x + ((width % desired_width) * x) / desired_width;
                const std::size_t old_y =
                    (height / desired_height) * y + ((height % desired_height) * y) / desired_height;
                const std::size_t data_pixel_index = old_y * width + old_x;
                status.data[pixel_index] = static_cast<u8>(data[data_pixel_index] & 0xFF);
            }
        }

        camera_status = status;
    }

    SetCamera(identifier, camera_status);
}

std::size_t Camera::GetImageWidth(Common::Input::CameraFormat format) {
    switch (format) {
    case Common::Input::CameraFormat::Size320x240:
        return 320;
    case Common::Input::CameraFormat::Size160x120:
        return 160;
    case Common::Input::CameraFormat::Size80x60:
        return 80;
    case Common::Input::CameraFormat::Size40x30:
        return 40;
    case Common::Input::CameraFormat::Size20x15:
        return 20;
    case Common::Input::CameraFormat::None:
    default:
        return 0;
    }
}

std::size_t Camera::GetImageHeight(Common::Input::CameraFormat format) {
    switch (format) {
    case Common::Input::CameraFormat::Size320x240:
        return 240;
    case Common::Input::CameraFormat::Size160x120:
        return 120;
    case Common::Input::CameraFormat::Size80x60:
        return 60;
    case Common::Input::CameraFormat::Size40x30:
        return 30;
    case Common::Input::CameraFormat::Size20x15:
        return 15;
    case Common::Input::CameraFormat::None:
    default:
        return 0;
    }
}

std::size_t Camera::getImageWidth() const {
    std::scoped_lock lock{status_mutex};
    return GetImageWidth(status.format);
}

std::size_t Camera::getImageHeight() const {
    std::scoped_lock lock{status_mutex};
    return GetImageHeight(status.format);
}

Common::Input::DriverResult Camera::SetCameraFormat(
    [[maybe_unused]] const PadIdentifier& identifier_,
    const Common::Input::CameraFormat camera_format) {
    std::scoped_lock lock{status_mutex};
    status.format = camera_format;
    return Common::Input::DriverResult::Success;
}

} // namespace InputCommon
