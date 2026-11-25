// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include "common/assert.h"
#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/socket_types.h"
#include "enet/enet.h"
#include "network/packet.h"
#include "network/room_member.h"

namespace Network {

constexpr u32 ConnectionTimeoutMs = 5000;

/// Minimum number of bytes that a member entry of the room information occupies. Every entry
/// contains a fake ip address, the id of the game (a u64) and six strings, which are stored
/// with a length prefix.
constexpr std::size_t MinMemberEntrySize = sizeof(IPv4Address) + sizeof(u64) + 6 * sizeof(u32);

/// Maximum number of members that a room information packet is allowed to describe.
constexpr u32 MaxRoomMemberEntries = 1024;

class RoomMember::RoomMemberImpl {
public:
    void SetState(const State new_state) noexcept {
        // The state is exchanged instead of compared and stored, so that only the thread that
        // actually changed the state notifies the callbacks. Otherwise two threads that change
        // the state at the same time could both notify a state that was already reached.
        if (state.exchange(new_state) != new_state) {
            Invoke<State>(new_state);
        }
    }

    void SetError(const Error new_error) noexcept {
        Invoke<Error>(new_error);
    }

    [[nodiscard]] bool IsConnected() const noexcept {
        return state == State::Joining || state == State::Joined || state == State::Moderator;
    }

