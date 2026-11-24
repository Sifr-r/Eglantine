// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "common/string_util.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/bcat_util.h"
#include "core/hle/service/bcat/delivery_cache_file_service.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::BCAT {

IDeliveryCacheFileService::IDeliveryCacheFileService(Core::System& system_,
                                                     FileSys::VirtualDir root_)
    : ServiceFramework{system_, "IDeliveryCacheFileService"}, root(std::move(root_)) {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IDeliveryCacheFileService::Open>, "Open"},
        {1, D<&IDeliveryCacheFileService::Read>, "Read"},
        {2, D<&IDeliveryCacheFileService::GetSize>, "GetSize"},
        {3, D<&IDeliveryCacheFileService::GetDigest>, "GetDigest"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IDeliveryCacheFileService::~IDeliveryCacheFileService() = default;

Result IDeliveryCacheFileService::Open(const DirectoryName& dir_name_raw,
                                       const FileName& file_name_raw) {
    const auto dir_name =
        Common::StringFromFixedZeroTerminatedBuffer(dir_name_raw.data(), dir_name_raw.size());
    const auto file_name =
        Common::StringFromFixedZeroTerminatedBuffer(file_name_raw.data(), file_name_raw.size());

    LOG_DEBUG(Service_BCAT, "called, dir_name={}, file_name={}", dir_name, file_name);

    R_TRY(VerifyNameValidDir(dir_name_raw));
    R_TRY(VerifyNameValidFile(file_name_raw));
    R_UNLESS(current_file == nullptr, ResultEntityAlreadyOpen);

    const auto dir = root->GetSubdirectory(dir_name);
    R_UNLESS(dir != nullptr, ResultFailedOpenEntity);

    current_file = dir->GetFile(file_name);
    R_UNLESS(current_file != nullptr, ResultFailedOpenEntity);

    R_SUCCEED();
}

Result IDeliveryCacheFileService::Read(Out<u64> out_buffer_size, u64 offset,
                                       OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    LOG_DEBUG(Service_BCAT, "called, offset={:016X}, size={:016X}", offset, out_buffer.size());

    R_UNLESS(current_file != nullptr, ResultNoOpenEntry);

    const u64 file_size = current_file->GetSize();
    if (offset >= file_size || out_buffer.empty()) {
        *out_buffer_size = 0;
        R_SUCCEED();
    }

    const u64 bytes_to_read = std::min<u64>(file_size - offset, out_buffer.size());
    const auto buffer = current_file->ReadBytes(bytes_to_read, offset);
    const std::size_t bytes_read = (std::min)(buffer.size(), out_buffer.size());
    if (bytes_read > 0) {
        std::memcpy(out_buffer.data(), buffer.data(), bytes_read);
    }
    *out_buffer_size = static_cast<u64>(bytes_read);

    R_SUCCEED();
}

Result IDeliveryCacheFileService::GetSize(Out<u64> out_size) {
    LOG_DEBUG(Service_BCAT, "called");

    R_UNLESS(current_file != nullptr, ResultNoOpenEntry);

    *out_size = current_file->GetSize();
    R_SUCCEED();
}

Result IDeliveryCacheFileService::GetDigest(Out<BcatDigest> out_digest) {
    LOG_DEBUG(Service_BCAT, "called");

    R_UNLESS(current_file != nullptr, ResultNoOpenEntry);

    //*out_digest = DigestFile(current_file);
    R_SUCCEED();
}

} // namespace Service::BCAT
