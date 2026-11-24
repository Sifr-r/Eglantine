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
        requests.push_back({id, Command::Pause});
    } else if (command == "resume") {
        requests.push_back({id, Command::Resume});
    } else if (command == "stop") {
        requests.push_back({id, Command::Stop});
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
    Write(reply.dump());
}

void Session::Error(std::string_view message) {
    Write(Json{{"version", ProtocolVersion}, {"event", "error"}, {"message", message}}
              .dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Session::SendEvent(std::string_view event) {
    Write(Json{{"version", ProtocolVersion}, {"event", event}}.dump());
}

void Session::Game(std::string_view path, std::string_view title, std::string_view format,
                   std::optional<std::uint64_t> program_id, std::string_view icon_hex) {
    Json game{{"version", ProtocolVersion}, {"event", "game"}, {"path", path},
              {"title", title},             {"format", format}};
    if (program_id) {
        game["program_id"] = fmt::format("{:016x}", *program_id);
    }
    if (!icon_hex.empty()) {
        game["icon_hex"] = icon_hex;
    }
    Write(game.dump(-1, ' ', false, Json::error_handler_t::replace));
}

void Session::LibraryDone(std::size_t scan_errors) {
    Write(Json{{"version", ProtocolVersion}, {"event", "library_done"}, {"scan_errors", scan_errors}}
              .dump());
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
