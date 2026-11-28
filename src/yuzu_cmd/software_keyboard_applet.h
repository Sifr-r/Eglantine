// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <SDL3/SDL_events.h>

#include "core/frontend/applets/software_keyboard.h"
#include "yuzu_cmd/gpui_session.h"

class EmuWindow_SDL3;
struct SDL_Window;

namespace YuzuCmd {

class SoftwareKeyboardController {
public:
    SoftwareKeyboardController() = default;
    ~SoftwareKeyboardController();

    SoftwareKeyboardController(const SoftwareKeyboardController&) = delete;
    SoftwareKeyboardController& operator=(const SoftwareKeyboardController&) = delete;

    // Called from Core::Frontend::SoftwareKeyboardApplet on guest/service threads.
    void InitializeNormal(Core::Frontend::KeyboardInitializeParameters params,
                          Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback callback);
    void ShowNormal();
    void ShowTextCheck(Service::AM::Frontend::SwkbdTextCheckResult result,
                       std::u16string message);
    void ExitNormal();
    void CloseNormal();

    // Called on the SDL main thread.
    bool Submit(std::uint64_t request_id, std::string_view utf8_text, bool confirmed,
                std::string& error);
    bool Cancel(std::uint64_t request_id, std::string& error);

    void DrainGpuiEvents(Gpui::Session& session, EmuWindow_SDL3* emu_window);
    void DrainSdlEvents(SDL_Window* window);
    bool HandleSdlEvent(const SDL_Event& event, SDL_Window* window);
    [[nodiscard]] bool IsSdlActive() const;

private:
    struct ShowEvent {
        Gpui::SwkbdShowParams params;
    };
    struct TextCheckEvent {
        std::uint64_t request_id{};
        Gpui::SwkbdTextCheckKind kind{Gpui::SwkbdTextCheckKind::Failure};
        std::string message;
    };
    struct CloseEvent {
        std::uint64_t request_id{};
    };
    using UiEvent = std::variant<ShowEvent, TextCheckEvent, CloseEvent>;

    std::vector<UiEvent> TakePendingEvents();
    void UpdateSdlWindowTitle(SDL_Window* window) const;
    void AppendSdlUtf8(std::string_view text);
    void PopSdlLastCodepoint();

    mutable std::mutex mutex;
    std::uint64_t next_request_id{0};
    std::uint64_t active_request_id{0};
    bool normal_active{false};
    bool normal_completed{false};
    bool awaiting_text_check{false};
    bool awaiting_confirm{false};
    Core::Frontend::KeyboardInitializeParameters parameters{};
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback submit_normal_callback;
    std::vector<UiEvent> pending_events;

    // Main-thread-only state for standalone SDL fallback and GPUI embedded visibility.
    bool gpui_hid_embedded{false};
    bool sdl_open{false};
    std::uint64_t sdl_request_id{0};
    std::string sdl_header;
    std::string sdl_draft;
    std::uint32_t sdl_min_length{0};
    std::uint32_t sdl_max_length{500};
    bool sdl_password{false};
    bool sdl_multiline{false};
    bool sdl_disable_cancel{false};
    std::optional<std::pair<Gpui::SwkbdTextCheckKind, std::string>> sdl_text_check;
    std::string sdl_status_note;
};

class HostSoftwareKeyboardApplet final : public Core::Frontend::SoftwareKeyboardApplet {
public:
    explicit HostSoftwareKeyboardApplet(std::shared_ptr<SoftwareKeyboardController> controller_);
    ~HostSoftwareKeyboardApplet() override;

    void Close() const override;

    void InitializeKeyboard(bool is_inline,
                            Core::Frontend::KeyboardInitializeParameters initialize_parameters,
                            SubmitNormalCallback submit_normal_callback_,
                            SubmitInlineCallback submit_inline_callback_) override;

    void ShowNormalKeyboard() const override;

    void ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
                             std::u16string text_check_message) const override;

    void ShowInlineKeyboard(Core::Frontend::InlineAppearParameters appear_parameters) const override;

    void HideInlineKeyboard() const override;

    void InlineTextChanged(Core::Frontend::InlineTextParameters text_parameters) const override;

    void ExitKeyboard() const override;

private:
    std::shared_ptr<SoftwareKeyboardController> controller;
    mutable Core::Frontend::DefaultSoftwareKeyboardApplet inline_fallback;
    bool is_inline_mode{false};
};

} // namespace YuzuCmd