    /// Returns whether the calling thread is the network loop thread of this member.
    /// The id of the network thread is kept in its own member, so that the thread can identify
    /// itself without touching the thread object, which another thread may destroy.
    [[nodiscard]] bool IsLoopThread() const {
        return loop_thread_id.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    ENetHost* client = nullptr; ///< ENet network interface.
    ENetPeer* server = nullptr; ///< The server peer the client is connected to

    /// Information about the clients connected to the same room as us.
    MemberList member_information;
    /// Information about the room we're connected to.
    RoomInformation room_information;

    /// The current game name, id and version
    GameInfo current_game_info;

    std::atomic<State> state{State::Idle}; ///< Current state of the RoomMember.

    std::string nickname;              ///< The nickname of this member.
    mutable std::mutex nickname_mutex; ///< Mutex for locking nickname.

    std::string username;              ///< The username of this member.
    mutable std::mutex username_mutex; ///< Mutex for locking username.

    IPv4Address fake_ip; ///< The fake ip of this member.

    /// Mutex that guards the member list, the room information and the fake ip address. Those
    /// are written by the network thread while they are read by other threads.
    mutable std::mutex state_mutex;

    std::mutex network_mutex;   ///< Mutex that controls access to the `client` variable.
    std::mutex lifecycle_mutex; ///< Serializes Join, Leave, and destruction.
    /// Thread that receives and dispatches network packets
    std::optional<std::jthread> loop_thread;
    /// The id of the network loop thread. It is written when the thread starts and is never
    /// changed afterwards, so that it can be read without holding a lock.
    std::atomic<std::thread::id> loop_thread_id{};
    /// Set when the member wants to leave from the network thread itself. The network thread
    /// performs the state change when it returns to its loop.
    std::atomic<bool> leave_requested{false};
    std::atomic<bool> join_abort_requested{false};
    std::mutex send_list_mutex;  ///< Mutex that controls access to the `send_list` variable.
    std::vector<Packet> send_list; ///< A list that stores all packets to send the async

    template <typename T>
    using CallbackSet = std::set<CallbackHandle<T>>;
    std::mutex callback_mutex; ///< The mutex used for handling callbacks

    class Callbacks {
    public:
        template <typename T>
        CallbackSet<T>& Get();

    private:
        CallbackSet<ProxyPacket> callback_set_proxy_packet;
        CallbackSet<LDNPacket> callback_set_ldn_packet;
        CallbackSet<ChatEntry> callback_set_chat_messages;
        CallbackSet<StatusMessageEntry> callback_set_status_messages;
        CallbackSet<RoomInformation> callback_set_room_information;
        CallbackSet<State> callback_set_state;
        CallbackSet<Error> callback_set_error;
        CallbackSet<Room::BanList> callback_set_ban_list;
    };
    Callbacks callbacks; ///< All CallbackSets to all events

    void StartLoop();

    /**
     * Sends data to the room. It will be send on channel 0 with flag RELIABLE
     * @param packet The data to send
     */
    void Send(Packet&& packet);
    void SendGameInfoPacket(const GameInfo& game_info);

    /**
     * Sends a request to the server, asking for permission to join a room with the specified
     * nickname and preferred fake ip.
     * @params nickname The desired nickname.
     * @params preferred_fake_ip The preferred IP address to use in the room, the NoPreferredIP
     * tells
     * @params password The password for the room
     * the server to assign one for us.
     */
    void SendJoinRequest(const std::string& nickname_,
                         const IPv4Address& preferred_fake_ip = NoPreferredIP,
                         const std::string& password = "", const std::string& token = "");

    /**
     * Extracts a MAC Address from a received ENet packet.
     * @param event The ENet event that was received.
     */
    bool HandleJoinPacket(const ENetEvent* event);
    /**
     * Extracts RoomInformation and MemberInformation from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleRoomInformationPacket(const ENetEvent* event);

    /**
     * Extracts a ProxyPacket from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleProxyPackets(const ENetEvent* event);

    /**
     * Extracts an LdnPacket from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleLdnPackets(const ENetEvent* event);

    /**
     * Extracts a chat entry from a received ENet packet and adds it to the chat queue.
     * @param event The ENet event that was received.
     */
    void HandleChatPacket(const ENetEvent* event);

    /**
     * Extracts a system message entry from a received ENet packet and adds it to the system message
     * queue.
     * @param event The ENet event that was received.
     */
    void HandleStatusMessagePacket(const ENetEvent* event);

    /**
     * Extracts a ban list request response from a received ENet packet.
     * @param event The ENet event that was received.
     */
    void HandleModBanListResponsePacket(const ENetEvent* event);

    /**
     * Disconnects the RoomMember from the Room
     */
    void Disconnect();

    /// Stops the network loop and destroys the ENet client. Caller must hold lifecycle_mutex.
    void LeaveInternal();

    template <typename T>
    void Invoke(const T& data);

    template <typename T>
    CallbackHandle<T> Bind(std::function<void(const T&)> callback);
};

// RoomMemberImpl
void RoomMember::RoomMemberImpl::StartLoop() {
    leave_requested = false;
    loop_thread.emplace([&](std::stop_token stoken) {
        loop_thread_id.store(std::this_thread::get_id(), std::memory_order_release);
        // Receive packets while the connection is open
        while (IsConnected() && !leave_requested.load(std::memory_order_acquire) &&
               !stoken.stop_requested()) {
            std::lock_guard lock(network_mutex);
            ENetEvent event;
            if (enet_host_service(client, &event, 5) > 0) {
                switch (event.type) {
                case ENET_EVENT_TYPE_RECEIVE:
                    if (event.packet->dataLength == 0) {
                        LOG_WARNING(Network, "Received an empty packet from the room. Ignoring it.");
                        enet_packet_destroy(event.packet);
                        break;
                    }
                    switch (event.packet->data[0]) {
                    case IdProxyPacket:
                        HandleProxyPackets(&event);
                        break;
                    case IdLdnPacket:
                        HandleLdnPackets(&event);
                        break;
                    case IdChatMessage:
                        HandleChatPacket(&event);
                        break;
                    case IdStatusMessage:
                        HandleStatusMessagePacket(&event);
                        break;
                    case IdRoomInformation:
                        HandleRoomInformationPacket(&event);
                        break;
                    case IdJoinSuccess:
                    case IdJoinSuccessAsMod: {
                        if (event.packet->dataLength < sizeof(u8) + sizeof(IPv4Address)) {
                            LOG_WARNING(Network,
                                        "Received a malformed join success packet. Ignoring it.");
                            break;
                        }
                        // The join request was successful, we are now in the room.
                        // If we joined successfully, there must be at least one client in the room: us.
                        bool has_member_information = false;
                        {
                            std::lock_guard state_lock(state_mutex);
                            has_member_information = !member_information.empty();
                        }
                        if (!has_member_information) {
                            LOG_WARNING(Network,
                                        "We have not yet received member information. Ignoring "
                                        "join success packet.");
                            break;
                        }
                        if (!HandleJoinPacket(&event)) { // Get the MAC Address for the client
                            break;
                        }
                        if (event.packet->data[0] == IdJoinSuccessAsMod) {
                            SetState(State::Moderator);
                        } else {
                            SetState(State::Joined);
                        }
                        break;
                    }
                    case IdModBanListResponse:
                        HandleModBanListResponsePacket(&event);
                        break;
                    case IdRoomIsFull:
                        SetState(State::Idle);
                        SetError(Error::RoomIsFull);
                        break;
                    case IdNameCollision:
                        SetState(State::Idle);
                        SetError(Error::NameCollision);
                        break;
                    case IdIpCollision:
                        SetState(State::Idle);
                        SetError(Error::IpCollision);
                        break;
                    case IdVersionMismatch:
                        SetState(State::Idle);
                        SetError(Error::WrongVersion);
                        break;
                    case IdWrongPassword:
                        SetState(State::Idle);
                        SetError(Error::WrongPassword);
                        break;
                    case IdCloseRoom:
                        SetState(State::Idle);
                        SetError(Error::LostConnection);
                        break;
                    case IdHostKicked:
                        SetState(State::Idle);
                        SetError(Error::HostKicked);
                        break;
                    case IdHostBanned:
                        SetState(State::Idle);
                        SetError(Error::HostBanned);
                        break;
                    case IdModPermissionDenied:
                        SetError(Error::PermissionDenied);
                        break;
                    case IdModNoSuchUser:
                        SetError(Error::NoSuchUser);
                        break;
                    }
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    if (IsConnected()) {
                        SetState(State::Idle);
                        SetError(Error::LostConnection);
                    }
                    server = nullptr;
                    break;
                case ENET_EVENT_TYPE_NONE:
                    break;
                case ENET_EVENT_TYPE_CONNECT:
                    // The ENET_EVENT_TYPE_CONNECT event can not possibly happen here because we're
                    // already connected
                    LOG_WARNING(Network,
                                "Received unexpected connect event while already connected");
                    break;
                }
            }
            std::vector<Packet> packets;
            {
                std::lock_guard send_lock(send_list_mutex);
                packets.swap(send_list);
            }
            if (IsConnected() && server != nullptr) {
                for (auto const& packet : packets) {
                    ENetPacket* enetPacket = enet_packet_create(
                        packet.GetData(), packet.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
                    if (enetPacket != nullptr && enet_peer_send(server, 0, enetPacket) < 0) {
                        enet_packet_destroy(enetPacket);
                    }
                }
                enet_host_flush(client);
            }
        }
        if (leave_requested.exchange(false, std::memory_order_acq_rel)) {
            // The leave was requested from a callback that runs on this thread. The state
            // change is done here, outside of that callback, because the state callbacks
            // cannot be invoked while the callback mutex is held by this thread.
            SetState(State::Idle);
        }
        Disconnect();
        // The thread is gone, so that no other thread can be mistaken for it.
        loop_thread_id.store(std::thread::id{}, std::memory_order_release);
    });
}

void RoomMember::RoomMemberImpl::Send(Packet&& packet) {
    if (!IsConnected()) {
        return;
    }
    std::lock_guard lock(send_list_mutex);
    send_list.push_back(std::move(packet));
}

void RoomMember::RoomMemberImpl::SendGameInfoPacket(const GameInfo& game_info) {
    Packet packet;
    packet.Write(static_cast<u8>(IdSetGameInfo));
    packet.Write(game_info.name);
    packet.Write(game_info.id);
    packet.Write(game_info.version);
    Send(std::move(packet));
}

void RoomMember::RoomMemberImpl::SendJoinRequest(const std::string& nickname_,
                                                 const IPv4Address& preferred_fake_ip,
                                                 const std::string& password,
                                                 const std::string& token) {
    Packet packet;
    packet.Write(static_cast<u8>(IdJoinRequest));
    packet.Write(nickname_);
    packet.Write(preferred_fake_ip);
    packet.Write(network_version);
    packet.Write(password);
    packet.Write(token);
    Send(std::move(packet));
}

void RoomMember::RoomMemberImpl::HandleRoomInformationPacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    RoomInformation info{};
    packet.Read(info.name);
    packet.Read(info.description);
    packet.Read(info.member_slots);
    packet.Read(info.port);
    packet.Read(info.preferred_game.name);
    packet.Read(info.host_username);

    u32 num_members = 0;
    packet.Read(num_members);

    // The member count is controlled by the peer, so it has to be bounded by the amount of
    // data that is left in the packet and by a sane maximum before anything is allocated.
    const std::size_t max_members =
        (std::min)(static_cast<std::size_t>(MaxRoomMemberEntries),
                   packet.RemainingBytes() / MinMemberEntrySize);
    if (num_members > max_members) {
        LOG_ERROR(Network,
                  "The room information claims to contain {} members, but the packet can only "
                  "describe {}. Ignoring the packet.",
                  num_members, max_members);
        packet.Invalidate();
        return;
    }

    MemberList new_members;
    new_members.resize(num_members);

    std::string current_nickname;
    {
        std::lock_guard lock(nickname_mutex);
        current_nickname = nickname;
    }

    std::string own_username;
    bool found_own_member = false;
    for (auto& member : new_members) {
        packet.Read(member.nickname);
        packet.Read(member.fake_ip);
        packet.Read(member.game_info.name);
        packet.Read(member.game_info.id);
        packet.Read(member.game_info.version);
        packet.Read(member.username);
        packet.Read(member.display_name);
        packet.Read(member.avatar_url);

        if (member.nickname == current_nickname) {
            own_username = member.username;
            found_own_member = true;
        }
    }

    if (!packet) {
        LOG_ERROR(Network, "The room information packet is malformed. Ignoring it.");
        return;
    }

    {
        std::lock_guard lock(state_mutex);
        room_information.name = info.name;
        room_information.description = info.description;
        room_information.member_slots = info.member_slots;
        room_information.port = info.port;
        room_information.preferred_game = info.preferred_game;
        room_information.host_username = info.host_username;
        member_information = std::move(new_members);
    }

    if (found_own_member) {
        std::lock_guard lock(username_mutex);
        username = own_username;
    }

    Invoke(info);
}

bool RoomMember::RoomMemberImpl::HandleJoinPacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    // Parse the MAC Address from the packet
    IPv4Address new_fake_ip{};
    packet.Read(new_fake_ip);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed join packet. Ignoring it.");
        return false;
    }

