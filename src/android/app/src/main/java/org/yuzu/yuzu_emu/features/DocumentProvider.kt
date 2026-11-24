// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

package org.yuzu.yuzu_emu.features

import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.DocumentsProvider
import android.webkit.MimeTypeMap
import java.io.*
import org.yuzu.yuzu_emu.BuildConfig
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.getPublicFilesDir

class DocumentProvider : DocumentsProvider() {
    private val baseDirectory: File
        get() = File(YuzuApplication.application.getPublicFilesDir().canonicalPath)

    companion object {
        private val DEFAULT_ROOT_PROJECTION: Array<String> = arrayOf(
            DocumentsContract.Root.COLUMN_ROOT_ID,
            DocumentsContract.Root.COLUMN_MIME_TYPES,
            DocumentsContract.Root.COLUMN_FLAGS,
            DocumentsContract.Root.COLUMN_ICON,
            DocumentsContract.Root.COLUMN_TITLE,
            DocumentsContract.Root.COLUMN_SUMMARY,
            DocumentsContract.Root.COLUMN_DOCUMENT_ID,
            DocumentsContract.Root.COLUMN_AVAILABLE_BYTES
        )

        private val DEFAULT_DOCUMENT_PROJECTION: Array<String> = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
            DocumentsContract.Document.COLUMN_FLAGS,
            DocumentsContract.Document.COLUMN_SIZE
        )

