package com.izzy2lost.neonsaturn

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.File

/**
 * On-disk view of one game's save state slots, as written by emulator_main.cpp:
 * `<slot>.savestate` plus an optional `<slot>.png` thumbnail, with slots numbered from 0.
 */
data class SaveStateSlot(
    /** 1-based number shown to the user. */
    val number: Int,
    val stateFile: File,
    val thumbnailFile: File,
) {
    val index: Int get() = number - 1
    val exists: Boolean get() = stateFile.isFile
    val savedAtMillis: Long get() = stateFile.lastModified()
    val hasThumbnail: Boolean get() = exists && thumbnailFile.isFile

    /** Decodes the thumbnail, downsampled so its width stays near [targetWidthPx]. */
    fun decodeThumbnail(targetWidthPx: Int = 0): Bitmap? {
        if (!hasThumbnail) {
            return null
        }
        val path = thumbnailFile.absolutePath
        val options = BitmapFactory.Options()
        if (targetWidthPx > 0) {
            options.inJustDecodeBounds = true
            BitmapFactory.decodeFile(path, options)
            var sampleSize = 1
            while (options.outWidth / (sampleSize * 2) >= targetWidthPx) {
                sampleSize *= 2
            }
            options.inJustDecodeBounds = false
            options.inSampleSize = sampleSize
        }
        return runCatching { BitmapFactory.decodeFile(path, options) }.getOrNull()
    }

    companion object {
        const val COUNT = 10

        fun listIn(directory: File): List<SaveStateSlot> =
            (1..COUNT).map { number ->
                SaveStateSlot(
                    number = number,
                    stateFile = File(directory, "${number - 1}.savestate"),
                    thumbnailFile = File(directory, "${number - 1}.png"),
                )
            }
    }
}