    std::lock_guard lock(state_mutex);
    fake_ip = new_fake_ip;
    return true;
}

void RoomMember::RoomMemberImpl::HandleProxyPackets(const ENetEvent* event) {
    ProxyPacket proxy_packet{};
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    // Parse the ProxyPacket from the packet
    u8 local_family{};
    packet.Read(local_family);
    proxy_packet.local_endpoint.family = static_cast<Domain>(local_family);
    packet.Read(proxy_packet.local_endpoint.ip);
    packet.Read(proxy_packet.local_endpoint.portno);

    u8 remote_family{};
    packet.Read(remote_family);
    proxy_packet.remote_endpoint.family = static_cast<Domain>(remote_family);
    packet.Read(proxy_packet.remote_endpoint.ip);
    packet.Read(proxy_packet.remote_endpoint.portno);

    u8 protocol_type{};
    packet.Read(protocol_type);
    proxy_packet.protocol = static_cast<Protocol>(protocol_type);

    packet.Read(proxy_packet.broadcast);
    packet.Read(proxy_packet.data);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed proxy packet. Ignoring it.");
        return;
    }

    Invoke<ProxyPacket>(proxy_packet);
}

void RoomMember::RoomMemberImpl::HandleLdnPackets(const ENetEvent* event) {
    LDNPacket ldn_packet{};
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8)); // Ignore the message type

    u8 packet_type{};
    packet.Read(packet_type);
    ldn_packet.type = static_cast<LDNPacketType>(packet_type);

    packet.Read(ldn_packet.local_ip);
    packet.Read(ldn_packet.remote_ip);
    packet.Read(ldn_packet.broadcast);

    packet.Read(ldn_packet.data);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed LDN packet. Ignoring it.");
        return;
    }

    Invoke<LDNPacket>(ldn_packet);
}

