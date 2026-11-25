// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/socket_types.h"
#include "network/packet.h"
#include "network/room.h"
#include "network/room_member.h"

namespace Network {
namespace {

static_assert(std::is_same_v<decltype(std::declval<const RoomMember&>().GetNickname()),
                             std::string>,
              "RoomMember::GetNickname must return the nickname by value");
static_assert(PacketElementMaxCount<std::string>::value == MaxPacketStringVectorElements);
static_assert(PacketElementMaxCount<u8>::value == (std::numeric_limits<u32>::max)());

TEST_CASE("Packet", "[network]") {
    SECTION("A vector read of a truncated payload does not allocate") {
        Packet packet;
        packet.Write(static_cast<u32>(0xFFFFFFFF)); // Element count
        packet.Write(static_cast<u8>(0x42));        // A single element

        std::vector<u8> data;
        packet.Read(data);

        REQUIRE_FALSE(static_cast<bool>(packet));
        REQUIRE(data.empty());
        // The vector was never resized, so it never allocated anything.
        REQUIRE(data.capacity() == 0);
    }

    SECTION("A vector read that fits is read") {
        Packet packet;
        packet.Write(std::vector<u8>{1, 2, 3});

        std::vector<u8> data;
        packet.Read(data);

        REQUIRE(static_cast<bool>(packet));
        REQUIRE((data == std::vector<u8>{1, 2, 3}));
    }

    SECTION("A string vector read of a truncated payload does not allocate") {
        Packet packet;
        packet.Write(static_cast<u32>(1000)); // Element count

        std::vector<std::string> data;
        packet.Read(data);

        REQUIRE_FALSE(static_cast<bool>(packet));
        REQUIRE(data.empty());
        REQUIRE(data.capacity() == 0);
    }

    SECTION("A string vector read that fits is read") {
        Packet packet;
        packet.Write(std::vector<std::string>{"a", "bb"});

        std::vector<std::string> data;
        packet.Read(data);

        REQUIRE(static_cast<bool>(packet));
        REQUIRE((data == std::vector<std::string>{"a", "bb"}));
    }

    SECTION("A string vector exceeding the element cap does not allocate even with 4*N zero-length prefixes") {
        constexpr u32 count = static_cast<u32>(MaxPacketStringVectorElements + 1);
        Packet packet;
        packet.Write(count);
        for (u32 i = 0; i < count; ++i) {
            packet.Write(static_cast<u32>(0));
        }

        std::vector<std::string> data;
        packet.Read(data);

        REQUIRE_FALSE(static_cast<bool>(packet));
        REQUIRE(data.empty());
        REQUIRE(data.capacity() == 0);
    }

    SECTION("A string vector at the element cap is read") {
        constexpr u32 count = static_cast<u32>(MaxPacketStringVectorElements);
        Packet packet;
        packet.Write(count);
        for (u32 i = 0; i < count; ++i) {
            packet.Write(static_cast<u32>(0));
        }

        std::vector<std::string> data;
        packet.Read(data);

        REQUIRE(static_cast<bool>(packet));
        REQUIRE(data.size() == MaxPacketStringVectorElements);
    }

    SECTION("IgnoreBytes past the end invalidates the packet") {
        Packet packet;
        packet.Write(static_cast<u32>(0x12345678));

        packet.IgnoreBytes(1024);

        REQUIRE_FALSE(static_cast<bool>(packet));
        // The read position was clamped to the end of the packet.
        REQUIRE(packet.EndOfPacket());
        REQUIRE(packet.RemainingBytes() == 0);

        // A read after the invalidation does not return any data.
        u32 value = 0xFFFFFFFF;
        packet.Read(value);
        REQUIRE_FALSE(static_cast<bool>(packet));
        REQUIRE(value == 0);
    }

    SECTION("IgnoreBytes within the packet keeps the packet valid") {
        Packet packet;
        packet.Write(static_cast<u32>(0x12345678));

        packet.IgnoreBytes(sizeof(u32));

        REQUIRE(static_cast<bool>(packet));
        REQUIRE(packet.EndOfPacket());
    }

    SECTION("A bool read of a truncated packet yields false") {
        Packet packet; // An empty packet

        bool value = true;
        packet.Read(value);

        REQUIRE_FALSE(value);
        REQUIRE_FALSE(static_cast<bool>(packet));
    }

    SECTION("A bounded string read refuses to overflow the destination buffer") {
        Packet packet;
        packet.Write(std::string("0123456789abcdef"));

        std::array<char, 8> buffer{};
        packet.Read(buffer.data(), buffer.size());

        REQUIRE_FALSE(static_cast<bool>(packet));
        REQUIRE(buffer[0] == '\0');
    }

    SECTION("A bounded string read that fits is read") {
        Packet packet;
        packet.Write(std::string("abc"));

        std::array<char, 8> buffer{};
        packet.Read(buffer.data(), buffer.size());

        REQUIRE(static_cast<bool>(packet));
        REQUIRE(std::string(buffer.data()) == "abc");
    }

    SECTION("A relayed packet has its source address normalised") {
        // A proxy packet starts with the message type, the family and the four bytes of the
        // source address of the sender.
        Packet packet;
        packet.Write(static_cast<u8>(IdProxyPacket)); // Message type
        packet.Write(static_cast<u8>(0x02));         // Family
        const std::array<u8, 4> foreign_ip{10, 20, 30, 40};
        packet.Write(foreign_ip);                    // The address that the member claimed
        packet.Write(static_cast<u8>(0x99));         // Some other data, which must not change

        // The room rewrites the source address of a relayed packet with the address that it
        // assigned to the member, instead of dropping the packet.
        const std::array<u8, 4> assigned_ip{192, 168, 1, 5};
        const bool overwritten =
            packet.OverwriteBytes(RelaySourceIpOffset, assigned_ip.data(), assigned_ip.size());

        REQUIRE(overwritten);
        // The packet is still valid, it is relayed instead of being dropped.
        REQUIRE(static_cast<bool>(packet));
        REQUIRE(packet.GetDataSize() == 7);

        const auto* bytes = static_cast<const u8*>(packet.GetData());
        REQUIRE(bytes != nullptr);
        REQUIRE(std::memcmp(bytes + RelaySourceIpOffset, assigned_ip.data(), assigned_ip.size()) == 0);
        // The rest of the packet is unchanged.
        REQUIRE(bytes[0] == static_cast<u8>(IdProxyPacket));
        REQUIRE(bytes[1] == 0x02);
        REQUIRE(bytes[6] == 0x99);
    }

    SECTION("A relayed packet that is too short is not overwritten") {
        Packet packet;
        packet.Write(static_cast<u8>(IdProxyPacket)); // Message type
        packet.Write(static_cast<u8>(0x02));         // Family

        const std::array<u8, 4> assigned_ip{192, 168, 1, 5};
        REQUIRE_FALSE(
            packet.OverwriteBytes(RelaySourceIpOffset, assigned_ip.data(), assigned_ip.size()));
        REQUIRE(static_cast<bool>(packet));
    }

    SECTION("An overwrite that does not fit is reported as a failure") {
        // The caller of the overwrite has to be able to detect a failed overwrite, so that it
        // can refuse to relay a packet whose source address could not be corrected.
        const std::array<u8, 4> assigned_ip{192, 168, 1, 5};

        Packet short_packet;
        short_packet.Write(static_cast<u8>(IdProxyPacket)); // Message type
        short_packet.Write(static_cast<u8>(0x02));         // Family
        short_packet.Write(static_cast<u8>(0x0A));         // One byte of the source address

        // The packet is one byte too short for the source address.
        REQUIRE_FALSE(short_packet.OverwriteBytes(RelaySourceIpOffset, assigned_ip.data(),
                                                  assigned_ip.size()));
        // The failed overwrite left the packet unchanged, so the caller can still drop it.
        REQUIRE(static_cast<bool>(short_packet));
        REQUIRE(short_packet.GetDataSize() == 3);
        const auto* short_bytes = static_cast<const u8*>(short_packet.GetData());
        REQUIRE(short_bytes != nullptr);
        REQUIRE(short_bytes[2] == 0x0A);

        // An overwrite that fits is reported as a success, so that the caller can tell the two
        // cases apart.
        Packet packet;
        packet.Write(static_cast<u8>(IdProxyPacket)); // Message type
        packet.Write(static_cast<u8>(0x02));         // Family
        const std::array<u8, 4> foreign_ip{0x0A, 0x0B, 0x0C, 0x0D};
        packet.Write(foreign_ip);                    // The source address of the sender

        REQUIRE(packet.OverwriteBytes(RelaySourceIpOffset, assigned_ip.data(), assigned_ip.size()));
    }

    SECTION("Proxy and LDN relay packet header+payload boundary checks") {
        const IPv4Address addr{192, 168, 1, 2};
        const std::vector<u8> max_payload(MaxRelayedPayloadSize, 0xAB);

        Packet proxy_packet;
        proxy_packet.Write(static_cast<u8>(IdProxyPacket));
        proxy_packet.Write(static_cast<u8>(Domain::INET));
        proxy_packet.Write(addr);
        proxy_packet.Write(static_cast<u16>(1234));
        proxy_packet.Write(static_cast<u8>(Domain::INET));
        proxy_packet.Write(addr);
        proxy_packet.Write(static_cast<u16>(5678));
        proxy_packet.Write(static_cast<u8>(Protocol::UDP));
        proxy_packet.Write(false);
        proxy_packet.Write(max_payload);

        REQUIRE(proxy_packet.GetDataSize() == ProxyPacketHeaderSize + MaxRelayedPayloadSize);
        CHECK(IsRelayedPacketSizeWithinCap(proxy_packet.GetDataSize(), ProxyPacketHeaderSize));
        CHECK_FALSE(
            IsRelayedPacketSizeWithinCap(proxy_packet.GetDataSize() + 1, ProxyPacketHeaderSize));
        CHECK_FALSE(IsRelayedPacketSizeWithinCap(ProxyPacketHeaderSize - 1, ProxyPacketHeaderSize));

        Packet ldn_packet;
        ldn_packet.Write(static_cast<u8>(IdLdnPacket));
        ldn_packet.Write(static_cast<u8>(LDNPacketType::Scan));
        ldn_packet.Write(addr);
        ldn_packet.Write(addr);
        ldn_packet.Write(true);
        ldn_packet.Write(max_payload);

        REQUIRE(ldn_packet.GetDataSize() == LdnPacketHeaderSize + MaxRelayedPayloadSize);
        CHECK(IsRelayedPacketSizeWithinCap(ldn_packet.GetDataSize(), LdnPacketHeaderSize));
        CHECK_FALSE(
            IsRelayedPacketSizeWithinCap(ldn_packet.GetDataSize() + 1, LdnPacketHeaderSize));
        CHECK_FALSE(IsRelayedPacketSizeWithinCap(LdnPacketHeaderSize - 1, LdnPacketHeaderSize));
    }
}

TEST_CASE("RoomMember GetNickname returns by value safely", "[network]") {
    RoomMember member;
    REQUIRE(member.GetNickname().empty());

    auto nickname = member.GetNickname();
    nickname += "mutated";

    // A later read must not observe the mutation, proving GetNickname returns by value.
    REQUIRE(member.GetNickname().empty());
}

} // Anonymous namespace
} // namespace Network
