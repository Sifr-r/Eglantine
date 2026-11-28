// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/software_keyboard_applet.h"

#include <algorithm>
#include <utility>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include "common/logging.h"
#include "common/string_util.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3.h"

namespace YuzuCmd {
namespace {

constexpr std::uint32_t kDefaultMaxTextLength = 500;

std::size_t Utf16CodeUnitCount(std::string_view utf8) {
    return Common::UTF8ToUTF16(utf8).size();
}

} // namespace

SoftwareKeyboardController::~SoftwareKeyboardController() {
    CloseNormal();
}

void SoftwareKeyboardController::InitializeNormal(
    Core::Frontend::KeyboardInitializeParameters params,
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback callback) {
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback previous_callback;
    {
        std::scoped_lock lk{mutex};
        if (normal_active && !normal_completed) {
            const auto req_id = active_request_id;
            normal_active = false;
            normal_completed = true;
            awaiting_text_check = false;
            awaiting_confirm = false;
            previous_callback = std::move(submit_normal_callback);
            pending_events.push_back(CloseEvent{.request_id = req_id});
        }
        parameters = std::move(params);
        submit_normal_callback = std::move(callback);
        normal_active = false;
        normal_completed = false;
        awaiting_text_check = false;
        awaiting_confirm = false;
    }
    if (previous_callback) {
        previous_callback(Service::AM::Frontend::SwkbdResult::Cancel, u"", true);
    }
}

void SoftwareKeyboardController::ShowNormal() {
    std::scoped_lock lk{mutex};
    const std::uint32_t max_len =
        parameters.max_text_length > 0 && parameters.max_text_length <= kDefaultMaxTextLength
            ? parameters.max_text_length
            : kDefaultMaxTextLength;
    const std::uint32_t min_len = std::min(parameters.min_text_length, max_len);

    active_request_id = ++next_request_id;
    normal_active = true;
    normal_completed = false;
    awaiting_text_check = false;
    awaiting_confirm = false;

    Gpui::SwkbdShowParams show{
        .request_id = active_request_id,
        .header_text = Common::UTF16ToUTF8(parameters.header_text),
        .sub_text = Common::UTF16ToUTF8(parameters.sub_text),
        .guide_text = Common::UTF16ToUTF8(parameters.guide_text),
        .ok_text = Common::UTF16ToUTF8(parameters.ok_text),
        .initial_text = Common::UTF16ToUTF8(parameters.initial_text),
        .min_length = min_len,
        .max_length = max_len,
        .password = parameters.password_mode == Service::AM::Frontend::SwkbdPasswordMode::Enabled,
        .multiline = parameters.enable_return_button,
        .disable_cancel = parameters.disable_cancel_button,
    };
    pending_events.push_back(ShowEvent{.params = std::move(show)});
}

void SoftwareKeyboardController::ShowTextCheck(
    Service::AM::Frontend::SwkbdTextCheckResult result, std::u16string message) {
    std::scoped_lock lk{mutex};
    if (!normal_active || normal_completed) {
        return;
    }
    awaiting_text_check = false;
    if (result == Service::AM::Frontend::SwkbdTextCheckResult::Failure ||
        result == Service::AM::Frontend::SwkbdTextCheckResult::Confirm) {
        awaiting_confirm = (result == Service::AM::Frontend::SwkbdTextCheckResult::Confirm);
        pending_events.push_back(TextCheckEvent{
            .request_id = active_request_id,
            .kind = awaiting_confirm ? Gpui::SwkbdTextCheckKind::Confirm
                                     : Gpui::SwkbdTextCheckKind::Failure,
            .message = Common::UTF16ToUTF8(message),
        });
    }
}

void SoftwareKeyboardController::ExitNormal() {
    std::scoped_lock lk{mutex};
    if (!normal_active && normal_completed) {
        return;
    }
    const auto req_id = active_request_id;
    const bool was_active = normal_active;
    normal_active = false;
    normal_completed = true;
    awaiting_text_check = false;
    awaiting_confirm = false;
    submit_normal_callback = {};
    if (was_active) {
        pending_events.push_back(CloseEvent{.request_id = req_id});
    }
}

void SoftwareKeyboardController::CloseNormal() {
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback callback;
    {
        std::scoped_lock lk{mutex};
        if (!normal_active || normal_completed) {
            return;
        }
        const auto req_id = active_request_id;
        normal_active = false;
        normal_completed = true;
        awaiting_text_check = false;
        awaiting_confirm = false;
        callback = std::move(submit_normal_callback);
        pending_events.push_back(CloseEvent{.request_id = req_id});
    }
    if (callback) {
        callback(Service::AM::Frontend::SwkbdResult::Cancel, u"", true);
    }
}

bool SoftwareKeyboardController::Submit(std::uint64_t request_id, std::string_view utf8_text,
                                        bool confirmed, std::string& error) {
    std::u16string utf16_text = Common::UTF8ToUTF16(utf8_text);
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback callback;
    bool effective_confirmed = false;
    {
        std::scoped_lock lk{mutex};
        if (!normal_active || normal_completed || request_id != active_request_id) {
            error = "No matching active software keyboard request";
            return false;
        }
        if (awaiting_text_check) {
            error = "Software keyboard input is awaiting validation";
            return false;
        }
        const std::uint32_t max_len =
            parameters.max_text_length > 0 && parameters.max_text_length <= kDefaultMaxTextLength
                ? parameters.max_text_length
                : kDefaultMaxTextLength;
        const std::uint32_t min_len = std::min(parameters.min_text_length, max_len);
        if (utf16_text.size() < min_len) {
            error = fmt::format("Input must be at least {} characters", min_len);
            return false;
        }
        if (utf16_text.size() > max_len) {
            error = fmt::format("Input must be at most {} characters", max_len);
            return false;
        }
        if (utf8_text.size() >= Service::AM::Frontend::STRING_BUFFER_SIZE) {
            error = "Input exceeds the software keyboard buffer limit";
            return false;
        }

        effective_confirmed = confirmed && awaiting_confirm;
        if (effective_confirmed) {
            normal_active = false;
            normal_completed = true;
            awaiting_text_check = false;
            awaiting_confirm = false;
            callback = std::move(submit_normal_callback);
            pending_events.push_back(CloseEvent{.request_id = request_id});
        } else {
            awaiting_text_check = true;
            awaiting_confirm = false;
            callback = submit_normal_callback;
        }
    }
    if (callback) {
        callback(Service::AM::Frontend::SwkbdResult::Ok, std::move(utf16_text),
                 effective_confirmed);
    }
    return true;
}

bool SoftwareKeyboardController::Cancel(std::uint64_t request_id, std::string& error) {
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback callback;
    {
        std::scoped_lock lk{mutex};
        if (!normal_active || normal_completed || request_id != active_request_id) {
            error = "No matching active software keyboard request";
            return false;
        }
        if (parameters.disable_cancel_button) {
            error = "Cancel is disabled for this software keyboard request";
            return false;
        }
        normal_active = false;
        normal_completed = true;
        awaiting_text_check = false;
        awaiting_confirm = false;
        callback = std::move(submit_normal_callback);
        pending_events.push_back(CloseEvent{.request_id = request_id});
    }
    if (callback) {
        callback(Service::AM::Frontend::SwkbdResult::Cancel, u"", true);
    }
    return true;
}

std::vector<SoftwareKeyboardController::UiEvent> SoftwareKeyboardController::TakePendingEvents() {
    std::vector<UiEvent> events;
    std::scoped_lock lk{mutex};
    events.swap(pending_events);
    return events;
}

void SoftwareKeyboardController::DrainGpuiEvents(Gpui::Session& session,
                                                 EmuWindow_SDL3* emu_window) {
    const auto events = TakePendingEvents();
    for (const auto& event : events) {
        if (const auto* show = std::get_if<ShowEvent>(&event)) {
            if (emu_window && emu_window->IsEmbedded() && !gpui_hid_embedded) {
                if (emu_window->SetSessionVisible(false)) {
                    gpui_hid_embedded = true;
                }
            }
            session.ShowSoftwareKeyboard(show->params);
        } else if (const auto* check = std::get_if<TextCheckEvent>(&event)) {
            session.ShowSoftwareKeyboardTextCheck(check->request_id, check->kind, check->message);
        } else if (const auto* close = std::get_if<CloseEvent>(&event)) {
            session.CloseSoftwareKeyboard(close->request_id);
            if (gpui_hid_embedded && emu_window &&
                session.GetState() == Gpui::SessionState::Running) {
                emu_window->SetSessionVisible(true);
                gpui_hid_embedded = false;
            }
        }
    }
}

void SoftwareKeyboardController::DrainSdlEvents(SDL_Window* window) {
    const auto events = TakePendingEvents();
    for (const auto& event : events) {
        if (const auto* show = std::get_if<ShowEvent>(&event)) {
            sdl_open = true;
            sdl_request_id = show->params.request_id;
            sdl_header = show->params.header_text.empty() ? "Software Keyboard"
                                                          : show->params.header_text;
            sdl_draft = show->params.initial_text;
            sdl_min_length = show->params.min_length;
            sdl_max_length = show->params.max_length;
            sdl_password = show->params.password;
            sdl_multiline = show->params.multiline;
            sdl_disable_cancel = show->params.disable_cancel;
            sdl_text_check.reset();
            sdl_status_note.clear();
            if (window) {
                SDL_StartTextInput(window);
            }
            LOG_INFO(Frontend, "Software keyboard opened (min={}, max={}): {}", sdl_min_length,
                     sdl_max_length, sdl_header);
            UpdateSdlWindowTitle(window);
        } else if (const auto* check = std::get_if<TextCheckEvent>(&event)) {
            if (sdl_open && sdl_request_id == check->request_id) {
                sdl_text_check = std::make_pair(check->kind, check->message);
                sdl_status_note.clear();
                LOG_INFO(Frontend, "Software keyboard text check ({}): {}",
                         check->kind == Gpui::SwkbdTextCheckKind::Confirm ? "confirm" : "failure",
                         check->message);
                UpdateSdlWindowTitle(window);
            }
        } else if (const auto* close = std::get_if<CloseEvent>(&event)) {
            if (sdl_open && sdl_request_id == close->request_id) {
                sdl_open = false;
                sdl_text_check.reset();
                sdl_status_note.clear();
                if (window) {
                    SDL_StopTextInput(window);
                }
            }
        }
    }
}

bool SoftwareKeyboardController::IsSdlActive() const {
    return sdl_open;
}

void SoftwareKeyboardController::UpdateSdlWindowTitle(SDL_Window* window) const {
    if (!window || !sdl_open) {
        return;
    }
    std::string display;
    if (sdl_password) {
        display.assign(Utf16CodeUnitCount(sdl_draft), '*');
    } else {
        display = sdl_draft;
        std::ranges::replace(display, '\n', ' ');
    }
    std::string title;
    if (sdl_text_check) {
        const char* hint = sdl_text_check->first == Gpui::SwkbdTextCheckKind::Confirm
                               ? "Enter=Confirm, Esc=Edit"
                               : "Edit & Enter=Retry";
        title = fmt::format("[Keyboard] {} | {}_ ({})", sdl_text_check->second, display, hint);
    } else if (!sdl_status_note.empty()) {
        title = fmt::format("[Keyboard] {}: {}_ ({})", sdl_header, display, sdl_status_note);
    } else {
        title = fmt::format("[Keyboard] {}: {}_ (Enter=OK{})", sdl_header, display,
                            sdl_disable_cancel ? "" : ", Esc=Cancel");
    }
    SDL_SetWindowTitle(window, title.c_str());
}

void SoftwareKeyboardController::AppendSdlUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if ((lead & 0x80) == 0) {
            len = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > text.size()) {
            break;
        }
        const std::string_view ch = text.substr(i, len);
        i += len;
        if (len == 1 && static_cast<unsigned char>(ch[0]) < 0x20) {
            if (!(sdl_multiline && ch[0] == '\n')) {
                continue;
            }
        }
        std::string candidate = sdl_draft;
        candidate.append(ch);
        if (Utf16CodeUnitCount(candidate) > sdl_max_length ||
            candidate.size() >= Service::AM::Frontend::STRING_BUFFER_SIZE) {
            break;
        }
        sdl_draft = std::move(candidate);
    }
    sdl_status_note.clear();
    if (sdl_text_check && sdl_text_check->first == Gpui::SwkbdTextCheckKind::Confirm) {
        sdl_text_check.reset();
    }
}

