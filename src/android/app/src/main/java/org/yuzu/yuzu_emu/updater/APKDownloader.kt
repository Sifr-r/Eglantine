// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.updater

import okhttp3.Call
import okhttp3.Callback
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

class APKDownloader(private val url: String, private val outputFile: File) {

    fun download(onProgress: (Int) -> Unit, onComplete: (Boolean) -> Unit) {
        val client = OkHttpClient()
        val request = Request.Builder().url(url).build()

        client.newCall(request).enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                e.printStackTrace()
                onComplete(false)
            }

            override fun onResponse(call: Call, response: Response) {
                response.use { resp ->
                    if (!resp.isSuccessful) {
                        onComplete(false)
                        return
                    }

                    val body = resp.body
                    if (body == null) {
                        onComplete(false)
                        return
                    }

                    val contentLength = body.contentLength()
                    try {
                        body.byteStream().use { inputStream ->
                            FileOutputStream(outputFile).use { outputStream ->
                                val buffer = ByteArray(4096)
                                var bytesRead: Int
                                var totalBytesRead: Long = 0

                                while (inputStream.read(buffer).also { bytesRead = it } != -1) {
                                    outputStream.write(buffer, 0, bytesRead)
                                    totalBytesRead += bytesRead
                                    if (contentLength > 0) {
                                        val progress =
                                            ((totalBytesRead * 100) / contentLength).toInt()
                                                .coerceIn(0, 100)
                                        onProgress(progress)
                                    }
                                }
                                outputStream.flush()
                            }
                        }
                        onComplete(true)
                    } catch (e: IOException) {
                        e.printStackTrace()
                        onComplete(false)
                    }
                }
            }
        })
    }
}
