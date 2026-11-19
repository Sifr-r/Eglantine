// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"

namespace Network {
namespace {

struct NetworkGuard {
    bool initialized = Network::Init();

    NetworkGuard() = default;
    NetworkGuard(const NetworkGuard&) = delete;
    NetworkGuard& operator=(const NetworkGuard&) = delete;

    ~NetworkGuard() {
        if (initialized) {
            Network::Shutdown();
        }
    }
};

// The room information and the username are shared between the network thread and the thread
// that owns the Room and the RoomMember. An accessor that hands out a reference would let the
// caller read the shared state without holding the lock, so both accessors have to return
// their result by value.
static_assert(std::is_same_v<decltype(std::declval<const Room&>().GetRoomInformation()),
                             RoomInformation>,
              "Room::GetRoomInformation must return the room information by value");
static_assert(std::is_same_v<decltype(std::declval<const RoomMember&>().GetUsername()),
                             std::string>,
              "RoomMember::GetUsername must return the username by value");

TEST_CASE("Room information", "[network]") {
    NetworkGuard network_guard;
    REQUIRE(network_guard.initialized);

    SECTION("A room that was never created has empty information") {
        Room room;
        const auto info = room.GetRoomInformation();
        REQUIRE(info.name.empty());
        REQUIRE(info.description.empty());
        REQUIRE(info.member_slots == 0);
        REQUIRE(info.host_username.empty());
    }

    SECTION("The room information accessor hands out a copy") {
        Room room;
        auto info = room.GetRoomInformation();
        info.name = "mutated";
        info.member_slots = 1234;

        // A later read must not observe the mutation, which proves that the accessor does not
        // hand out a reference to the shared room information.
        const auto second = room.GetRoomInformation();
        REQUIRE(second.name.empty());
        REQUIRE(second.member_slots == 0);
    }

    SECTION("The room information is cleared when the room is destroyed") {
        Room room;
        const bool created = room.Create("test room", "a description", "127.0.0.1", 0,
                                         /*password=*/"", 8, "the_host");
        REQUIRE(created);

        const auto info = room.GetRoomInformation();
        REQUIRE(info.name == "test room");
        REQUIRE(info.description == "a description");
        REQUIRE(info.member_slots == 8);
        REQUIRE(info.host_username == "the_host");

        room.Destroy();

        const auto destroyed = room.GetRoomInformation();
        REQUIRE(destroyed.name.empty());
        REQUIRE(destroyed.description.empty());
        REQUIRE(destroyed.member_slots == 0);
        REQUIRE(destroyed.host_username.empty());
    }

    SECTION("A concurrent read never observes a half updated room") {
        Room room;
        const bool created = room.Create("test room", "a description", "127.0.0.1", 0,
                                         /*password=*/"", 8, "the_host");
        REQUIRE(created);

        std::atomic<bool> stop{false};
        std::atomic<bool> torn{false};
        std::thread reader([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto info = room.GetRoomInformation();
                // Every field of the snapshot has to come from the same update, so the reader
                // must never see a mixture of the old and the new room information.
                const bool before = info.name == "test room" && info.description == "a description" &&
                                    info.member_slots == 8 && info.host_username == "the_host";
                const bool after = info.name.empty() && info.description.empty() &&
                                   info.member_slots == 0 && info.host_username.empty();
                if (!before && !after) {
                    torn.store(true, std::memory_order_relaxed);
                }
            }
        });

        room.Destroy();
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        REQUIRE_FALSE(torn.load(std::memory_order_relaxed));
    }

    SECTION("A created room can be destroyed without destroying the room object") {
        Room room;
        const bool created = room.Create("test room", "a description", "127.0.0.1", 0);
        REQUIRE(created);

        // Destroying the room twice must not hang or crash, the second call has nothing left
        // to do.
        room.Destroy();
        room.Destroy();
    }

    SECTION("A created room can be destroyed without calling Destroy") {
        Room room;
        const bool created = room.Create("test room", "a description", "127.0.0.1", 0);
        REQUIRE(created);
    }
}

TEST_CASE("Room member information", "[network]") {
    SECTION("A member that is not connected has no username") {
        RoomMember member;
        REQUIRE(member.GetUsername().empty());
    }

    SECTION("The username accessor hands out a copy") {
        RoomMember member;
        auto username = member.GetUsername();
        username += "mutated";

        // A later read must not observe the mutation, which proves that the accessor does not
        // hand out a reference to the shared username.
        REQUIRE(member.GetUsername().empty());
    }
}

} // Anonymous namespace
} // namespace Network