void RoomMember::RoomMemberImpl::HandleChatPacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8));

    ChatEntry chat_entry{};
    packet.Read(chat_entry.nickname);
    packet.Read(chat_entry.username);
    packet.Read(chat_entry.message);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed chat packet. Ignoring it.");
        return;
    }
    Invoke<ChatEntry>(chat_entry);
}

void RoomMember::RoomMemberImpl::HandleStatusMessagePacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8));

    StatusMessageEntry status_message_entry{};
    u8 type{};
    packet.Read(type);
    status_message_entry.type = static_cast<StatusMessageTypes>(type);
    packet.Read(status_message_entry.nickname);
    packet.Read(status_message_entry.username);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed status message packet. Ignoring it.");
        return;
    }
    Invoke<StatusMessageEntry>(status_message_entry);
}

void RoomMember::RoomMemberImpl::HandleModBanListResponsePacket(const ENetEvent* event) {
    Packet packet;
    packet.Append(event->packet->data, event->packet->dataLength);

    // Ignore the first byte, which is the message id.
    packet.IgnoreBytes(sizeof(u8));

    Room::BanList ban_list = {};
    packet.Read(ban_list.first);
    packet.Read(ban_list.second);
    if (!packet) {
        LOG_WARNING(Network, "Received a malformed ban list response packet. Ignoring it.");
        return;
    }
    Invoke<Room::BanList>(ban_list);
}

