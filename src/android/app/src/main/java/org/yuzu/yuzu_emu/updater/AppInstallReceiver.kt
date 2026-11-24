// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.updater

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

class AppInstallReceiver(
    private val onComplete: () -> Unit,
    private val onFailure: (Exception) -> Unit,
    private val expectedPackageName: String? = null
) : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val packageName = intent.data?.schemeSpecificPart
        val targetPackage = expectedPackageName ?: context.applicationContext.packageName
        if (packageName != targetPackage) {
            return
        }

        runCatching { context.unregisterReceiver(this) }
        when (intent.action) {
            Intent.ACTION_PACKAGE_ADDED, Intent.ACTION_PACKAGE_REPLACED -> {
                Log.i("AppInstallReceiver", "Package installed or updated: $packageName")
                onComplete()
            }
            else -> {
                onFailure(Exception("Installation failed for package: $packageName"))
            }
        }
    }
}
