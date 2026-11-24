// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <utility>

#include "common/logging.h"
#include "common/string_util.h"
#include "core/frontend/applets/software_keyboard.h"

namespace Core::Frontend {

SoftwareKeyboardApplet::~SoftwareKeyboardApplet() = default;

DefaultSoftwareKeyboardApplet::~DefaultSoftwareKeyboardApplet() {
    CancelInlineSubmit();
}

void DefaultSoftwareKeyboardApplet::Close() const {
    CancelInlineSubmit();
}

void DefaultSoftwareKeyboardApplet::InitializeKeyboard(
    bool is_inline, KeyboardInitializeParameters initialize_parameters,
    SubmitNormalCallback submit_normal_callback_, SubmitInlineCallback submit_inline_callback_) {
    CancelInlineSubmit();
    if (is_inline) {
        LOG_WARNING(
            Service_AM,
            "(STUBBED) called, backend requested to initialize the inline software keyboard.");

        submit_inline_callback = std::move(submit_inline_callback_);
    } else {
        LOG_WARNING(
            Service_AM,
            "(STUBBED) called, backend requested to initialize the normal software keyboard.");

        submit_normal_callback = std::move(submit_normal_callback_);
    }

    parameters = std::move(initialize_parameters);

    LOG_INFO(Service_AM,
             "\nKeyboardInitializeParameters:"
             "\nok_text={}"
             "\nheader_text={}"
             "\nsub_text={}"
             "\nguide_text={}"
             "\ninitial_text={}"
             "\nmax_text_length={}"
             "\nmin_text_length={}"
             "\ninitial_cursor_position={}"
             "\ntype={}"
             "\npassword_mode={}"
             "\ntext_draw_type={}"
             "\nkey_disable_flags={}"
             "\nuse_blur_background={}"
             "\nenable_backspace_button={}"
             "\nenable_return_button={}"
             "\ndisable_cancel_button={}",
             Common::UTF16ToUTF8(parameters.ok_text), Common::UTF16ToUTF8(parameters.header_text),
             Common::UTF16ToUTF8(parameters.sub_text), Common::UTF16ToUTF8(parameters.guide_text),
             Common::UTF16ToUTF8(parameters.initial_text), parameters.max_text_length,
             parameters.min_text_length, parameters.initial_cursor_position, parameters.type,
             parameters.password_mode, parameters.text_draw_type, parameters.key_disable_flags.raw,
             parameters.use_blur_background, parameters.enable_backspace_button,
             parameters.enable_return_button, parameters.disable_cancel_button);
}

void DefaultSoftwareKeyboardApplet::ShowNormalKeyboard() const {
    LOG_WARNING(Service_AM,
                "(STUBBED) called, backend requested to show the normal software keyboard.");

    SubmitNormalText(u"Eden");
}

void DefaultSoftwareKeyboardApplet::ShowTextCheckDialog(
    Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
    std::u16string text_check_message) const {
    LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to show the text check dialog.");
}

void DefaultSoftwareKeyboardApplet::ShowInlineKeyboard(
    InlineAppearParameters appear_parameters) const {
    LOG_WARNING(Service_AM,
                "(STUBBED) called, backend requested to show the inline software keyboard.");

    LOG_INFO(Service_AM,
             "\nInlineAppearParameters:"
             "\nmax_text_length={}"
             "\nmin_text_length={}"
             "\nkey_top_scale_x={}"
             "\nkey_top_scale_y={}"
             "\nkey_top_translate_x={}"
             "\nkey_top_translate_y={}"
             "\ntype={}"
             "\nkey_disable_flags={}"
             "\nkey_top_as_floating={}"
             "\nenable_backspace_button={}"
             "\nenable_return_button={}"
             "\ndisable_cancel_button={}",
             appear_parameters.max_text_length, appear_parameters.min_text_length,
             appear_parameters.key_top_scale_x, appear_parameters.key_top_scale_y,
             appear_parameters.key_top_translate_x, appear_parameters.key_top_translate_y,
             appear_parameters.type, appear_parameters.key_disable_flags.raw,
             appear_parameters.key_top_as_floating, appear_parameters.enable_backspace_button,
             appear_parameters.enable_return_button, appear_parameters.disable_cancel_button);

    CancelInlineSubmit();
    {
        std::scoped_lock lk{inline_submit_mutex};
        if (inline_submit_thread.joinable() &&
            inline_submit_thread.get_id() == std::this_thread::get_id()) {
            return;
        }
        inline_submit_thread = std::jthread([this](std::stop_token stop_token) {
            SubmitInlineText(stop_token, u"Eden");
        });
    }
}

void DefaultSoftwareKeyboardApplet::HideInlineKeyboard() const {
    LOG_WARNING(Service_AM,
                "(STUBBED) called, backend requested to hide the inline software keyboard.");
    CancelInlineSubmit();
}

void DefaultSoftwareKeyboardApplet::InlineTextChanged(InlineTextParameters text_parameters) const {
    LOG_WARNING(Service_AM,
                "(STUBBED) called, backend requested to change the inline keyboard text.");

    LOG_INFO(Service_AM,
             "\nInlineTextParameters:"
             "\ninput_text={}"
             "\ncursor_position={}",
             Common::UTF16ToUTF8(text_parameters.input_text), text_parameters.cursor_position);

    submit_inline_callback(Service::AM::Frontend::SwkbdReplyType::ChangedString,
                           text_parameters.input_text, text_parameters.cursor_position);
}

void DefaultSoftwareKeyboardApplet::ExitKeyboard() const {
    LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to exit the software keyboard.");
    CancelInlineSubmit();
}

void DefaultSoftwareKeyboardApplet::CancelInlineSubmit() const {
    std::jthread thread_to_join;
    {
        std::scoped_lock lk{inline_submit_mutex};
        if (inline_submit_thread.joinable() &&
            inline_submit_thread.get_id() != std::this_thread::get_id()) {
            thread_to_join = std::move(inline_submit_thread);
        }
    }
    if (thread_to_join.joinable()) {
        thread_to_join.request_stop();
        thread_to_join.join();
    }
}

void DefaultSoftwareKeyboardApplet::SubmitNormalText(std::u16string text) const {
    submit_normal_callback(Service::AM::Frontend::SwkbdResult::Ok, text, true);
}

void DefaultSoftwareKeyboardApplet::SubmitInlineText(std::stop_token stop_token,
                                                     std::u16string_view text) const {
    const auto wait_for_step = [&](std::chrono::milliseconds delay) {
        std::unique_lock lk{inline_submit_mutex};
        return !inline_submit_cv.wait_for(lk, stop_token, delay,
                                          [&stop_token] { return stop_token.stop_requested(); });
    };

    if (!wait_for_step(std::chrono::milliseconds{500})) {
        return;
    }

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (stop_token.stop_requested()) {
            return;
        }
        submit_inline_callback(Service::AM::Frontend::SwkbdReplyType::ChangedString,
                               std::u16string(text.data(), text.data() + index + 1),
                               static_cast<s32>(index) + 1);

        if (!wait_for_step(std::chrono::milliseconds{250})) {
            return;
        }
    }

    if (stop_token.stop_requested()) {
        return;
    }
    submit_inline_callback(Service::AM::Frontend::SwkbdReplyType::DecidedEnter,
                           std::u16string(text), static_cast<s32>(text.size()));
}

} // namespace Core::Frontend