void SoftwareKeyboardController::PopSdlLastCodepoint() {
    while (!sdl_draft.empty()) {
        const unsigned char byte = static_cast<unsigned char>(sdl_draft.back());
        sdl_draft.pop_back();
        if ((byte & 0xC0) != 0x80) {
            break;
        }
    }
    sdl_status_note.clear();
    if (sdl_text_check && sdl_text_check->first == Gpui::SwkbdTextCheckKind::Confirm) {
        sdl_text_check.reset();
    }
}

bool SoftwareKeyboardController::HandleSdlEvent(const SDL_Event& event, SDL_Window* window) {
    if (!sdl_open) {
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_TEXT_INPUT:
        if (event.text.text) {
            AppendSdlUtf8(event.text.text);
            UpdateSdlWindowTitle(window);
        }
        return true;
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode key = event.key.key;
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (sdl_multiline && (event.key.mod & SDL_KMOD_SHIFT) != 0) {
                AppendSdlUtf8("\n");
                UpdateSdlWindowTitle(window);
                return true;
            }
            const bool confirmed =
                sdl_text_check && sdl_text_check->first == Gpui::SwkbdTextCheckKind::Confirm;
            std::string error;
            if (!Submit(sdl_request_id, sdl_draft, confirmed, error)) {
                sdl_status_note = std::move(error);
                UpdateSdlWindowTitle(window);
            }
            return true;
        }
        if (key == SDLK_ESCAPE) {
            if (sdl_text_check && sdl_text_check->first == Gpui::SwkbdTextCheckKind::Confirm) {
                sdl_text_check.reset();
                UpdateSdlWindowTitle(window);
                return true;
            }
            std::string error;
            if (!Cancel(sdl_request_id, error)) {
                sdl_status_note = std::move(error);
                UpdateSdlWindowTitle(window);
            }
            return true;
        }
        if (key == SDLK_BACKSPACE) {
            PopSdlLastCodepoint();
            UpdateSdlWindowTitle(window);
            return true;
        }
        if (key == SDLK_V && (event.key.mod & SDL_KMOD_CTRL) != 0) {
            if (char* clip = SDL_GetClipboardText()) {
                AppendSdlUtf8(clip);
                SDL_free(clip);
                UpdateSdlWindowTitle(window);
            }
            return true;
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_EDITING:
        return true;
    default:
        return false;
    }
}

