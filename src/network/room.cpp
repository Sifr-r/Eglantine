// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <vector>
#include "common/polyfill_thread.h"
#include "common/logging.h"
#include "enet/enet.h"
#include "network/packet.h"
#include "network/room.h"
#include "network/verify_user.h"

namespace Network {

/// Maximum size of a payload that a member may relay to the other members of a room. This is
/// sized for the largest UDP datagram that can be sent over IPv4.
constexpr std::size_t MaxRelayedPayloadSize = 65507;

/// Maximum length of a game name or version string accepted in a game info packet.
constexpr std::size_t MaxGameInfoStringSize = 256;

class Room::RoomImpl {
public:
    ENetHost* server = nullptr; ///< Network interface.

    std::atomic<State> state{State::Closed}; ///< Current state of the room.
    RoomInformation room_information;        ///< Information about this room.
    /// Mutex for locking room_information. The room information is read by the UI thread and
    /// written by the room network thread, so every access has to be guarded by this mutex.
    mutable std::mutex room_information_mutex;

    std::string verify_uid;              ///< A GUID which may be used for verification.
    mutable std::mutex verify_uid_mutex; ///< Mutex for verify_uid

    std::string password; ///< The password required to connect to this room.

    struct Member {
        std::string nickname; ///< The nickname of the member.
        GameInfo game_info;   ///< The current game of the member
        IPv4Address fake_ip;  ///< The assigned fake ip address of the member.
        /// Data of the user, often including authenticated forum username.
        VerifyUser::UserData user_data;
        ENetPeer* peer; ///< The remote peer.
    };
    using MemberList = std::vector<Member>;
    MemberList members;                     ///< Information about the members of this room
    mutable std::shared_mutex member_mutex; ///< Mutex for locking the members list

    UsernameBanList username_ban_list; ///< List of banned usernames
    IPBanList ip_ban_list;             ///< List of banned IP addresses
    mutable std::mutex ban_list_mutex; ///< Mutex for the ban lists

    /// Verification backend of the room
    std::unique_ptr<VerifyUser::Backend> verify_backend;

    /// Thread that receives and dispatches network packets.
    /// Declared last so it is stopped and joined before any state it accesses is destroyed.
    std::optional<std::jthread> room_thread;

    RoomImpl() = default;
    ~RoomImpl() {
        state = State::Closed;
        room_thread.reset();
        if (server) {
            enet_host_destroy(server);
            server = nullptr;
        }
    }

    void StartLoop();

    /**
     * Parses and answers a room join request from a client.
     * Validates the uniqueness of the username and assigns the MAC address
     * that the client will use for the remainder of the connection.
     */
    void HandleJoinRequest(const ENetEvent* event);

    /**
     * Parses and answers a kick request from a client.
     * Validates the permissions and that the given user exists and then kicks the member.
     */
    void HandleModKickPacket(const ENetEvent* event);

    /**
     * Parses and answers a ban request from a client.
     * Validates the permissions and bans the user (by forum username or IP).
     */
    void HandleModBanPacket(const ENetEvent* event);

    /**
     * Parses and answers a unban request from a client.
     * Validates the permissions and unbans the address.
     */
    void HandleModUnbanPacket(const ENetEvent* event);

    /**
     * Parses and answers a get ban list request from a client.
     * Validates the permissions and returns the ban list.
     */
    void HandleModGetBanListPacket(const ENetEvent* event);

    /**
     * Returns whether the nickname is valid, ie. isn't already taken by someone else in the room.
     */
    bool IsValidNickname(const std::string& nickname) const;

    /**
     * Returns whether the fake ip address is valid, ie. isn't already taken by someone else in the
     * room.
     */
    bool IsValidFakeIPAddress(const IPv4Address& address) const;

    /**
     * Returns whether a user has mod permissions.
     */
    bool HasModPermission(const ENetPeer* client) const;

    /**
     * Returns whether the given peer is a member of this room.
     */
    bool IsMember(const ENetPeer* client) const;

    /**
     * Returns the fake ip address that was assigned to the member with the given peer, or
     * std::nullopt if the peer is not a member of this room.
     */
    std::optional<IPv4Address> GetMemberFakeIp(const ENetPeer* client) const;

    /**
     * Sends a ID_ROOM_IS_FULL message telling the client that the room is full.
     */
    void SendRoomIsFull(ENetPeer* client);

    /**
     * Sends a ID_ROOM_NAME_COLLISION message telling the client that the name is invalid.
     */
    void SendNameCollision(ENetPeer* client);

    /**
     * Sends a ID_ROOM_IP_COLLISION message telling the client that the IP is invalid.
     */
    void SendIPCollision(ENetPeer* client);

    /**
     * Sends a ID_ROOM_VERSION_MISMATCH message telling the client that the version is invalid.
     */
    void SendVersionMismatch(ENetPeer* client);

    /**
     * Sends a ID_ROOM_WRONG_PASSWORD message telling the client that the password is wrong.
     */
    void SendWrongPassword(ENetPeer* client);

    /**
     * Notifies the member that its connection attempt was successful,
     * and it is now part of the room.
     */
    void SendJoinSuccess(ENetPeer* client, IPv4Address fake_ip);