void RoomMember::RoomMemberImpl::Disconnect() {
    {
        std::lock_guard lock(state_mutex);
        member_information.clear();
        room_information.member_slots = 0;
        room_information.name.clear();
    }
    {
        std::lock_guard send_lock(send_list_mutex);
        send_list.clear();
    }

    if (!server || !client || server->state == ENET_PEER_STATE_DISCONNECTED) {
        server = nullptr;
        return;
    }
    enet_peer_disconnect(server, 0);

    ENetEvent event;
    while (enet_host_service(client, &event, ConnectionTimeoutMs) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE:
            enet_packet_destroy(event.packet); // Ignore all incoming data
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            server = nullptr;
            return;
        case ENET_EVENT_TYPE_NONE:
        case ENET_EVENT_TYPE_CONNECT:
            break;
        }
    }
    // didn't disconnect gracefully force disconnect
    enet_peer_reset(server);
    server = nullptr;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<ProxyPacket>& RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_proxy_packet;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<LDNPacket>& RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_ldn_packet;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<RoomMember::State>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_state;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<RoomMember::Error>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_error;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<RoomInformation>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_room_information;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<ChatEntry>& RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_chat_messages;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<StatusMessageEntry>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_status_messages;
}

template <>
RoomMember::RoomMemberImpl::CallbackSet<Room::BanList>&
RoomMember::RoomMemberImpl::Callbacks::Get() {
    return callback_set_ban_list;
}

