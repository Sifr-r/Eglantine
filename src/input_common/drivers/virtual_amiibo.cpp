// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <fmt/ranges.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "input_common/drivers/virtual_amiibo.h"

namespace InputCommon {
constexpr PadIdentifier identifier = {
    .guid = Common::UUID{},
    .port = 0,
    .pad = 0,
};

VirtualAmiibo::VirtualAmiibo(std::string input_engine_) : InputEngine(std::move(input_engine_)) {}

VirtualAmiibo::~VirtualAmiibo() = default;

Common::Input::DriverResult VirtualAmiibo::SetPollingMode(
    [[maybe_unused]] const PadIdentifier& identifier_,
    const Common::Input::PollingMode polling_mode_) {
    std::optional<Common::Input::NfcStatus> notify_status;
    Common::Input::DriverResult result = Common::Input::DriverResult::NotSupported;
    {
        std::scoped_lock lock{state_mutex};
        polling_mode = polling_mode_;

        switch (polling_mode) {
        case Common::Input::PollingMode::NFC:
            state = State::Initialized;
            result = Common::Input::DriverResult::Success;
            break;
        default:
            if (state == State::TagNearby) {
                CloseAmiiboLocked(notify_status);
            }
            state = State::Disabled;
            result = Common::Input::DriverResult::NotSupported;
            break;
        }
    }
    if (notify_status.has_value()) {
        SetNfc(identifier, *notify_status);
    }
    return result;
}

Common::Input::NfcState VirtualAmiibo::SupportsNfc(
    [[maybe_unused]] const PadIdentifier& identifier_) const {
    return Common::Input::NfcState::Success;
}
Common::Input::NfcState VirtualAmiibo::StartNfcPolling(const PadIdentifier& identifier_) {
    std::scoped_lock lock{state_mutex};
    if (state != State::Initialized) {
        return Common::Input::NfcState::WrongDeviceState;
    }
    state = State::WaitingForAmiibo;
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::StopNfcPolling(const PadIdentifier& identifier_) {
    std::optional<Common::Input::NfcStatus> notify_status;
    {
        std::scoped_lock lock{state_mutex};
        if (state == State::Disabled) {
            return Common::Input::NfcState::WrongDeviceState;
        }
        if (state == State::TagNearby) {
            CloseAmiiboLocked(notify_status);
        }
        state = State::Initialized;
    }
    if (notify_status.has_value()) {
        SetNfc(identifier, *notify_status);
    }
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::ReadAmiiboData(const PadIdentifier& identifier_,
                                                      std::vector<u8>& out_data) {
    std::scoped_lock lock{state_mutex};
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 1) {
        return Common::Input::NfcState::InvalidTagType;
    }

    out_data.resize(nfc_data.size());
    std::memcpy(out_data.data(), nfc_data.data(), nfc_data.size());
    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::WriteNfcData(
    [[maybe_unused]] const PadIdentifier& identifier_, const std::vector<u8>& data) {
    std::scoped_lock lock{state_mutex};
    const Common::FS::IOFile nfc_file{file_path, Common::FS::FileAccessMode::ReadWrite,
                                      Common::FS::FileType::BinaryFile};

    if (!nfc_file.IsOpen()) {
        LOG_ERROR(Core, "Amiibo is already on use");
        return Common::Input::NfcState::WriteFailed;
    }

    if (!nfc_file.Write(data)) {
        LOG_ERROR(Service_NFP, "Error writing to file");
        return Common::Input::NfcState::WriteFailed;
    }

    nfc_data = data;

    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::ReadMifareData(const PadIdentifier& identifier_,
                                                      const Common::Input::MifareRequest& request,
                                                      Common::Input::MifareRequest& out_data) {
    std::scoped_lock lock{state_mutex};
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 6) {
        return Common::Input::NfcState::InvalidTagType;
    }

    for (std::size_t i = 0; i < request.data.size(); i++) {
        if (request.data[i].command == 0) {
            continue;
        }
        out_data.data[i].command = request.data[i].command;
        out_data.data[i].sector = request.data[i].sector;

        const std::size_t sector_index =
            request.data[i].sector * sizeof(Common::Input::MifareData::data);

        if (nfc_data.size() < sector_index + sizeof(Common::Input::MifareData::data)) {
            return Common::Input::NfcState::WriteFailed;
        }

        // Ignore the sector key as we don't support it
        std::memcpy(out_data.data[i].data.data(), nfc_data.data() + sector_index,
                    sizeof(Common::Input::MifareData::data));
    }

    return Common::Input::NfcState::Success;
}

Common::Input::NfcState VirtualAmiibo::WriteMifareData(
    const PadIdentifier& identifier_, const Common::Input::MifareRequest& request) {
    std::scoped_lock lock{state_mutex};
    if (state != State::TagNearby) {
        return Common::Input::NfcState::WrongDeviceState;
    }

    if (status.tag_type != 1U << 6) {
        return Common::Input::NfcState::InvalidTagType;
    }

    for (std::size_t i = 0; i < request.data.size(); i++) {
        if (request.data[i].command == 0) {
            continue;
        }

        const std::size_t sector_index =
            request.data[i].sector * sizeof(Common::Input::MifareData::data);

        if (nfc_data.size() < sector_index + sizeof(Common::Input::MifareData::data)) {
            return Common::Input::NfcState::WriteFailed;
        }

        // Ignore the sector key as we don't support it
        std::memcpy(nfc_data.data() + sector_index, request.data[i].data.data(),
                    sizeof(Common::Input::MifareData::data));
    }

    return Common::Input::NfcState::Success;
}

VirtualAmiibo::State VirtualAmiibo::GetCurrentState() const {
    std::scoped_lock lock{state_mutex};
    return state;
}

VirtualAmiibo::Info VirtualAmiibo::LoadAmiibo(const std::string& filename) {
    const Common::FS::IOFile nfc_file{filename, Common::FS::FileAccessMode::Read,
                                      Common::FS::FileType::BinaryFile};
    std::vector<u8> data{};

    if (!nfc_file.IsOpen()) {
        return Info::UnableToLoad;
    }

    switch (nfc_file.GetSize()) {
    case AmiiboSize:
    case AmiiboSizeWithoutPassword:
    case AmiiboSizeWithSignature:
        data.resize(AmiiboSize);
        if (nfc_file.Read(data) < AmiiboSizeWithoutPassword) {
            return Info::NotAnAmiibo;
        }
        break;
    case MifareSize:
        data.resize(MifareSize);
        if (nfc_file.Read(data) < MifareSize) {
            return Info::NotAnAmiibo;
        }
        break;
    default:
        return Info::NotAnAmiibo;
    }

    std::optional<Common::Input::NfcStatus> notify_status;
    Info info = Info::Unknown;
    {
        std::scoped_lock lock{state_mutex};
        file_path = filename;
        info = LoadAmiiboLocked(data, notify_status);
    }
    if (notify_status.has_value()) {
        SetNfc(identifier, *notify_status);
    }
    return info;
}

VirtualAmiibo::Info VirtualAmiibo::LoadAmiibo(std::span<u8> data) {
    std::optional<Common::Input::NfcStatus> notify_status;
    Info info = Info::Unknown;
    {
        std::scoped_lock lock{state_mutex};
        info = LoadAmiiboLocked(data, notify_status);
    }
    if (notify_status.has_value()) {
        SetNfc(identifier, *notify_status);
    }
    return info;
}

VirtualAmiibo::Info VirtualAmiibo::LoadAmiiboLocked(
    std::span<u8> data, std::optional<Common::Input::NfcStatus>& out_status) {
    if (state != State::WaitingForAmiibo) {
        return Info::WrongDeviceState;
    }

    switch (data.size_bytes()) {
    case AmiiboSize:
    case AmiiboSizeWithoutPassword:
    case AmiiboSizeWithSignature:
        nfc_data.assign(AmiiboSize, 0);
        status.tag_type = 1U << 1;
        status.uuid_length = 7;
        break;
    case MifareSize:
        nfc_data.assign(MifareSize, 0);
        status.tag_type = 1U << 6;
        status.uuid_length = 4;
        break;
    default:
        return Info::NotAnAmiibo;
    }

    status.uuid = {};
    status.protocol = 1;
    state = State::TagNearby;
    status.state = Common::Input::NfcState::NewAmiibo;
    const std::size_t copy_size = (std::min)(data.size_bytes(), nfc_data.size());
    std::memcpy(nfc_data.data(), data.data(), copy_size);
    std::memcpy(status.uuid.data(), nfc_data.data(), status.uuid_length);
    out_status = status;
    return Info::Success;
}

VirtualAmiibo::Info VirtualAmiibo::ReloadAmiibo() {
    std::optional<Common::Input::NfcStatus> notify_status;
    std::string path;
    {
        std::scoped_lock lock{state_mutex};
        if (state == State::TagNearby) {
            notify_status = status;
        } else {
            path = file_path;
        }
    }
    if (notify_status.has_value()) {
        SetNfc(identifier, *notify_status);
        return Info::Success;
    }
    return LoadAmiibo(path);
}

VirtualAmiibo::Info VirtualAmiibo::CloseAmiibo() {
    std::optional<Common::Input::NfcStatus> notify_status;
    Info info = Info::Success;
    {
        std::scoped_lock lock{state_mutex};
        info = CloseAmiiboLocked(notify_status);
    }
    if (notify_status.has_value()) {
        SetNfc(identifier, *notify_status);
    }
    return info;
}

VirtualAmiibo::Info VirtualAmiibo::CloseAmiiboLocked(
    std::optional<Common::Input::NfcStatus>& out_status) {
    if (state != State::TagNearby) {
        return Info::Success;
    }

    state = State::WaitingForAmiibo;
    status.state = Common::Input::NfcState::AmiiboRemoved;
    out_status = status;
    status.tag_type = 0;
    return Info::Success;
}

std::string VirtualAmiibo::GetLastFilePath() const {
    std::scoped_lock lock{state_mutex};
    return file_path;
}

} // namespace InputCommon
