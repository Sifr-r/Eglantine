// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Gpui {

struct GameInfo {
    std::string_view publisher;
    std::string_view version;
    std::string_view region;
    std::string_view product_code;
    std::string_view internal_title;
    std::string_view hardware;
    std::uint64_t file_size{};
    std::span<const std::string> languages;
};

enum class SessionState { Starting, Running, Paused, Stopping, Stopped };
enum class Command {
    Pause,
    Resume,
    Stop,
    SaveState,
    LoadState,
    SetFastForward,
    RewindStep,
    SetVideoOptions,
    SetCheats,
    Reset,
    SetViewport,
    SwkbdSubmit,
    SwkbdCancel,
};

enum class SwkbdTextCheckKind {
    Failure,
    Confirm,
};

struct SwkbdShowParams {
    std::uint64_t request_id{};
    std::string header_text;
    std::string sub_text;
    std::string guide_text;
    std::string ok_text;
    std::string initial_text;
    std::uint32_t min_length{};
    std::uint32_t max_length{};
    bool password{};
    bool multiline{};
    bool disable_cancel{};
};

struct Request {
    std::uint64_t id;
    Command command;
    std::uint8_t slot{1};
    bool enabled{};
    bool color_correction{};
    bool interframe_blending{};
    int viewport_x{};
    int viewport_y{};
    int viewport_width{};
    int viewport_height{};
    std::vector<std::string> codes;
    std::uint64_t swkbd_request_id{};
    std::string swkbd_text;
    bool swkbd_confirmed{};
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
              std::optional<std::uint64_t> program_id, std::string_view icon_hex,
              std::string_view system, const GameInfo& info = {});
    void LibraryDone(std::size_t scan_errors);
    void ShowSoftwareKeyboard(const SwkbdShowParams& params);
    void ShowSoftwareKeyboardTextCheck(std::uint64_t request_id, SwkbdTextCheckKind kind,
                                       std::string_view message);
    void CloseSoftwareKeyboard(std::uint64_t request_id);
    void ControllerState(std::optional<std::string_view> name);
    void ControllerInput(std::uint32_t buttons, float left_x, float left_y);
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