        const val AUTHORITY: String = BuildConfig.APPLICATION_ID + ".user"
        const val ROOT_ID: String = "root"
    }

    override fun onCreate(): Boolean {
        return true
    }

    private fun ensureContainedIn(candidate: File, root: File, label: String): File {
        val canonicalRoot = root.canonicalFile
        val canonicalCandidate = candidate.canonicalFile
        val rootPath = canonicalRoot.path
        val rootPrefix = if (rootPath.endsWith(File.separator)) {
            rootPath
        } else {
            rootPath + File.separator
        }
        if (canonicalCandidate != canonicalRoot && !canonicalCandidate.path.startsWith(rootPrefix)) {
            throw FileNotFoundException("'$label' is outside allowed directory")
        }
        return canonicalCandidate
    }

    private fun validateDisplayName(displayName: String?): String {
        if (displayName.isNullOrEmpty() ||
            displayName == "." ||
            displayName == ".." ||
            displayName.contains('/') ||
            displayName.contains('\\') ||
            displayName.contains('\u0000')
        ) {
            throw FileNotFoundException("Invalid document name '$displayName'")
        }
        return displayName
    }

    /**
     * @return The [File] that corresponds to the document ID supplied by [getDocumentId]
     */
    private fun getFile(documentId: String): File {
        if (documentId != ROOT_ID && !documentId.startsWith("$ROOT_ID/")) {
            throw FileNotFoundException("'$documentId' is not in any known root")
        }
        val relativePath = if (documentId == ROOT_ID) {
            ""
        } else {
            documentId.removePrefix("$ROOT_ID/")
        }
        val file = ensureContainedIn(baseDirectory.resolve(relativePath), baseDirectory, documentId)
        if (!file.exists()) {
            throw FileNotFoundException(
                "${file.absolutePath} ($documentId) not found"
            )
        }
        return file
    }

    /**
     * @return A unique ID for the provided [File]
     */
    private fun getDocumentId(file: File): String {
        return "$ROOT_ID/${file.toRelativeString(baseDirectory)}"
    }

    override fun queryRoots(projection: Array<out String>?): Cursor {
        val cursor = MatrixCursor(projection ?: DEFAULT_ROOT_PROJECTION)

        cursor.newRow().apply {
            add(DocumentsContract.Root.COLUMN_ROOT_ID, ROOT_ID)
            add(DocumentsContract.Root.COLUMN_SUMMARY, null)
            add(
                DocumentsContract.Root.COLUMN_FLAGS,
                DocumentsContract.Root.FLAG_SUPPORTS_CREATE or
                    DocumentsContract.Root.FLAG_SUPPORTS_IS_CHILD
            )
            add(DocumentsContract.Root.COLUMN_TITLE, context!!.getString(R.string.app_name))
            add(DocumentsContract.Root.COLUMN_DOCUMENT_ID, getDocumentId(baseDirectory))
            add(DocumentsContract.Root.COLUMN_MIME_TYPES, "*/*")
            add(DocumentsContract.Root.COLUMN_AVAILABLE_BYTES, baseDirectory.freeSpace)
            add(DocumentsContract.Root.COLUMN_ICON, R.drawable.ic_yuzu)
        }

        return cursor
    }

    override fun queryDocument(documentId: String?, projection: Array<out String>?): Cursor {
        val cursor = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        return includeFile(cursor, documentId, null)
    }

    override fun isChildDocument(parentDocumentId: String?, documentId: String?): Boolean {
        if (parentDocumentId == null || documentId == null) {
            return false
        }
        val prefix = if (parentDocumentId.endsWith('/')) {
            parentDocumentId
        } else {
            "$parentDocumentId/"
        }
        return documentId == parentDocumentId || documentId.startsWith(prefix)
    }

    /**
     * @return A new [File] with a unique name based off the supplied [name], not conflicting with any existing file
     */
    private fun File.resolveWithoutConflict(name: String): File {
        var file = resolve(name)
        if (file.exists()) {
            var noConflictId =
                1 // Makes sure two files don't have the same name by adding a number to the end
            val extension = name.substringAfterLast('.')
            val baseName = name.substringBeforeLast('.')
            while (file.exists())
                file = resolve("$baseName (${noConflictId++}).$extension")
        }
        return file
    }

    override fun createDocument(
        parentDocumentId: String?,
        mimeType: String?,
        displayName: String
    ): String {
        val parentId = parentDocumentId
            ?: throw FileNotFoundException("Parent document ID cannot be null")
        val parentFile = getFile(parentId)
        if (!parentFile.isDirectory) {
            throw FileNotFoundException("Parent '$parentId' is not a directory")
        }
        val safeName = validateDisplayName(displayName)
        val candidate = parentFile.resolveWithoutConflict(safeName)
        val newFile = ensureContainedIn(candidate, baseDirectory, safeName)
        ensureContainedIn(newFile, parentFile, safeName)

        try {
            if (DocumentsContract.Document.MIME_TYPE_DIR == mimeType) {
                if (!newFile.mkdir()) {
                    throw IOException("Failed to create directory")
                }
            } else {
                if (!newFile.createNewFile()) {
                    throw IOException("Failed to create file")
                }
            }
        } catch (e: IOException) {
            throw FileNotFoundException("Couldn't create document '${newFile.path}': ${e.message}")
        }

        return getDocumentId(newFile)
    }

    override fun deleteDocument(documentId: String?) {
        val docId = documentId ?: throw FileNotFoundException("Document ID cannot be null")
        val file = getFile(docId)
        if (!file.delete()) {
            throw FileNotFoundException("Couldn't delete document with ID '$docId'")
        }
    }

    override fun removeDocument(documentId: String, parentDocumentId: String?) {
        val parentId = parentDocumentId
            ?: throw FileNotFoundException("Parent document ID cannot be null")
        val parent = getFile(parentId)
        val file = getFile(documentId)

        if (parent == file || file.parentFile == null || file.parentFile == parent) {
            if (!file.delete()) {
                throw FileNotFoundException("Couldn't delete document with ID '$documentId'")
            }
        } else {
            throw FileNotFoundException("Couldn't delete document with ID '$documentId'")
        }
    }

    override fun renameDocument(documentId: String?, displayName: String?): String {
        val docId = documentId ?: throw FileNotFoundException("Document ID cannot be null")
        val safeName = validateDisplayName(displayName)

        val sourceFile = getFile(docId)
        if (sourceFile == baseDirectory.canonicalFile) {
            throw FileNotFoundException("Couldn't rename root directory '$docId'")
        }
        val sourceParentFile = sourceFile.parentFile
            ?: throw FileNotFoundException(
                "Couldn't rename document '$docId' as it has no parent"
            )
        ensureContainedIn(sourceParentFile, baseDirectory, docId)
        val destFile = ensureContainedIn(
            sourceParentFile.resolve(safeName),
            baseDirectory,
            safeName
        )
        ensureContainedIn(destFile, sourceParentFile, safeName)

        try {
            if (!sourceFile.renameTo(destFile)) {
                throw FileNotFoundException(
                    "Couldn't rename document from '${sourceFile.name}' to '${destFile.name}'"
                )
            }
        } catch (e: Exception) {
            throw FileNotFoundException(
                "Couldn't rename document from '${sourceFile.name}' to '${destFile.name}': " +
                    "${e.message}"
            )
        }

        return getDocumentId(destFile)
    }

    private fun copyDocument(
        sourceDocumentId: String,
        sourceParentDocumentId: String,
        targetParentDocumentId: String?
    ): String {
        if (!isChildDocument(sourceParentDocumentId, sourceDocumentId)) {
            throw FileNotFoundException(
                "Couldn't copy document '$sourceDocumentId' as its parent is not " +
                    "'$sourceParentDocumentId'"
            )
        }

        return copyDocument(sourceDocumentId, targetParentDocumentId)
    }

    override fun copyDocument(sourceDocumentId: String, targetParentDocumentId: String?): String {
        val parentId = targetParentDocumentId
            ?: throw FileNotFoundException("Target parent document ID cannot be null")
        val parent = getFile(parentId)
        val oldFile = getFile(sourceDocumentId)
        val newFile = ensureContainedIn(
            parent.resolveWithoutConflict(oldFile.name),
            baseDirectory,
            oldFile.name
        )

        try {
            if (!(
                newFile.createNewFile() && newFile.setWritable(true) &&
                    newFile.setReadable(true)
                )
            ) {
                throw IOException("Couldn't create new file")
            }

            FileInputStream(oldFile).use { inStream ->
                FileOutputStream(newFile).use { outStream ->
                    inStream.copyTo(outStream)
                }
            }
        } catch (e: IOException) {
            throw FileNotFoundException("Couldn't copy document '$sourceDocumentId': ${e.message}")
        }

        return getDocumentId(newFile)
    }

    override fun moveDocument(
        sourceDocumentId: String,
        sourceParentDocumentId: String?,
        targetParentDocumentId: String?
    ): String {
        try {
            val newDocumentId = copyDocument(
                sourceDocumentId,
                sourceParentDocumentId!!,
                targetParentDocumentId
            )
            removeDocument(sourceDocumentId, sourceParentDocumentId)
            return newDocumentId
        } catch (e: FileNotFoundException) {
            throw FileNotFoundException("Couldn't move document '$sourceDocumentId'")
        }
    }

    private fun includeFile(cursor: MatrixCursor, documentId: String?, file: File?): MatrixCursor {
        val localDocumentId = documentId ?: file?.let { getDocumentId(it) }
        val localFile = file ?: getFile(documentId!!)

        var flags = 0
        if (localFile.isDirectory && localFile.canWrite()) {
            flags = DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE
        } else if (localFile.canWrite()) {
            flags = DocumentsContract.Document.FLAG_SUPPORTS_WRITE
            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_DELETE

            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_REMOVE
            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_MOVE
            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_COPY
            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_RENAME
        }

        cursor.newRow().apply {
            add(DocumentsContract.Document.COLUMN_DOCUMENT_ID, localDocumentId)
            add(
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                if (localFile == baseDirectory) {
                    context!!.getString(R.string.app_name)
                } else {
                    localFile.name
                }
            )
            add(DocumentsContract.Document.COLUMN_SIZE, localFile.length())
            add(DocumentsContract.Document.COLUMN_MIME_TYPE, getTypeForFile(localFile))
            add(DocumentsContract.Document.COLUMN_LAST_MODIFIED, localFile.lastModified())
            add(DocumentsContract.Document.COLUMN_FLAGS, flags)
            if (localFile == baseDirectory) {
                add(DocumentsContract.Root.COLUMN_ICON, R.drawable.ic_yuzu)
            }
        }

        return cursor
    }

    private fun getTypeForFile(file: File): Any {
        return if (file.isDirectory) {
            DocumentsContract.Document.MIME_TYPE_DIR
        } else {
            getTypeForName(file.name)
        }
    }

    private fun getTypeForName(name: String): Any {
        val lastDot = name.lastIndexOf('.')
        if (lastDot >= 0) {
            val extension = name.substring(lastDot + 1)
            val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension)
            if (mime != null) {
                return mime
            }
        }
        return "application/octect-stream"
    }

    override fun queryChildDocuments(
        parentDocumentId: String?,
        projection: Array<out String>?,
        sortOrder: String?
    ): Cursor {
        var cursor = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)

        val parentId = parentDocumentId
            ?: throw FileNotFoundException("Parent document ID cannot be null")
        val parent = getFile(parentId)
        if (!parent.isDirectory) {
            throw FileNotFoundException("'$parentId' is not a directory")
        }
        for (file in parent.listFiles().orEmpty()) {
            cursor = includeFile(cursor, null, file)
        }

        return cursor
    }

    override fun openDocument(
        documentId: String?,
        mode: String?,
        signal: CancellationSignal?
    ): ParcelFileDescriptor {
        val docId = documentId ?: throw FileNotFoundException("Document ID cannot be null")
        val file = getFile(docId)
        val accessMode = ParcelFileDescriptor.parseMode(mode)
        return ParcelFileDescriptor.open(file, accessMode)
    }
}
