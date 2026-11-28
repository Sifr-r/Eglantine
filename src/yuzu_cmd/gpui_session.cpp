// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/gpui_session.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/poll.h>
#include <unistd.h>
#endif

namespace Gpui {
namespace {

using Json = nlohmann::json;
constexpr unsigned ProtocolVersion = 1;
constexpr std::size_t MaxLine = 4096;

std::string_view StateName(SessionState state) {
    switch (state) {
    case SessionState::Starting:
        return "starting";
    case SessionState::Running:
        return "running";
    case SessionState::Paused:
        return "paused";
    case SessionState::Stopping:
        return "stopping";
    case SessionState::Stopped:
        return "stopped";
    }
    return "stopped";
}

} // namespace

struct Session::Pipes {
#ifdef _WIN32
    HANDLE input{};
    HANDLE output{};

    ~Pipes() {
        if (output) {
            CloseHandle(output);
        }
    }

    bool Open() {
        input = GetStdHandle(STD_INPUT_HANDLE);
        const HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        const HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
        const HANDLE process = GetCurrentProcess();
        if (GetFileType(input) != FILE_TYPE_PIPE || GetFileType(stdout_handle) != FILE_TYPE_PIPE ||
            !DuplicateHandle(process, stdout_handle, process, &output, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            return false;
        }
        // Keep the saved pipe exclusive to protocol messages, including for Win32 writers.
        return SetStdHandle(STD_OUTPUT_HANDLE, stderr_handle) &&
               _dup2(_fileno(stderr), _fileno(stdout)) == 0;
    }

    bool Read(std::array<char, MaxLine>& buffer, std::size_t& size) const {
        DWORD available{};
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }
        if (available == 0) {
            return true;
        }
        DWORD received{};
        if (!ReadFile(input, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())),
                      &received, nullptr)) {
            return false;
        }
        size = received;
        return received != 0;
    }

    bool Write(std::string_view message) const {
        DWORD written{};
        return WriteFile(output, message.data(), static_cast<DWORD>(message.size()), &written,
                         nullptr) && written == message.size();
    }
#else
    int output{-1};

    ~Pipes() {
        if (output >= 0) {
            close(output);
        }
    }

    bool Open() {
        if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) {
            return false;
        }
        output = dup(STDOUT_FILENO);
        if (output < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            return false;
        }
        // A vanished parent must trigger orderly shutdown instead of SIGPIPE termination.
        return signal(SIGPIPE, SIG_IGN) != SIG_ERR;
    }

    bool Read(std::array<char, MaxLine>& buffer, std::size_t& size) const {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = poll(&input, 1, 0);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            return true;
        }
        if (ready < 0 || !(input.revents & (POLLIN | POLLHUP))) {
            return false;
        }
        const auto received = read(STDIN_FILENO, buffer.data(), buffer.size());
        if (received < 0 && errno == EINTR) {
            return true;
        }
        if (received <= 0) {
            return false;
        }
        size = static_cast<std::size_t>(received);
        return true;
    }

    bool Write(std::string_view message) const {
        while (!message.empty()) {
            const auto written = write(output, message.data(), message.size());
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return false;
            }
            message.remove_prefix(static_cast<std::size_t>(written));
        }
        return true;
    }
#endif
};

std::unique_ptr<Session> Session::Open() {
    auto pipes = std::make_unique<Pipes>();
    if (!pipes->Open()) {
        return {};
    }
    auto session = std::unique_ptr<Session>{new Session(std::move(pipes))};
    session->SendEvent("ready");
    return session;
}

Session::Session(std::unique_ptr<Pipes> pipes_) : pipes(std::move(pipes_)) {}
Session::~Session() = default;

bool Session::Poll(std::vector<Request>& requests) {
    std::array<char, MaxLine> buffer;
    std::size_t size{};
    if (!connected || !pipes->Read(buffer, size)) {
        return false;
    }
    for (const char byte : std::string_view{buffer.data(), size}) {
        if (byte == '\n') {
            Parse(pending, requests);
            pending.clear();
        } else if (pending.size() == MaxLine) {
            Error("Session request exceeds the size limit");
            return false;
        } else {
            pending.push_back(byte);
        }
    }
    return connected;
}