template <typename T>
void RoomMember::RoomMemberImpl::Invoke(const T& data) {
    std::lock_guard lock(callback_mutex);
    CallbackSet<T> callback_set = callbacks.Get<T>();
    for (auto const& callback : callback_set) {
        (*callback)(data);
    }
}

template <typename T>
RoomMember::CallbackHandle<T> RoomMember::RoomMemberImpl::Bind(
    std::function<void(const T&)> callback) {
    std::lock_guard lock(callback_mutex);
    CallbackHandle<T> handle;
    handle = std::make_shared<std::function<void(const T&)>>(callback);
    callbacks.Get<T>().insert(handle);
    return handle;
}

void RoomMember::RoomMemberImpl::LeaveInternal() {
    SetState(State::Idle);
    loop_thread.reset();
    {
        std::lock_guard send_lock(send_list_mutex);
        send_list.clear();
    }
    if (server != nullptr) {
        enet_peer_reset(server);
        server = nullptr;
    }
    if (client != nullptr) {
        enet_host_destroy(client);
        client = nullptr;
    }
}

// RoomMember
RoomMember::RoomMember() : room_member_impl{std::make_unique<RoomMemberImpl>()} {}

RoomMember::~RoomMember() {
    ASSERT_MSG(!IsConnected(), "RoomMember is being destroyed while connected");
    room_member_impl->join_abort_requested.store(true, std::memory_order_release);
    std::lock_guard lock(room_member_impl->lifecycle_mutex);
    room_member_impl->LeaveInternal();
}

RoomMember::State RoomMember::GetState() const {
    return room_member_impl->state;
}

RoomMember::MemberList RoomMember::GetMemberInformation() const {
    std::lock_guard lock(room_member_impl->state_mutex);
    return room_member_impl->member_information;
}

std::string RoomMember::GetNickname() const {
    std::lock_guard lock(room_member_impl->nickname_mutex);
    return room_member_impl->nickname;
}

std::string RoomMember::GetUsername() const {
    std::lock_guard lock(room_member_impl->username_mutex);
    return room_member_impl->username;
}

IPv4Address RoomMember::GetFakeIpAddress() const {
    ASSERT_MSG(IsConnected(), "Tried to get fake ip address while not connected");
    std::lock_guard lock(room_member_impl->state_mutex);
    return room_member_impl->fake_ip;
}

RoomInformation RoomMember::GetRoomInformation() const {
    std::lock_guard lock(room_member_impl->state_mutex);
    return room_member_impl->room_information;
}