    /**
     * Notifies the member that its connection attempt was successful,
     * and it is now part of the room, and it has been granted mod permissions.
     */
    void SendJoinSuccessAsMod(ENetPeer* client, IPv4Address fake_ip);

    /**
     * Sends a IdHostKicked message telling the client that they have been kicked.
     */
    void SendUserKicked(ENetPeer* client);

    /**
     * Sends a IdHostBanned message telling the client that they have been banned.
     */
    void SendUserBanned(ENetPeer* client);

    /**
     * Sends a IdModPermissionDenied message telling the client that they do not have mod
     * permission.
     */
    void SendModPermissionDenied(ENetPeer* client);

    /**
     * Sends a IdModNoSuchUser message telling the client that the given user could not be found.
     */
    void SendModNoSuchUser(ENetPeer* client);

    /**
     * Sends the ban list in response to a client's request for getting ban list.
     */
    void SendModBanListResponse(ENetPeer* client);

    /**
     * Notifies the members that the room is closed,
     */
    void SendCloseMessage();

    /**
     * Sends a system message to all the connected clients.
     */
    void SendStatusMessage(StatusMessageTypes type, const std::string& nickname,
                           const std::string& username, const std::string& ip);

    /**
     * Sends the information about the room, along with the list of members
     * to every connected client in the room.
     * The packet has the structure:
     * <MessageID>ID_ROOM_INFORMATION
     * <String> room_name
     * <String> room_description
     * <u32> member_slots: The max number of clients allowed in this room
     * <String> uid
     * <u16> port
     * <u32> num_members: the number of currently joined clients
     * This is followed by the following three values for each member:
     * <String> nickname of that member
     * <IPv4Address> fake_ip of that member
     * <String> game_name of that member
     */
    void BroadcastRoomInformation();

    /**
     * Generates a free MAC address to assign to a new client.
     * The first 3 bytes are the NintendoOUI 0x00, 0x1F, 0x32
     */
    IPv4Address GenerateFakeIPAddress();

    /**
     * Broadcasts this packet to all members except the sender.
     * @param event The ENet event containing the data
     */
    void HandleProxyPacket(const ENetEvent* event);

    /**
     * Broadcasts this packet to all members except the sender.
     * @param event The ENet event containing the data
     */
    void HandleLdnPacket(const ENetEvent* event);

    /**
     * Extracts a chat entry from a received ENet packet and adds it to the chat queue.
     * @param event The ENet event that was received.
     */
    void HandleChatPacket(const ENetEvent* event);

    /**
     * Extracts the game name from a received ENet packet and broadcasts it.
     * @param event The ENet event that was received.
     */
    void HandleGameInfoPacket(const ENetEvent* event);

    /**
     * Removes the client from the members list if it was in it and announces the change
     * to all other clients.
     */
    void HandleClientDisconnection(ENetPeer* client);

    /**
     * Returns a snapshot of the room information.
     *
     * The room information is shared between the room network thread and the thread that owns
     * the Room object, so it must never be read or written without holding
     * room_information_mutex.
     */
    RoomInformation GetRoomInformationSnapshot() const {
        std::lock_guard lock(room_information_mutex);
        return room_information;
    }

    /**
     * Returns the maximum number of members that may join this room.
     */
    u32 GetMemberSlots() const {
        std::lock_guard lock(room_information_mutex);
        return room_information.member_slots;
    }

