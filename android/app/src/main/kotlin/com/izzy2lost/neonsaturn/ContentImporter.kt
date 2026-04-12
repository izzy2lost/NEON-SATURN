package com.izzy2lost.neonsaturn

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException

enum class ContentBucket {
    IPL,
    CDB,
    DISC
}

class ContentImporter(
    private val context: Context,
    private val paths: NeonSaturnPaths = context.neonSaturnPaths()
) {
    fun importDocument(uri: Uri, bucket: ContentBucket): File {
        paths.ensureAll()

        val displayName = resolveDisplayName(uri) ?: defaultFileName(bucket)
        val targetDir = when (bucket) {
            ContentBucket.IPL -> paths.iplDir
            ContentBucket.CDB -> paths.cdbDir
            ContentBucket.DISC -> paths.discDir
        }
        val target = uniqueFile(targetDir, sanitizeFileName(displayName))

        val inputStream = context.contentResolver.openInputStream(uri)
            ?: throw IOException("Unable to open document stream")

        inputStream.use { input ->
            target.outputStream().use { output ->
                input.copyTo(output)
            }
        }

        return target
    }

    private fun resolveDisplayName(uri: Uri): String? {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val columnIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (columnIndex >= 0) {
                    return cursor.getString(columnIndex)
                }
            }
        }
        return null
    }

    private fun sanitizeFileName(value: String): String =
        value.replace(Regex("""[^\w.\- ]"""), "_").trim().ifBlank { "imported.bin" }

    private fun uniqueFile(directory: File, fileName: String): File {
        if (!directory.exists()) {
            directory.mkdirs()
        }

        val dotIndex = fileName.lastIndexOf('.')
        val baseName = if (dotIndex > 0) fileName.substring(0, dotIndex) else fileName
        val extension = if (dotIndex > 0) fileName.substring(dotIndex) else ""

        var index = 0
        while (true) {
            val suffix = if (index == 0) "" else "-$index"
            val candidate = File(directory, "$baseName$suffix$extension")
            if (!candidate.exists()) {
                return candidate
            }
            index += 1
        }
    }

    private fun defaultFileName(bucket: ContentBucket): String =
        when (bucket) {
            ContentBucket.IPL -> "saturn_ipl.bin"
            ContentBucket.CDB -> "cd_block.bin"
            ContentBucket.DISC -> "disc_image.bin"
        }
}