void RoomMember::Join(const std::string& nick, const char* server_addr, u16 server_port,
                      u16 client_port, const IPv4Address& preferred_fake_ip,
                      const std::string& password, const std::string& token) {
    if (room_member_impl->IsLoopThread()) {
        // The network thread cannot replace itself, that would mean joining itself.
        LOG_ERROR(Network, "Join() was called from the network thread. Ignoring the request.");
        return;
    }

    std::optional<Error> error_to_emit;
    {
        std::lock_guard lock(room_member_impl->lifecycle_mutex);

        room_member_impl->LeaveInternal();
        room_member_impl->join_abort_requested.store(false, std::memory_order_release);

        if (!room_member_impl->client) {
            room_member_impl->client = enet_host_create(nullptr, 1, NumChannels, 0, 0);
            if (!room_member_impl->client) {
                LOG_CRITICAL(Network, "Could not create client");
                room_member_impl->SetState(State::Idle);
                error_to_emit = Error::UnknownError;
            }
        }

        ENetAddress address{};
        if (!error_to_emit) {
            if (!server_addr || enet_address_set_host(&address, server_addr) != 0) {
                LOG_ERROR(Network, "Failed to resolve server address '{}'",
                          server_addr ? server_addr : "");
                room_member_impl->SetState(State::Idle);
                error_to_emit = Error::CouldNotConnect;
            }
        }

        if (!error_to_emit) {
            room_member_impl->SetState(State::Joining);

            address.port = server_port;
            room_member_impl->server =
                enet_host_connect(room_member_impl->client, &address, NumChannels, 0);

            if (!room_member_impl->server) {
                room_member_impl->SetState(State::Idle);
                error_to_emit = Error::UnknownError;
            } else {
                constexpr u32 ConnectPollSliceMs = 25;
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(ConnectionTimeoutMs);
                ENetEvent event{};
                int net = 0;
                while (!room_member_impl->join_abort_requested.load(std::memory_order_acquire)) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        break;
                    }
                    const auto remaining_ms = static_cast<u32>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                            .count());
                    const u32 slice_ms = (std::min)(remaining_ms, ConnectPollSliceMs);
                    net = enet_host_service(room_member_impl->client, &event, slice_ms);
                    if (net != 0) {
                        break;
                    }
                }

                const bool aborted =
                    room_member_impl->join_abort_requested.load(std::memory_order_acquire);
                if (!aborted && net > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
                    {
                        std::lock_guard nick_lock(room_member_impl->nickname_mutex);
                        room_member_impl->nickname = nick;
                    }
                    room_member_impl->StartLoop();
                    room_member_impl->SendJoinRequest(nick, preferred_fake_ip, password, token);
                    {
                        std::lock_guard state_lock(room_member_impl->state_mutex);
                        room_member_impl->SendGameInfoPacket(room_member_impl->current_game_info);
                    }
                } else {
                    if (net > 0 && event.type == ENET_EVENT_TYPE_RECEIVE &&
                        event.packet != nullptr) {
                        enet_packet_destroy(event.packet);
                    }
                    enet_peer_reset(room_member_impl->server);
                    room_member_impl->server = nullptr;
                    room_member_impl->SetState(State::Idle);
                    if (!aborted) {
                        error_to_emit = Error::CouldNotConnect;
                    }
                }
            }
        }
    }

    if (error_to_emit) {
        room_member_impl->SetError(*error_to_emit);
    }
}

bool RoomMember::IsConnected() const {
    return room_member_impl->IsConnected();
}

void RoomMember::SendProxyPacket(const ProxyPacket& proxy_packet) {
    if (!IsConnected()) {
        return;
    }

    Packet packet;
    packet.Write(static_cast<u8>(IdProxyPacket));

    packet.Write(static_cast<u8>(proxy_packet.local_endpoint.family));
    packet.Write(proxy_packet.local_endpoint.ip);
    packet.Write(proxy_packet.local_endpoint.portno);

    packet.Write(static_cast<u8>(proxy_packet.remote_endpoint.family));
    packet.Write(proxy_packet.remote_endpoint.ip);
    packet.Write(proxy_packet.remote_endpoint.portno);

    packet.Write(static_cast<u8>(proxy_packet.protocol));
    packet.Write(proxy_packet.broadcast);
    packet.Write(proxy_packet.data);

    room_member_impl->Send(std::move(packet));
}

void RoomMember::SendLdnPacket(const LDNPacket& ldn_packet) {
    if (!IsConnected()) {
        return;
    }

    Packet packet;
    packet.Write(static_cast<u8>(IdLdnPacket));

    packet.Write(static_cast<u8>(ldn_packet.type));

    packet.Write(ldn_packet.local_ip);
    packet.Write(ldn_packet.remote_ip);
    packet.Write(ldn_packet.broadcast);

    packet.Write(ldn_packet.data);

    room_member_impl->Send(std::move(packet));
}

void RoomMember::SendChatMessage(const std::string& message) {
    if (!IsConnected()) {
        return;
    }

    Packet packet;
    packet.Write(static_cast<u8>(IdChatMessage));
    packet.Write(message);
    room_member_impl->Send(std::move(packet));
}