    /**
     * Returns the forum username of the host of this room.
     */
    std::string GetHostUsername() const {
        std::lock_guard lock(room_information_mutex);
        return room_information.host_username;
    }
};

// RoomImpl
void Room::RoomImpl::StartLoop() {
    room_thread.emplace([&](std::stop_token stoken) {
        // The stop token has to be honoured, because the destruction of the thread joins it.
        // Without it, a Room that was created but never destroyed would block the process
        // forever, because the loop would only ever end when the room is closed explicitly.
        while (state != State::Closed && !stoken.stop_requested()) {
            ENetEvent event;
            if (enet_host_service(server, &event, 5) > 0) {
                switch (event.type) {
                case ENET_EVENT_TYPE_RECEIVE:
                    if (event.packet->dataLength == 0) {
                        LOG_WARNING(Network, "Received an empty packet from a peer. Ignoring it.");
                        enet_packet_destroy(event.packet);
                        break;
                    }
                    // Every message except the join request requires the sender to be a
                    // member of the room. A peer that never joined must not be able to
                    // relay traffic or to use the moderation requests.
                    if (event.packet->data[0] != IdJoinRequest && !IsMember(event.peer)) {
                        LOG_WARNING(Network,
                                    "Received a packet of type {} from a peer that is not a member "
                                    "of the room. Ignoring it.",
                                    static_cast<u32>(event.packet->data[0]));
                        enet_packet_destroy(event.packet);
                        break;
                    }
                    switch (event.packet->data[0]) {
                    case IdJoinRequest:
                        HandleJoinRequest(&event);
                        break;
                    case IdSetGameInfo:
                        HandleGameInfoPacket(&event);
                        break;
                    case IdProxyPacket:
                        HandleProxyPacket(&event);
                        break;
                    case IdLdnPacket:
                        HandleLdnPacket(&event);
                        break;
                    case IdChatMessage:
                        HandleChatPacket(&event);
                        break;
                    // Moderation
                    case IdModKick:
                        HandleModKickPacket(&event);
                        break;
                    case IdModBan:
                        HandleModBanPacket(&event);
                        break;
                    case IdModUnban:
                        HandleModUnbanPacket(&event);
                        break;
                    case IdModGetBanList:
                        HandleModGetBanListPacket(&event);
                        break;
                    }
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    HandleClientDisconnection(event.peer);
                    break;
                case ENET_EVENT_TYPE_NONE:
                case ENET_EVENT_TYPE_CONNECT:
                    break;
                }
            }
        }
        // Close the connection to all members:
        SendCloseMessage();
    });
}

void Room::RoomImpl::HandleJoinRequest(const ENetEvent* event) {
    if (IsMember(event->peer)) {
        LOG_WARNING(Network,
                    "Received a duplicate join request from an existing member. Ignoring it.");
        return;
    }

    // The member limit is read before the member list is locked, so that the two locks are
    // never held at the same time.
    const u32 member_slots = GetMemberSlots();
    {
        std::lock_guard lock(member_mutex);
        if (members.size() >= member_slots) {
            SendRoomIsFull(event->peer);
            return;
        }
    }
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type
    std::string nickname;
    packet.Read(nickname);

    IPv4Address preferred_fake_ip{};
    packet.Read(preferred_fake_ip);

    u32 client_version = 0;
    packet.Read(client_version);

    std::string pass;
    packet.Read(pass);

    std::string token;
    packet.Read(token);

    if (!packet) {
        LOG_WARNING(Network, "Received a malformed join request packet. Ignoring it.");
        return;
    }

    if (pass != password) {
        SendWrongPassword(event->peer);
        return;
    }

    if (!IsValidNickname(nickname)) {
        SendNameCollision(event->peer);
        return;
    }

    if (preferred_fake_ip != NoPreferredIP) {
        // Verify if the preferred fake ip is available
        if (!IsValidFakeIPAddress(preferred_fake_ip)) {
            SendIPCollision(event->peer);
            return;
        }
    } else {
        // Assign a fake ip address of this client automatically
        preferred_fake_ip = GenerateFakeIPAddress();
    }

    if (client_version != network_version) {
        SendVersionMismatch(event->peer);
        return;
    }

    // At this point the client is ready to be added to the room.
    Member member{};
    member.fake_ip = preferred_fake_ip;
    member.nickname = nickname;
    member.peer = event->peer;

    std::string uid;
    {
        std::lock_guard lock(verify_uid_mutex);
        uid = verify_uid;
    }

    if (verify_backend != nullptr)
     member.user_data = verify_backend->LoadUserData(uid, token);

    // Moderator rights are only granted by the verification backend, which authenticates the
    // user. The nickname is client supplied and must never be used for that. The room host is
    // recognized by its authenticated username instead, see HasModPermission.
    if (member.user_data.moderator) {
        LOG_INFO(Network, "User {} ({}) has been granted moderator permissions", member.nickname,
                 member.user_data.username);
    }

    std::string ip;
    {
        std::lock_guard lock(ban_list_mutex);

        // Check username ban
        if (!member.user_data.username.empty() &&
            std::find(username_ban_list.begin(), username_ban_list.end(),
                      member.user_data.username) != username_ban_list.end()) {

            SendUserBanned(event->peer);
            return;
        }

        // Check IP ban
        std::array<char, 256> ip_raw{};
        enet_address_get_host_ip(&event->peer->address, ip_raw.data(), sizeof(ip_raw) - 1);
        ip = ip_raw.data();

        if (std::find(ip_ban_list.begin(), ip_ban_list.end(), ip) != ip_ban_list.end()) {
            SendUserBanned(event->peer);
            return;
        }
    }

    // Notify everyone that the user has joined.
    SendStatusMessage(IdMemberJoin, member.nickname, member.user_data.username, ip);

    {
        std::lock_guard lock(member_mutex);
        members.push_back(std::move(member));
    }

    // Notify everyone that the room information has changed.
    BroadcastRoomInformation();
    if (HasModPermission(event->peer)) {
        SendJoinSuccessAsMod(event->peer, preferred_fake_ip);
    } else {
        SendJoinSuccess(event->peer, preferred_fake_ip);
    }
}

void Room::RoomImpl::HandleModKickPacket(const ENetEvent* event) {
    if (!HasModPermission(event->peer)) {
        SendModPermissionDenied(event->peer);
        return;
    }

    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    std::string nickname;
    packet.Read(nickname);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed mod kick packet. Ignoring it.");
        return;
    }

    std::string username, ip;
    {
        std::lock_guard lock(member_mutex);
        const auto target_member =
            std::find_if(members.begin(), members.end(),
                         [&nickname](const auto& member) { return member.nickname == nickname; });
        if (target_member == members.end()) {
            SendModNoSuchUser(event->peer);
            return;
        }

        // Notify the kicked member
        SendUserKicked(target_member->peer);

        username = target_member->user_data.username;

        std::array<char, 256> ip_raw{};
        enet_address_get_host_ip(&target_member->peer->address, ip_raw.data(), sizeof(ip_raw) - 1);
        ip = ip_raw.data();

        enet_peer_disconnect(target_member->peer, 0);
        members.erase(target_member);
    }

    // Announce the change to all clients.
    SendStatusMessage(IdMemberKicked, nickname, username, ip);
    BroadcastRoomInformation();
}

