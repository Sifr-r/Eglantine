
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.applets.keyboard

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.InputMethodManager
import androidx.annotation.Keep
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import java.io.Serializable
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.applets.keyboard.ui.KeyboardDialogFragment

@Keep
object SoftwareKeyboard {
    var data: KeyboardData = KeyboardData(SwkbdResult.Cancel.ordinal, "")
    val dataLock = Object()
    private var isDataReady = false

    fun notifyNormalCompleted() {
        synchronized(dataLock) {
            isDataReady = true
            dataLock.notifyAll()
        }
    }

    private fun executeNormalImpl(config: KeyboardConfig) {
        val emulationActivity = NativeLibrary.sEmulationActivity.get()
        if (emulationActivity == null ||
            emulationActivity.isFinishing ||
            emulationActivity.isDestroyed
        ) {
            notifyNormalCompleted()
            return
        }

        val fragmentManager = emulationActivity.supportFragmentManager
        if (fragmentManager.isStateSaved) {
            notifyNormalCompleted()
            return
        }

        try {
            val fragment = KeyboardDialogFragment.newInstance(config)
            fragment.show(fragmentManager, KeyboardDialogFragment.TAG)
        } catch (_: IllegalStateException) {
            notifyNormalCompleted()
        }
    }

    private fun executeInlineImpl(config: KeyboardConfig) {
        val emulationActivity = NativeLibrary.sEmulationActivity.get()
        if (emulationActivity == null ||
            emulationActivity.isFinishing ||
            emulationActivity.isDestroyed
        ) {
            return
        }

        val overlayView =
            emulationActivity.findViewById<View>(R.id.surface_input_overlay) ?: return
        overlayView.requestFocus()
        val im =
            overlayView.context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        im.restartInput(overlayView)
        im.showSoftInput(overlayView, InputMethodManager.SHOW_FORCED)

        // There isn't a good way to know that the IMM is dismissed, so poll every 500ms to submit inline keyboard result.
        val handler = Handler(Looper.getMainLooper())
        val delayMs = 500L
        handler.postDelayed(
            object : Runnable {
                override fun run() {
                    if (!overlayView.isAttachedToWindow) {
                        return
                    }
                    val insets = ViewCompat.getRootWindowInsets(overlayView) ?: return
                    val isKeyboardVisible = insets.isVisible(WindowInsetsCompat.Type.ime())
                    if (isKeyboardVisible) {
                        handler.postDelayed(this, delayMs)
                        return
                    }

                    // No longer visible, submit the result.
                    NativeLibrary.submitInlineKeyboardInput(KeyEvent.KEYCODE_ENTER)
                }
            },
            delayMs
        )
    }

    @JvmStatic
    fun executeNormal(config: KeyboardConfig): KeyboardData {
        val emulationActivity = NativeLibrary.sEmulationActivity.get()
        if (emulationActivity == null ||
            emulationActivity.isFinishing ||
            emulationActivity.isDestroyed
        ) {
            return KeyboardData(SwkbdResult.Cancel.ordinal, "")
        }

        synchronized(dataLock) {
            data = KeyboardData(SwkbdResult.Cancel.ordinal, "")
            isDataReady = false
        }

        emulationActivity.runOnUiThread { executeNormalImpl(config) }

        synchronized(dataLock) {
            while (!isDataReady) {
                try {
                    dataLock.wait()
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    break
                }
            }
            return data.copy()
        }
    }

    @JvmStatic
    fun executeInline(config: KeyboardConfig) {
        val emulationActivity = NativeLibrary.sEmulationActivity.get() ?: return
        if (emulationActivity.isFinishing || emulationActivity.isDestroyed) {
            return
        }
        emulationActivity.runOnUiThread { executeInlineImpl(config) }
    }

    // Corresponds to Service::AM::Applets::SwkbdType
    enum class SwkbdType {
        Normal,
        NumberPad,
        Qwerty,
        Unknown3,
        Latin,
        SimplifiedChinese,
        TraditionalChinese,
        Korean
    }

    // Corresponds to Service::AM::Applets::SwkbdPasswordMode
    enum class SwkbdPasswordMode {
        Disabled,
        Enabled
    }

    // Corresponds to Service::AM::Applets::SwkbdResult
    enum class SwkbdResult {
        Ok,
        Cancel
    }

    @Keep
    data class KeyboardConfig(
        var ok_text: String? = null,
        var header_text: String? = null,
        var sub_text: String? = null,
        var guide_text: String? = null,
        var initial_text: String? = null,
        var left_optional_symbol_key: Short = 0,
        var right_optional_symbol_key: Short = 0,
        var max_text_length: Int = 0,
        var min_text_length: Int = 0,
        var initial_cursor_position: Int = 0,
        var type: Int = 0,
        var password_mode: Int = 0,
        var text_draw_type: Int = 0,
        var key_disable_flags: Int = 0,
        var use_blur_background: Boolean = false,
        var enable_backspace_button: Boolean = false,
        var enable_return_button: Boolean = false,
        var disable_cancel_button: Boolean = false
    ) : Serializable

    // Corresponds to Frontend::KeyboardData
    @Keep
    data class KeyboardData(var result: Int, var text: String)
}