void RoomMember::SendGameInfo(const GameInfo& game_info) {
    std::lock_guard lock(room_member_impl->state_mutex);
    room_member_impl->current_game_info = game_info;
    if (!IsConnected()) {
        return;
    }

    room_member_impl->SendGameInfoPacket(room_member_impl->current_game_info);
}

void RoomMember::SendModerationRequest(RoomMessageTypes type, const std::string& nickname) {
    ASSERT_MSG(type == IdModKick || type == IdModBan || type == IdModUnban,
               "type is not a moderation request");
    if (!IsConnected())
        return;

    Packet packet;
    packet.Write(static_cast<u8>(type));
    packet.Write(nickname);
    room_member_impl->Send(std::move(packet));
}

void RoomMember::RequestBanList() {
    if (!IsConnected())
        return;

    Packet packet;
    packet.Write(static_cast<u8>(IdModGetBanList));
    room_member_impl->Send(std::move(packet));
}

RoomMember::CallbackHandle<RoomMember::State> RoomMember::BindOnStateChanged(
    std::function<void(const RoomMember::State&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<RoomMember::Error> RoomMember::BindOnError(
    std::function<void(const RoomMember::Error&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<ProxyPacket> RoomMember::BindOnProxyPacketReceived(
    std::function<void(const ProxyPacket&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<LDNPacket> RoomMember::BindOnLdnPacketReceived(
    std::function<void(const LDNPacket&)> callback) {
    return room_member_impl->Bind(std::move(callback));
}

RoomMember::CallbackHandle<RoomInformation> RoomMember::BindOnRoomInformationChanged(
    std::function<void(const RoomInformation&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<ChatEntry> RoomMember::BindOnChatMessageReceived(
    std::function<void(const ChatEntry&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<StatusMessageEntry> RoomMember::BindOnStatusMessageReceived(
    std::function<void(const StatusMessageEntry&)> callback) {
    return room_member_impl->Bind(callback);
}

RoomMember::CallbackHandle<Room::BanList> RoomMember::BindOnBanListReceived(
    std::function<void(const Room::BanList&)> callback) {
    return room_member_impl->Bind(callback);
}

template <typename T>
void RoomMember::Unbind(CallbackHandle<T> handle) {
    std::lock_guard lock(room_member_impl->callback_mutex);
    room_member_impl->callbacks.Get<T>().erase(handle);
}

void RoomMember::Leave() {
    if (room_member_impl->IsLoopThread()) {
        // Leave() was called from the network thread itself, for example from a callback. The
        // state must not be changed here: the state change invokes the state callbacks, while
        // the callback mutex is already held by this thread inside the callback that called
        // Leave(). The leave is requested instead and the network thread performs the state
        // change once it is back in its loop.
        room_member_impl->leave_requested = true;
        LOG_INFO(Network,
                 "Leave() was called from the network thread. The network thread leaves the room "
                 "itself.");
        return;
    }
    room_member_impl->join_abort_requested.store(true, std::memory_order_release);
    std::lock_guard lock(room_member_impl->lifecycle_mutex);
    room_member_impl->LeaveInternal();
}

template void RoomMember::Unbind(CallbackHandle<ProxyPacket>);
template void RoomMember::Unbind(CallbackHandle<LDNPacket>);
template void RoomMember::Unbind(CallbackHandle<RoomMember::State>);
template void RoomMember::Unbind(CallbackHandle<RoomMember::Error>);
template void RoomMember::Unbind(CallbackHandle<RoomInformation>);
template void RoomMember::Unbind(CallbackHandle<ChatEntry>);
template void RoomMember::Unbind(CallbackHandle<StatusMessageEntry>);
template void RoomMember::Unbind(CallbackHandle<Room::BanList>);

} // namespace Network