void Room::RoomImpl::HandleModBanPacket(const ENetEvent* event) {
    if (!HasModPermission(event->peer)) {
        SendModPermissionDenied(event->peer);
        return;
    }

    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    std::string nickname;
    packet.Read(nickname);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed mod ban packet. Ignoring it.");
        return;
    }

    std::string username, ip;
    {
        std::lock_guard lock(member_mutex);
        const auto target_member =
            std::find_if(members.begin(), members.end(),
                         [&nickname](const auto& member) { return member.nickname == nickname; });
        if (target_member == members.end()) {
            SendModNoSuchUser(event->peer);
            return;
        }

        // Notify the banned member
        SendUserBanned(target_member->peer);

        nickname = target_member->nickname;
        username = target_member->user_data.username;

        std::array<char, 256> ip_raw{};
        enet_address_get_host_ip(&target_member->peer->address, ip_raw.data(), sizeof(ip_raw) - 1);
        ip = ip_raw.data();

        enet_peer_disconnect(target_member->peer, 0);
        members.erase(target_member);
    }

    {
        std::lock_guard lock(ban_list_mutex);

        if (!username.empty()) {
            // Ban the forum username
            if (std::find(username_ban_list.begin(), username_ban_list.end(), username) ==
                username_ban_list.end()) {

                username_ban_list.emplace_back(username);
            }
        }

        // Ban the member's IP as well
        if (std::find(ip_ban_list.begin(), ip_ban_list.end(), ip) == ip_ban_list.end()) {
            ip_ban_list.emplace_back(ip);
        }
    }

    // Announce the change to all clients.
    SendStatusMessage(IdMemberBanned, nickname, username, ip);
    BroadcastRoomInformation();
}

void Room::RoomImpl::HandleModUnbanPacket(const ENetEvent* event) {
    if (!HasModPermission(event->peer)) {
        SendModPermissionDenied(event->peer);
        return;
    }

    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    std::string address;
    packet.Read(address);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed mod unban packet. Ignoring it.");
        return;
    }

    bool unbanned = false;
    {
        std::lock_guard lock(ban_list_mutex);

        auto it = std::find(username_ban_list.begin(), username_ban_list.end(), address);
        if (it != username_ban_list.end()) {
            unbanned = true;
            username_ban_list.erase(it);
        }

        it = std::find(ip_ban_list.begin(), ip_ban_list.end(), address);
        if (it != ip_ban_list.end()) {
            unbanned = true;
            ip_ban_list.erase(it);
        }
    }

    if (unbanned) {
        SendStatusMessage(IdAddressUnbanned, address, "", "");
    } else {
        SendModNoSuchUser(event->peer);
    }
}

void Room::RoomImpl::HandleModGetBanListPacket(const ENetEvent* event) {
    if (!HasModPermission(event->peer)) {
        SendModPermissionDenied(event->peer);
        return;
    }

    SendModBanListResponse(event->peer);
}

bool Room::RoomImpl::IsValidNickname(const std::string& nickname) const {
    // A nickname is valid if it matches the regex and is not already taken by anybody else in the
    // room.
    const std::regex nickname_regex("^[ a-zA-Z0-9._-]{4,20}$");
    if (!std::regex_match(nickname, nickname_regex))
        return false;

    std::lock_guard lock(member_mutex);
    return std::all_of(members.begin(), members.end(),
                       [&nickname](const auto& member) { return member.nickname != nickname; });
}

bool Room::RoomImpl::IsValidFakeIPAddress(const IPv4Address& address) const {
    // An IP address is valid if it is not already taken by anybody else in the room.
    std::lock_guard lock(member_mutex);
    return std::all_of(members.begin(), members.end(),
                       [&address](const auto& member) { return member.fake_ip != address; });
}

bool Room::RoomImpl::HasModPermission(const ENetPeer* client) const {
    // The host username is read before the member list is locked, so that the two locks are
    // never held at the same time.
    const std::string host_username = GetHostUsername();
    std::lock_guard lock(member_mutex);
    const auto sending_member =
        std::find_if(members.begin(), members.end(),
                     [client](const auto& member) { return member.peer == client; });
    if (sending_member == members.end()) {
        return false;
    }
    if (sending_member->user_data.moderator) { // Community moderator

        return true;
    }
    if (!host_username.empty() &&
        sending_member->user_data.username == host_username) { // Room host

        return true;
    }
    return false;
}

bool Room::RoomImpl::IsMember(const ENetPeer* client) const {
    std::lock_guard lock(member_mutex);
    return std::any_of(members.begin(), members.end(),
                       [client](const auto& member) { return member.peer == client; });
}

std::optional<IPv4Address> Room::RoomImpl::GetMemberFakeIp(const ENetPeer* client) const {
    std::lock_guard lock(member_mutex);
    const auto sending_member =
        std::find_if(members.begin(), members.end(),
                     [client](const auto& member) { return member.peer == client; });
    if (sending_member == members.end()) {
        return std::nullopt;
    }
    return sending_member->fake_ip;
}

