// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Gpui {

enum class SessionState { Starting, Running, Paused, Stopping, Stopped };
enum class Command { Pause, Resume, Stop };

struct Request {
    std::uint64_t id;
    Command command;
};

// Owns the private inherited pipes and translates bounded JSON messages on the SDL main thread.
class Session {
public:
    static std::unique_ptr<Session> Open();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Poll(std::vector<Request>& requests);
    void SetState(SessionState state);
    [[nodiscard]] SessionState GetState() const;
    void Reply(std::uint64_t id, std::string_view error = {});
    void Error(std::string_view message);
    void SendEvent(std::string_view event);
    void Game(std::string_view path, std::string_view title, std::string_view format,
              std::optional<std::uint64_t> program_id, std::string_view icon_hex);
    void LibraryDone(std::size_t scan_errors);
    [[nodiscard]] bool IsConnected() const;

private:
    struct Pipes;
    explicit Session(std::unique_ptr<Pipes> pipes);
    void Write(std::string message);
    void Parse(std::string_view line, std::vector<Request>& requests);

    std::unique_ptr<Pipes> pipes;
    std::string pending;
    SessionState state{SessionState::Starting};
    bool connected{true};
};

} // namespace Gpui