HostSoftwareKeyboardApplet::HostSoftwareKeyboardApplet(
    std::shared_ptr<SoftwareKeyboardController> controller_)
    : controller(std::move(controller_)) {}

HostSoftwareKeyboardApplet::~HostSoftwareKeyboardApplet() = default;

void HostSoftwareKeyboardApplet::Close() const {
    if (controller) {
        controller->CloseNormal();
    }
    inline_fallback.Close();
}

void HostSoftwareKeyboardApplet::InitializeKeyboard(
    bool is_inline, Core::Frontend::KeyboardInitializeParameters initialize_parameters,
    SubmitNormalCallback submit_normal_callback_, SubmitInlineCallback submit_inline_callback_) {
    is_inline_mode = is_inline;
    if (is_inline) {
        inline_fallback.InitializeKeyboard(true, std::move(initialize_parameters), {},
                                           std::move(submit_inline_callback_));
        return;
    }
    if (controller) {
        controller->InitializeNormal(std::move(initialize_parameters),
                                     std::move(submit_normal_callback_));
    }
}

void HostSoftwareKeyboardApplet::ShowNormalKeyboard() const {
    if (controller) {
        controller->ShowNormal();
    }
}

void HostSoftwareKeyboardApplet::ShowTextCheckDialog(
    Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
    std::u16string text_check_message) const {
    if (controller) {
        controller->ShowTextCheck(text_check_result, std::move(text_check_message));
    }
}

void HostSoftwareKeyboardApplet::ShowInlineKeyboard(
    Core::Frontend::InlineAppearParameters appear_parameters) const {
    inline_fallback.ShowInlineKeyboard(std::move(appear_parameters));
}

void HostSoftwareKeyboardApplet::HideInlineKeyboard() const {
    inline_fallback.HideInlineKeyboard();
}

void HostSoftwareKeyboardApplet::InlineTextChanged(
    Core::Frontend::InlineTextParameters text_parameters) const {
    inline_fallback.InlineTextChanged(std::move(text_parameters));
}

void HostSoftwareKeyboardApplet::ExitKeyboard() const {
    if (is_inline_mode) {
        inline_fallback.ExitKeyboard();
        return;
    }
    if (controller) {
        controller->ExitNormal();
    }
}

} // namespace YuzuCmd