void Room::RoomImpl::SendNameCollision(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdNameCollision));

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendIPCollision(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdIpCollision));

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendWrongPassword(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdWrongPassword));

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendRoomIsFull(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdRoomIsFull));

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendVersionMismatch(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdVersionMismatch));
    packet.Write(network_version);

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendJoinSuccess(ENetPeer* client, IPv4Address fake_ip) {
    Packet packet;
    packet.Write(static_cast<u8>(IdJoinSuccess));
    packet.Write(fake_ip);
    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendJoinSuccessAsMod(ENetPeer* client, IPv4Address fake_ip) {
    Packet packet;
    packet.Write(static_cast<u8>(IdJoinSuccessAsMod));
    packet.Write(fake_ip);
    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendUserKicked(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdHostKicked));

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendUserBanned(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdHostBanned));

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendModPermissionDenied(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdModPermissionDenied));

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendModNoSuchUser(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdModNoSuchUser));

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendModBanListResponse(ENetPeer* client) {
    Packet packet;
    packet.Write(static_cast<u8>(IdModBanListResponse));
    {
        std::lock_guard lock(ban_list_mutex);
        packet.Write(username_ban_list);
        packet.Write(ip_ban_list);
    }

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(client, 0, enet_packet);
    enet_host_flush(server);
}

void Room::RoomImpl::SendCloseMessage() {
    Packet packet;
    packet.Write(static_cast<u8>(IdCloseRoom));
    std::lock_guard lock(member_mutex);
    if (!members.empty()) {
        ENetPacket* enet_packet =
            enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
        for (auto& member : members) {
            enet_peer_send(member.peer, 0, enet_packet);
        }
    }
    enet_host_flush(server);
    for (auto& member : members) {
        enet_peer_disconnect(member.peer, 0);
    }
}

void Room::RoomImpl::SendStatusMessage(StatusMessageTypes type, const std::string& nickname,
                                       const std::string& username, const std::string& ip) {
    Packet packet;
    packet.Write(static_cast<u8>(IdStatusMessage));
    packet.Write(static_cast<u8>(type));
    packet.Write(nickname);
    packet.Write(username);
    std::lock_guard lock(member_mutex);
    if (!members.empty()) {
        ENetPacket* enet_packet =
            enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
        for (auto& member : members) {
            enet_peer_send(member.peer, 0, enet_packet);
        }
    }
    enet_host_flush(server);

    const std::string display_name =
        username.empty() ? nickname : fmt::format("{} ({})", nickname, username);

    switch (type) {
    case IdMemberJoin:
        LOG_INFO(Network, "[{}] {} has joined.", ip, display_name);
        break;
    case IdMemberLeave:
        LOG_INFO(Network, "[{}] {} has left.", ip, display_name);
        break;
    case IdMemberKicked:
        LOG_INFO(Network, "[{}] {} has been kicked.", ip, display_name);
        break;
    case IdMemberBanned:
        LOG_INFO(Network, "[{}] {} has been banned.", ip, display_name);
        break;
    case IdAddressUnbanned:
        LOG_INFO(Network, "{} has been unbanned.", display_name);
        break;
    }
}

