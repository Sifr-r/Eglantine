// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

object GameMetadata {
    val gameExtensions: Set<String> by lazy { getFileExtensions(externalContent = false).toSet() }

    val externalContentExtensions: Set<String> by lazy {
        getFileExtensions(externalContent = true).toSet()
    }

    private external fun getFileExtensions(externalContent: Boolean): Array<String>

    external fun getIsValid(path: String): Boolean

    external fun getTitle(path: String): String

    external fun getProgramId(path: String): String

    external fun getDeveloper(path: String): String

    external fun getVersion(path: String, reload: Boolean): String

    external fun getIcon(path: String): ByteArray

    external fun getIsHomebrew(path: String): Boolean

    external fun resetMetadata()
}
