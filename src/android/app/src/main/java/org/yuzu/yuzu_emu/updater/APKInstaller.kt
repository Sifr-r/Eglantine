// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.updater

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import java.io.File

class APKInstaller(private val context: Context) {
    private val appContext = context.applicationContext
    private var registeredReceiver: AppInstallReceiver? = null
    private var lifecycleObserver: DefaultLifecycleObserver? = null

    fun install(apkFile: File, onComplete: () -> Unit, onFailure: (Exception) -> Unit) {
        unregister()
        if ((context as? LifecycleOwner)?.lifecycle?.currentState == Lifecycle.State.DESTROYED) {
            return
        }

        try {
            val apkUri: Uri = FileProvider.getUriForFile(
                appContext,
                appContext.packageName + ".provider",
                apkFile
            )
            val receiver = AppInstallReceiver(
                onComplete = {
                    unregister()
                    onComplete()
                },
                onFailure = { error ->
                    unregister()
                    onFailure(error)
                },
                expectedPackageName = appContext.packageName
            )
            val filter = IntentFilter().apply {
                addAction(Intent.ACTION_PACKAGE_ADDED)
                addAction(Intent.ACTION_PACKAGE_REPLACED)
                addDataScheme("package")
            }
            ContextCompat.registerReceiver(
                appContext,
                receiver,
                filter,
                ContextCompat.RECEIVER_EXPORTED
            )
            registeredReceiver = receiver

            (context as? LifecycleOwner)?.let { owner ->
                val observer = object : DefaultLifecycleObserver {
                    override fun onDestroy(owner: LifecycleOwner) {
                        unregister()
                    }
                }
                lifecycleObserver = observer
                owner.lifecycle.addObserver(observer)
            }

            val intent = Intent(Intent.ACTION_VIEW)
            intent.setDataAndType(apkUri, "application/vnd.android.package-archive")
            intent.flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK
            context.startActivity(intent)
        } catch (e: Exception) {
            unregister()
            onFailure(e)
        }
    }

    fun unregister() {
        registeredReceiver?.let { receiver ->
            registeredReceiver = null
            runCatching { appContext.unregisterReceiver(receiver) }
        }
        lifecycleObserver?.let { observer ->
            lifecycleObserver = null
            (context as? LifecycleOwner)?.lifecycle?.removeObserver(observer)
        }
    }
}