void Session::Parse(std::string_view line, std::vector<Request>& requests) {
    const auto message = Json::parse(line, nullptr, false);
    if (!message.is_object() || !message.contains("id") || !message["id"].is_number_unsigned()) {
        Error("Invalid session request");
        return;
    }
    const auto id = message["id"].get<std::uint64_t>();
    if (!message.contains("version") || !message["version"].is_number_unsigned() ||
        message["version"] != ProtocolVersion ||
        !message.contains("command") || !message["command"].is_string()) {
        Reply(id, "Invalid or unsupported session request");
        return;
    }
    const auto& command = message["command"].get_ref<const std::string&>();
    if (command == "pause") {
        requests.push_back({.id = id, .command = Command::Pause});
    } else if (command == "resume") {
        requests.push_back({.id = id, .command = Command::Resume});
    } else if (command == "stop") {
        requests.push_back({.id = id, .command = Command::Stop});
    } else if (command == "save_state" || command == "load_state") {
        std::uint8_t slot = 1;
        if (message.contains("slot")) {
            if (!message["slot"].is_number_unsigned()) {
                Reply(id, "Invalid save state slot");
                return;
            }
            const auto raw_slot = message["slot"].get<std::uint64_t>();
            if (raw_slot < 1 || raw_slot > 9) {
                Reply(id, "Save state slot must be between 1 and 9");
                return;
            }
            slot = static_cast<std::uint8_t>(raw_slot);
        }
        requests.push_back({
            .id = id,
            .command = command == "save_state" ? Command::SaveState : Command::LoadState,
            .slot = slot,
        });
    } else if (command == "set_fast_forward") {
        if (!message.contains("enabled") || !message["enabled"].is_boolean()) {
            Reply(id, "Missing boolean 'enabled' field");
            return;
        }
        requests.push_back({
            .id = id,
            .command = Command::SetFastForward,
            .enabled = message["enabled"].get<bool>(),
        });
    } else if (command == "rewind_step") {
        requests.push_back({.id = id, .command = Command::RewindStep});
    } else if (command == "set_video_options") {
        bool color_correction = false;
        bool interframe_blending = false;
        if (message.contains("color_correction")) {
            if (!message["color_correction"].is_boolean()) {
                Reply(id, "Invalid boolean 'color_correction' field");
                return;
            }
            color_correction = message["color_correction"].get<bool>();
        }
        if (message.contains("interframe_blending")) {
            if (!message["interframe_blending"].is_boolean()) {
                Reply(id, "Invalid boolean 'interframe_blending' field");
                return;
            }
            interframe_blending = message["interframe_blending"].get<bool>();
        }
        requests.push_back({
            .id = id,
            .command = Command::SetVideoOptions,
            .color_correction = color_correction,
            .interframe_blending = interframe_blending,
        });
    } else if (command == "set_cheats") {
        if (!message.contains("codes") || !message["codes"].is_array()) {
            Reply(id, "Missing 'codes' array");
            return;
        }
        std::vector<std::string> codes;
        for (const auto& item : message["codes"]) {
            if (!item.is_string()) {
                Reply(id, "Cheat entries must be strings");
                return;
            }
            codes.push_back(item.get<std::string>());
        }
        requests.push_back({
            .id = id,
            .command = Command::SetCheats,
            .codes = std::move(codes),
        });
    } else if (command == "reset") {
        requests.push_back({.id = id, .command = Command::Reset});
    } else if (command == "set_viewport") {
        if (!message.contains("x") || !message["x"].is_number_integer() ||
            !message.contains("y") || !message["y"].is_number_integer() ||
            !message.contains("width") || !message["width"].is_number_integer() ||
            !message.contains("height") || !message["height"].is_number_integer()) {
            Reply(id, "Invalid viewport rectangle");
            return;
        }
        const auto x = message["x"].get<std::int64_t>();
        const auto y = message["y"].get<std::int64_t>();
        const auto width = message["width"].get<std::int64_t>();
        const auto height = message["height"].get<std::int64_t>();
        if (x < -16384 || x > 16384 || y < -16384 || y > 16384 || width <= 0 || height <= 0 ||
            width > 16384 || height > 16384) {
            Reply(id, "Invalid viewport rectangle");
            return;
        }
        requests.push_back({
            .id = id,
            .command = Command::SetViewport,
            .viewport_x = static_cast<int>(x),
            .viewport_y = static_cast<int>(y),
            .viewport_width = static_cast<int>(width),
            .viewport_height = static_cast<int>(height),
        });
    } else if (command == "swkbd_submit") {
        if (!message.contains("request_id") || !message["request_id"].is_number_unsigned() ||
            !message.contains("text") || !message["text"].is_string()) {
            Reply(id, "Invalid software keyboard submit payload");
            return;
        }
        bool confirmed = false;
        if (message.contains("confirmed")) {
            if (!message["confirmed"].is_boolean()) {
                Reply(id, "Invalid boolean 'confirmed' field");
                return;
            }
            confirmed = message["confirmed"].get<bool>();
        }
        requests.push_back({
            .id = id,
            .command = Command::SwkbdSubmit,
            .swkbd_request_id = message["request_id"].get<std::uint64_t>(),
            .swkbd_text = message["text"].get<std::string>(),
            .swkbd_confirmed = confirmed,
        });
    } else if (command == "swkbd_cancel") {
        if (!message.contains("request_id") || !message["request_id"].is_number_unsigned()) {
            Reply(id, "Invalid software keyboard cancel payload");
            return;
        }
        requests.push_back({
            .id = id,
            .command = Command::SwkbdCancel,
            .swkbd_request_id = message["request_id"].get<std::uint64_t>(),
        });
    } else {
        Reply(id, "Unknown session command");
    }
}

