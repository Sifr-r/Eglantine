// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
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

    SECTION("A connected member caps oversized game info and does not duplicate on rejoin") {
        Room room;
        u16 port = 0;
        for (u16 candidate = 24879; candidate <= 24899; ++candidate) {
            if (room.Create("test room", "a description", "127.0.0.1", candidate)) {
                port = candidate;
                break;
            }
        }
        REQUIRE(port != 0);

        const auto wait_until = [](auto&& predicate) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                if (predicate()) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return predicate();
        };

        RoomMember member;
        SCOPE_EXIT {
            member.Leave();
        };
        const IPv4Address preferred_ip{192, 168, 7, 42};
        member.Join("player_one", "127.0.0.1", port, 0, preferred_ip);
        REQUIRE(wait_until([&] { return member.GetState() == RoomMember::State::Joined; }));
        REQUIRE(room.GetRoomMemberList().size() == 1);
        REQUIRE(member.GetMemberInformation().size() == 1);
        REQUIRE(member.GetFakeIpAddress() == preferred_ip);

        member.SendGameInfo(GameInfo{
            .name = std::string(1024, 'A'),
            .id = 1,
            .version = std::string(1024, 'B'),
        });
        REQUIRE(wait_until([&] {
            const auto current_members = room.GetRoomMemberList();
            return current_members.size() == 1 && !current_members[0].game.name.empty();
        }));
        const auto members_after_game_info = room.GetRoomMemberList();
        REQUIRE(members_after_game_info.size() == 1);
        REQUIRE(members_after_game_info[0].game.name.size() == 256);
        REQUIRE(members_after_game_info[0].game.version.size() == 256);

        RoomMember colliding_member;
        std::atomic<bool> saw_name_collision{false};
        const auto error_handle = colliding_member.BindOnError([&](const RoomMember::Error& error) {
            if (error == RoomMember::Error::NameCollision) {
                saw_name_collision.store(true, std::memory_order_release);
            }
        });
        SCOPE_EXIT {
            colliding_member.Unbind(error_handle);
            colliding_member.Leave();
        };
        colliding_member.Join("player_one", "127.0.0.1", port);
        REQUIRE(wait_until([&] {
            return saw_name_collision.load(std::memory_order_acquire) &&
                   colliding_member.GetState() == RoomMember::State::Idle;
        }));
        REQUIRE(colliding_member.GetMemberInformation().empty());

        member.Join("player_one", "127.0.0.1", port);
        REQUIRE(wait_until([&] { return member.GetState() == RoomMember::State::Joined; }));
        REQUIRE(room.GetRoomMemberList().size() == 1);
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