void Room::RoomImpl::BroadcastRoomInformation() {
    // Copy the member list under the lock, so that the packet is built from a consistent
    // snapshot of the room.
    MemberList member_snapshot;
    {
        std::lock_guard lock(member_mutex);
        member_snapshot = members;
    }

    Packet packet;
    packet.Write(static_cast<u8>(IdRoomInformation));
    const RoomInformation info = GetRoomInformationSnapshot();
    packet.Write(info.name);
    packet.Write(info.description);
    packet.Write(info.member_slots);
    packet.Write(info.port);
    packet.Write(info.preferred_game.name);
    packet.Write(info.host_username);

    packet.Write(static_cast<u32>(member_snapshot.size()));
    for (const auto& member : member_snapshot) {
        packet.Write(member.nickname);
        packet.Write(member.fake_ip);
        packet.Write(member.game_info.name);
        packet.Write(member.game_info.id);
        packet.Write(member.game_info.version);
        packet.Write(member.user_data.username);
        packet.Write(member.user_data.display_name);
        packet.Write(member.user_data.avatar_url);
    }

    ENetPacket* enet_packet =
        enet_packet_create(packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(server, 0, enet_packet);
    enet_host_flush(server);
}

IPv4Address Room::RoomImpl::GenerateFakeIPAddress() {
    // An IP address is valid if it is not already taken by anybody else in the room.
    std::lock_guard lock(member_mutex);
    for (u8 i = 0x01; i < 0xFF; ++i)
        for (u8 j = 0x01; j < 0xFF; ++j) {
            IPv4Address addr{192, 168, i, j};
            if (std::all_of(members.begin(), members.end(), [&addr](auto const& member) { return member.fake_ip != addr; }))
                return addr;
        }
    LOG_ERROR(Network, "All addresses are taken");
    return IPv4Address{192, 168, 0, 0};
}

void Room::RoomImpl::HandleProxyPacket(const ENetEvent* event) {
    Packet in_packet;
    in_packet.Append(event->packet->data, event->packet->dataLength);
    in_packet.IgnoreBytes(sizeof(u8)); // Message type

    u8 local_family = 0;
    in_packet.Read(local_family); // Domain
    IPv4Address local_ip{};
    in_packet.Read(local_ip);     // IP
    u16 local_port = 0;
    in_packet.Read(local_port); // Port

    u8 remote_family = 0;
    in_packet.Read(remote_family); // Domain
    IPv4Address remote_ip{};
    in_packet.Read(remote_ip);      // IP
    u16 remote_port = 0;
    in_packet.Read(remote_port); // Port

    u8 protocol = 0;
    in_packet.Read(protocol); // Protocol

    bool broadcast = false;
    in_packet.Read(broadcast); // Broadcast

    // The payload is relayed exactly as it was received, so it is not copied here. Its length
    // is read to validate the structure of the packet and to cap the amount of data that a
    // member can make the room relay.
    u32 payload_size = 0;
    in_packet.Read(payload_size);
    if (!in_packet) {
        LOG_WARNING(Network, "Received a malformed proxy packet. Ignoring it.");
        return;
    }
    if (payload_size > MaxRelayedPayloadSize) {
        LOG_WARNING(Network,
                    "Received a proxy packet with a payload of {} bytes, which is larger than the "
                    "maximum of {} bytes. Ignoring it.",
                    payload_size, MaxRelayedPayloadSize);
        return;
    }
    in_packet.IgnoreBytes(payload_size);
    if (!in_packet || !in_packet.EndOfPacket()) {
        // The packet has to end with the payload. A member must not be able to send more data
        // than it announced, because the whole packet is relayed.
        LOG_WARNING(Network, "Received a malformed proxy packet. Ignoring it.");
        return;
    }

    // A member may only relay traffic that comes from the address that the room assigned to
    // it. The source address of the relayed packet is therefore overwritten with the assigned
    // one, so that a member cannot make traffic appear to come from another member.
    const auto sender_fake_ip = GetMemberFakeIp(event->peer);
    if (!sender_fake_ip) {
        LOG_WARNING(Network,
                    "Received a proxy packet from a peer that is not a member of the room. "
                    "Ignoring it.");
        return;
    }

    Packet out_packet;
    out_packet.Append(event->packet->data, event->packet->dataLength);
    if (local_ip != *sender_fake_ip) {
        LOG_DEBUG(Network,
                  "The source address {}.{}.{}.{} of a proxy packet was corrected to "
                  "{}.{}.{}.{}.",
                  local_ip[0], local_ip[1], local_ip[2], local_ip[3], (*sender_fake_ip)[0],
                  (*sender_fake_ip)[1], (*sender_fake_ip)[2], (*sender_fake_ip)[3]);
    }
    if (!out_packet.OverwriteBytes(RelaySourceIpOffset, sender_fake_ip->data(), sender_fake_ip->size())) {
        // The packet is too short to carry a source address. Relaying it would relay the
        // address that the member claimed, which must never happen.
        LOG_WARNING(Network,
                    "Received a proxy packet that is too short to carry a source address. "
                    "Ignoring it.");
        return;
    }
    ENetPacket* enet_packet = enet_packet_create(out_packet.GetData(), out_packet.GetDataSize(),
                                                 ENET_PACKET_FLAG_RELIABLE);

    const auto& destination_address = remote_ip;
    if (broadcast) { // Send the data to everyone except the sender
        std::lock_guard lock(member_mutex);
        bool sent_packet = false;
        for (const auto& member : members) {
            if (member.peer != event->peer) {
                sent_packet = true;
                enet_peer_send(member.peer, 0, enet_packet);
            }
        }

        if (!sent_packet) {
            enet_packet_destroy(enet_packet);
        }
    } else { // Send the data only to the destination client
        std::lock_guard lock(member_mutex);
        auto member = std::find_if(members.begin(), members.end(),
                                   [destination_address](const Member& member_entry) -> bool {
                                       return member_entry.fake_ip == destination_address;
                                   });
        if (member != members.end()) {
            enet_peer_send(member->peer, 0, enet_packet);
        } else {
            LOG_ERROR(Network,
                      "Attempting to send to unknown IP address: "
                      "{}.{}.{}.{}",
                      destination_address[0], destination_address[1], destination_address[2],
                      destination_address[3]);
            enet_packet_destroy(enet_packet);
        }
    }
    enet_host_flush(server);
}

void Room::RoomImpl::HandleLdnPacket(const ENetEvent* event) {
    Packet in_packet;
    in_packet.Append(event->packet->data, event->packet->dataLength);

    in_packet.IgnoreBytes(sizeof(u8)); // Message type

    u8 packet_type = 0;
    in_packet.Read(packet_type); // LAN packet type
    IPv4Address local_ip{};
    in_packet.Read(local_ip); // Local IP

    IPv4Address remote_ip{};
    in_packet.Read(remote_ip); // Remote IP

    bool broadcast = false;
    in_packet.Read(broadcast); // Broadcast

    // The payload is relayed exactly as it was received, so it is not copied here. Its length
    // is read to validate the structure of the packet and to cap the amount of data that a
    // member can make the room relay.
    u32 payload_size = 0;
    in_packet.Read(payload_size);
    if (!in_packet) {
        LOG_WARNING(Network, "Received a malformed LDN packet. Ignoring it.");
        return;
    }
    if (payload_size > MaxRelayedPayloadSize) {
        LOG_WARNING(Network,
                    "Received an LDN packet with a payload of {} bytes, which is larger than the "
                    "maximum of {} bytes. Ignoring it.",
                    payload_size, MaxRelayedPayloadSize);
        return;
    }
    in_packet.IgnoreBytes(payload_size);
    if (!in_packet || !in_packet.EndOfPacket()) {
        // The packet has to end with the payload. A member must not be able to send more data
        // than it announced, because the whole packet is relayed.
        LOG_WARNING(Network, "Received a malformed LDN packet. Ignoring it.");
        return;
    }

    // A member may only relay traffic that comes from the address that the room assigned to
    // it. The source address of the relayed packet is therefore overwritten with the assigned
    // one, so that a member cannot make traffic appear to come from another member.
    const auto sender_fake_ip = GetMemberFakeIp(event->peer);
    if (!sender_fake_ip) {
        LOG_WARNING(Network,
                    "Received an LDN packet from a peer that is not a member of the room. "
                    "Ignoring it.");
        return;
    }

    Packet out_packet;
    out_packet.Append(event->packet->data, event->packet->dataLength);
    if (local_ip != *sender_fake_ip) {
        LOG_DEBUG(Network,
                  "The source address {}.{}.{}.{} of an LDN packet was corrected to "
                  "{}.{}.{}.{}.",
                  local_ip[0], local_ip[1], local_ip[2], local_ip[3], (*sender_fake_ip)[0],
                  (*sender_fake_ip)[1], (*sender_fake_ip)[2], (*sender_fake_ip)[3]);
    }
    if (!out_packet.OverwriteBytes(RelaySourceIpOffset, sender_fake_ip->data(), sender_fake_ip->size())) {
        // The packet is too short to carry a source address. Relaying it would relay the
        // address that the member claimed, which must never happen.
        LOG_WARNING(Network,
                    "Received an LDN packet that is too short to carry a source address. "
                    "Ignoring it.");
        return;
    }
    ENetPacket* enet_packet = enet_packet_create(out_packet.GetData(), out_packet.GetDataSize(),
                                                 ENET_PACKET_FLAG_RELIABLE);

    const auto& destination_address = remote_ip;
    if (broadcast) { // Send the data to everyone except the sender
        std::lock_guard lock(member_mutex);
        bool sent_packet = false;
        for (const auto& member : members) {
            if (member.peer != event->peer) {
                sent_packet = true;
                enet_peer_send(member.peer, 0, enet_packet);
            }
        }

        if (!sent_packet) {
            enet_packet_destroy(enet_packet);
        }
    } else {
        std::lock_guard lock(member_mutex);
        auto member = std::find_if(members.begin(), members.end(),
                                   [destination_address](const Member& member_entry) -> bool {
                                       return member_entry.fake_ip == destination_address;
                                   });
        if (member != members.end()) {
            enet_peer_send(member->peer, 0, enet_packet);
        } else {
            LOG_ERROR(Network,
                      "Attempting to send to unknown IP address: "
                      "{}.{}.{}.{}",
                      destination_address[0], destination_address[1], destination_address[2],
                      destination_address[3]);
            enet_packet_destroy(enet_packet);
        }
    }
    enet_host_flush(server);
}

void Room::RoomImpl::HandleChatPacket(const ENetEvent* event) {
    Packet in_packet;
    in_packet.Append(event->packet->data, event->packet->dataLength);

    in_packet.IgnoreBytes(sizeof(u8)); // Ignore the message type
    std::string message;
    in_packet.Read(message);
    if (!in_packet) {
        LOG_WARNING(Network, "Received a malformed chat packet. Ignoring it.");
        return;
    }
    auto CompareNetworkAddress = [event](const Member member) -> bool {
        return member.peer == event->peer;
    };

    std::lock_guard lock(member_mutex);
    const auto sending_member = std::find_if(members.begin(), members.end(), CompareNetworkAddress);
    if (sending_member == members.end()) {
        LOG_WARNING(Network, "Received a chat message from an unknown sender. Ignoring it.");
        return;
    }

    // Limit the size of chat messages to MaxMessageSize
    message.resize((std::min)(static_cast<u32>(message.size()), MaxMessageSize));

    Packet out_packet;
    out_packet.Write(static_cast<u8>(IdChatMessage));
    out_packet.Write(sending_member->nickname);
    out_packet.Write(sending_member->user_data.username);
    out_packet.Write(message);

    ENetPacket* enet_packet = enet_packet_create(out_packet.GetData(), out_packet.GetDataSize(),
                                                 ENET_PACKET_FLAG_RELIABLE);
    bool sent_packet = false;
    for (const auto& member : members) {
        if (member.peer != event->peer) {
            sent_packet = true;
            enet_peer_send(member.peer, 0, enet_packet);
        }
    }

    if (!sent_packet) {
        enet_packet_destroy(enet_packet);
    }

    enet_host_flush(server);

    if (sending_member->user_data.username.empty()) {
        LOG_INFO(Network, "{}: {}", sending_member->nickname, message);
    } else {
        LOG_INFO(Network, "{} ({}): {}", sending_member->nickname,
                 sending_member->user_data.username, message);
    }
}

void Room::RoomImpl::HandleGameInfoPacket(const ENetEvent* event) {
    Packet in_packet;
    in_packet.Append(event->packet->data, event->packet->dataLength);

    in_packet.IgnoreBytes(sizeof(u8)); // Ignore the message type
    GameInfo game_info;
    in_packet.Read(game_info.name);
    in_packet.Read(game_info.id);
    in_packet.Read(game_info.version);
    if (!in_packet) {
        LOG_WARNING(Network, "Received a malformed game info packet. Ignoring it.");
        return;
    }

    if (game_info.name.size() > MaxGameInfoStringSize) {
        game_info.name.resize(MaxGameInfoStringSize);
    }
    if (game_info.version.size() > MaxGameInfoStringSize) {
        game_info.version.resize(MaxGameInfoStringSize);
    }

    {
        std::lock_guard lock(member_mutex);
        auto member = std::find_if(members.begin(), members.end(),
                                   [event](const Member& member_entry) -> bool {
                                       return member_entry.peer == event->peer;
                                   });
        if (member != members.end()) {
            member->game_info = game_info;

            const std::string display_name =
                member->user_data.username.empty()
                    ? member->nickname
                    : fmt::format("{} ({})", member->nickname, member->user_data.username);

            if (game_info.name.empty()) {
                LOG_INFO(Network, "{} is not playing", display_name);
            } else {
                LOG_INFO(Network, "{} is playing {} ({})", display_name, game_info.name,
                         game_info.version);
            }
        }
    }
    BroadcastRoomInformation();
}

void Room::RoomImpl::HandleClientDisconnection(ENetPeer* client) {
    // Remove the client from the members list.
    std::string nickname, username, ip;
    {
        std::lock_guard lock(member_mutex);
        auto member =
            std::find_if(members.begin(), members.end(), [client](const Member& member_entry) {
                return member_entry.peer == client;
            });
        if (member != members.end()) {
            nickname = member->nickname;
            username = member->user_data.username;

            std::array<char, 256> ip_raw{};
            enet_address_get_host_ip(&member->peer->address, ip_raw.data(), sizeof(ip_raw) - 1);
            ip = ip_raw.data();

            members.erase(member);
        }
    }

    // Announce the change to all clients.
    enet_peer_disconnect(client, 0);
    if (!nickname.empty())
        SendStatusMessage(IdMemberLeave, nickname, username, ip);
    BroadcastRoomInformation();
}

// Room
Room::Room() : room_impl{std::make_unique<RoomImpl>()} {}

Room::~Room() {
    Destroy();
}

bool Room::Create(const std::string& name, const std::string& description,
                  const std::string& server_address, u16 server_port, const std::string& password,
                  const u32 max_connections, const std::string& host_username,
                  const GameInfo preferred_game,
                  std::unique_ptr<VerifyUser::Backend> verify_backend,
                  const Room::BanList& ban_list) {
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    if (!server_address.empty()) {
        enet_address_set_host(&address, server_address.c_str());
    }
    address.port = server_port;

    // In order to send the room is full message to the connecting client, we need to leave one
    // slot open so enet won't reject the incoming connection without telling us
    room_impl->server = enet_host_create(&address, max_connections + 1, NumChannels, 0, 0);
    if (!room_impl->server) {
        return false;
    }
    room_impl->state = State::Open;

    {
        std::lock_guard lock(room_impl->room_information_mutex);
        room_impl->room_information.name = name;
        room_impl->room_information.description = description;
        room_impl->room_information.member_slots = max_connections;
        room_impl->room_information.port = server_port;
        room_impl->room_information.preferred_game = preferred_game;
        room_impl->room_information.host_username = host_username;
    }
    room_impl->password = password;
    room_impl->verify_backend = std::move(verify_backend);
    room_impl->username_ban_list = ban_list.first;
    room_impl->ip_ban_list = ban_list.second;

    room_impl->StartLoop();
    return true;
}

Room::State Room::GetState() const {
    return room_impl->state;
}

RoomInformation Room::GetRoomInformation() const {
    return room_impl->GetRoomInformationSnapshot();
}

std::string Room::GetVerifyUID() const {
    std::lock_guard lock(room_impl->verify_uid_mutex);
    return room_impl->verify_uid;
}

Room::BanList Room::GetBanList() const {
    std::lock_guard lock(room_impl->ban_list_mutex);
    return {room_impl->username_ban_list, room_impl->ip_ban_list};
}

std::vector<Member> Room::GetRoomMemberList() const {
    std::vector<Member> member_list;
    std::lock_guard lock(room_impl->member_mutex);
    for (const auto& member_impl : room_impl->members) {
        Member member;
        member.nickname = member_impl.nickname;
        member.username = member_impl.user_data.username;
        member.display_name = member_impl.user_data.display_name;
        member.avatar_url = member_impl.user_data.avatar_url;
        member.fake_ip = member_impl.fake_ip;
        member.game = member_impl.game_info;
        member_list.push_back(member);
    }
    return member_list;
}

bool Room::HasPassword() const {
    return !room_impl->password.empty();
}

void Room::SetVerifyUID(const std::string& uid) {
    std::lock_guard lock(room_impl->verify_uid_mutex);
    room_impl->verify_uid = uid;
}

void Room::Destroy() {
    room_impl->state = State::Closed;
    room_impl->room_thread.reset();

    if (room_impl->server) {
        enet_host_destroy(room_impl->server);
    }
    {
        std::lock_guard lock(room_impl->room_information_mutex);
        room_impl->room_information = {};
    }
    room_impl->server = nullptr;
    {
        std::lock_guard lock(room_impl->member_mutex);
        room_impl->members.clear();
    }
}

} // namespace Network