void Session::SetState(SessionState next) {
    if (state != next) {
        state = next;
        Write(Json{{"version", ProtocolVersion}, {"event", "state"}, {"state", StateName(state)}}
                  .dump());
    }
}

SessionState Session::GetState() const {
    return state;
}

void Session::Reply(std::uint64_t id, std::string_view error) {
    Json reply{{"version", ProtocolVersion}, {"event", "reply"},       {"id", id},
               {"ok", error.empty()},       {"state", StateName(state)}};
    if (!error.empty()) {
        reply["message"] = error;
    }
    Write(reply.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Session::Error(std::string_view message) {
    Write(Json{{"version", ProtocolVersion}, {"event", "error"}, {"message", message}}
              .dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Session::SendEvent(std::string_view event) {
    Write(Json{{"version", ProtocolVersion}, {"event", event}}.dump());
}

void Session::Game(std::string_view path, std::string_view title, std::string_view format,
                   std::optional<std::uint64_t> program_id, std::string_view icon_hex,
                   std::string_view system, const GameInfo& info) {
    Json game{{"version", ProtocolVersion}, {"event", "game"}, {"path", path},
              {"title", title},             {"format", format}};
    if (program_id) {
        game["program_id"] = fmt::format("{:016x}", *program_id);
    }
    if (!icon_hex.empty()) {
        game["icon_hex"] = icon_hex;
    }
    if (!system.empty()) {
        game["system"] = system;
    }
    if (!info.publisher.empty()) {
        game["publisher"] = info.publisher;
    }
    if (!info.version.empty()) {
        game["game_version"] = info.version;
    }
    if (!info.region.empty()) {
        game["region"] = info.region;
    }
    if (!info.product_code.empty()) {
        game["product_code"] = info.product_code;
    }
    if (!info.internal_title.empty()) {
        game["internal_title"] = info.internal_title;
    }
    if (!info.hardware.empty()) {
        game["hardware"] = info.hardware;
    }
    if (info.file_size > 0) {
        game["file_size"] = info.file_size;
    }
    if (!info.languages.empty()) {
        game["languages"] = info.languages;
    }
    Write(game.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Session::LibraryDone(std::size_t scan_errors) {
    Write(Json{{"version", ProtocolVersion}, {"event", "library_done"}, {"scan_errors", scan_errors}}
              .dump());
}

void Session::ShowSoftwareKeyboard(const SwkbdShowParams& params) {
    Json message{
        {"version", ProtocolVersion},
        {"event", "swkbd_show"},
        {"request_id", params.request_id},
        {"header_text", params.header_text},
        {"sub_text", params.sub_text},
        {"guide_text", params.guide_text},
        {"ok_text", params.ok_text},
        {"initial_text", params.initial_text},
        {"min_length", params.min_length},
        {"max_length", params.max_length},
        {"password", params.password},
        {"multiline", params.multiline},
        {"disable_cancel", params.disable_cancel},
    };
    Write(message.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Session::ShowSoftwareKeyboardTextCheck(std::uint64_t request_id, SwkbdTextCheckKind kind,
                                            std::string_view message) {
    Json payload{
        {"version", ProtocolVersion},
        {"event", "swkbd_text_check"},
        {"request_id", request_id},
        {"result", kind == SwkbdTextCheckKind::Confirm ? "confirm" : "failure"},
        {"message", message},
    };
    Write(payload.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Session::CloseSoftwareKeyboard(std::uint64_t request_id) {
    Write(Json{{"version", ProtocolVersion}, {"event", "swkbd_close"}, {"request_id", request_id}}
              .dump());
}

void Session::ControllerState(std::optional<std::string_view> name,
                              std::optional<std::string_view> ble_handoff) {
    Json payload{{"version", ProtocolVersion}, {"event", "controller"}};
    if (name) {
        payload["name"] = *name;
    } else {
        payload["name"] = nullptr;
    }
    if (ble_handoff) {
        payload["ble_handoff"] = *ble_handoff;
    } else {
        payload["ble_handoff"] = nullptr;
    }
    Write(payload.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Session::ControllerInput(std::uint32_t buttons, float left_x, float left_y) {
    Json payload{
        {"version", ProtocolVersion},
        {"event", "controller_input"},
        {"buttons", buttons},
        {"left_x", left_x},
        {"left_y", left_y},
    };
    Write(payload.dump());
}

bool Session::IsConnected() const {
    return connected;
}

void Session::Write(std::string message) {
    if (connected) {
        message.push_back('\n');
        connected = pipes->Write(message);
    }
}

} // namespace Gpui
