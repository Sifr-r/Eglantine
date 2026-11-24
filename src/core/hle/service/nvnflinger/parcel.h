// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"

namespace Service::android {

struct ParcelHeader {
    u32 data_size;
    u32 data_offset;
    u32 objects_size;
    u32 objects_offset;
};
static_assert(sizeof(ParcelHeader) == 16, "ParcelHeader has wrong size");

class InputParcel final {
public:
    explicit InputParcel(std::span<const u8> in_data) : read_buffer(std::move(in_data)) {
        DeserializeHeader();
        if (m_is_valid) {
            [[maybe_unused]] const std::u16string token = ReadInterfaceToken();
        }
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return m_is_valid;
    }

    template <typename T>
    void Read(T& val) {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable.");
        if (!HasCapacity(sizeof(T))) {
            LOG_ERROR(Service_Nvnflinger,
                      "InputParcel::Read out of bounds (read_index={}, size={}, buffer_size={})",
                      read_index, sizeof(T), read_buffer.size());
            val = T{};
            Invalidate();
            return;
        }

        std::memcpy(&val, read_buffer.data() + read_index, sizeof(T));
        read_index = (std::min)(Common::AlignUp(read_index + sizeof(T), 4), read_buffer.size());
    }

    template <typename T>
    T Read() {
        T val{};
        Read(val);
        return val;
    }

    template <typename T>
    void ReadFlattened(T& val) {
        const auto flattened_size = Read<s64>();
        if (!m_is_valid || flattened_size != static_cast<s64>(sizeof(T))) {
            LOG_ERROR(Service_Nvnflinger,
                      "InputParcel::ReadFlattened size mismatch (expected={}, got={})",
                      sizeof(T), flattened_size);
            val = T{};
            Invalidate();
            return;
        }
        Read(val);
    }

    template <typename T>
    T ReadFlattened() {
        T val{};
        ReadFlattened(val);
        return val;
    }

    template <typename T>
    T ReadUnaligned() {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable.");
        T val{};
        if (!HasCapacity(sizeof(T))) {
            LOG_ERROR(
                Service_Nvnflinger,
                "InputParcel::ReadUnaligned out of bounds (read_index={}, size={}, buffer_size={})",
                read_index, sizeof(T), read_buffer.size());
            Invalidate();
            return val;
        }

        std::memcpy(&val, read_buffer.data() + read_index, sizeof(T));
        read_index += sizeof(T);
        return val;
    }

    template <typename T>
    const std::shared_ptr<T> ReadObject() {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable.");

        const auto is_valid{Read<bool>()};

        if (m_is_valid && is_valid) {
            auto result = std::make_shared<T>();
            ReadFlattened(*result);
            if (!m_is_valid) {
                return {};
            }
            return result;
        }

        return {};
    }

    std::u16string ReadInterfaceToken() {
        [[maybe_unused]] const u32 unknown = Read<u32>();
        const u32 length = Read<u32>();
        if (!m_is_valid) {
            return {};
        }

        const std::size_t remaining_u16 = (read_buffer.size() - read_index) / sizeof(u16);
        if (static_cast<std::size_t>(length) >= remaining_u16) {
            LOG_ERROR(Service_Nvnflinger,
                      "InputParcel::ReadInterfaceToken length {} exceeds remaining buffer ({} u16s)",
                      length, remaining_u16);
            Invalidate();
            return {};
        }

        const std::size_t code_units = static_cast<std::size_t>(length) + 1;
        std::u16string token;
        token.reserve(code_units);

        for (std::size_t ch = 0; ch < code_units; ++ch) {
            token.push_back(ReadUnaligned<u16>());
        }

        read_index = (std::min)(Common::AlignUp(read_index, 4), read_buffer.size());

        return token;
    }

    void DeserializeHeader() {
        if (read_buffer.size() < sizeof(ParcelHeader)) {
            LOG_ERROR(Service_Nvnflinger, "InputParcel buffer too small for header: {}",
                      read_buffer.size());
            Invalidate();
            return;
        }

        ParcelHeader header{};
        std::memcpy(&header, read_buffer.data(), sizeof(ParcelHeader));

        if (header.data_offset < sizeof(ParcelHeader) || header.data_offset > read_buffer.size()) {
            LOG_ERROR(Service_Nvnflinger,
                      "InputParcel invalid data_offset {} (buffer_size={})", header.data_offset,
                      read_buffer.size());
            Invalidate();
            return;
        }

        read_index = header.data_offset;
    }

private:
    [[nodiscard]] bool HasCapacity(std::size_t bytes) const noexcept {
        return m_is_valid && read_index <= read_buffer.size() &&
               bytes <= read_buffer.size() - read_index;
    }

    void Invalidate() noexcept {
        m_is_valid = false;
        read_index = read_buffer.size();
    }

    std::span<const u8> read_buffer;
    std::size_t read_index = 0;
    bool m_is_valid = true;
};

class OutputParcel final {
public:
    OutputParcel() = default;

    template <typename T>
    void Write(const T& val) {
        this->WriteImpl(val, m_data_buffer);
    }

    template <typename T>
    void WriteFlattenedObject(const T* ptr) {
        if (!ptr) {
            this->Write<u32>(0);
            return;
        }

        this->Write<u32>(1);
        this->Write<s64>(sizeof(T));
        this->Write(*ptr);
    }

    template <typename T>
    void WriteFlattenedObject(const std::shared_ptr<T> ptr) {
        this->WriteFlattenedObject(ptr.get());
    }

    template <typename T>
    void WriteInterface(const T& val) {
        this->WriteImpl(val, m_data_buffer);
        this->WriteImpl(0U, m_object_buffer);
    }

    std::span<u8> Serialize() {
        m_output_buffer.resize(sizeof(ParcelHeader) + m_data_buffer.size() +
                               m_object_buffer.size());

        ParcelHeader header{};
        header.data_size = static_cast<u32>(m_data_buffer.size());
        header.data_offset = sizeof(ParcelHeader);
        header.objects_size = static_cast<u32>(m_object_buffer.size());
        header.objects_offset = header.data_offset + header.data_size;

        std::memcpy(m_output_buffer.data(), &header, sizeof(ParcelHeader));
        std::ranges::copy(m_data_buffer, m_output_buffer.data() + header.data_offset);
        std::ranges::copy(m_object_buffer, m_output_buffer.data() + header.objects_offset);

        return m_output_buffer;
    }

private:
    template <typename T, size_t BufferSize>
        requires(std::is_trivially_copyable_v<T>)
    void WriteImpl(const T& val, boost::container::small_vector<u8, BufferSize>& buffer) {
        const size_t aligned_size = Common::AlignUp(sizeof(T), 4);
        const size_t old_size = buffer.size();
        buffer.resize(old_size + aligned_size);

        std::memcpy(buffer.data() + old_size, &val, sizeof(T));
    }

private:
    boost::container::small_vector<u8, 0x1B0> m_data_buffer;
    boost::container::small_vector<u8, 0x40> m_object_buffer;
#if BOOST_VERSION >= 108100 || __GNUC__ > 12
    boost::container::small_vector<u8, 0x200> m_output_buffer;
#else //TODO: debian stable
    std::vector<u8> m_output_buffer;
#endif
};

} // namespace Service::android
