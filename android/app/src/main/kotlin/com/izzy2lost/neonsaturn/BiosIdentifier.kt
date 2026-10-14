package com.izzy2lost.neonsaturn

import android.content.Context
import java.io.File
import java.security.MessageDigest

/*
 * Labels imported BIOS files from Ymir's IPL ROM database (by hash), the same
 * one the emulator uses to pick the BIOS for a disc's region at boot.
 */
object BiosIdentifier {
    const val IPL_SIZE = 512L * 1024

    enum class Region { US_EU, JP, KR, NONE }

    data class BiosInfo(
        val version: String,
        val region: Region,
        val regionFree: Boolean,
        val variant: String
    )

    private val nativeLoaded: Boolean by lazy {
        runCatching { System.loadLibrary("romid") }.isSuccess
    }

    // Keyed by path, size and mtime so a replaced file is hashed again
    private val cache = HashMap<String, BiosInfo?>()

    fun identify(file: File): BiosInfo? {
        if (!nativeLoaded || file.length() != IPL_SIZE) return null
        val key = "${file.absolutePath}|${file.length()}|${file.lastModified()}"
        synchronized(cache) {
            if (cache.containsKey(key)) return cache[key]
        }
        val info = nativeIdentify(file.absolutePath)?.split('|')?.takeIf { it.size == 4 }?.let { parts ->
            BiosInfo(
                version = parts[0],
                region = runCatching { Region.valueOf(parts[1]) }.getOrDefault(Region.NONE),
                regionFree = parts[2] == "1",
                variant = parts[3]
            )
        }
        synchronized(cache) { cache[key] = info }
        return info
    }

    /* Every file in the BIOS folder, sorted by name. */
    fun listBiosFiles(directory: File): List<File> =
        directory.listFiles()
            ?.filter { it.isFile && !it.name.endsWith(".tmp") }
            ?.sortedBy { it.name.lowercase() }
            .orEmpty()

    fun label(context: Context, info: BiosInfo?): String {
        if (info == null) return context.getString(R.string.bios_unknown)
        val variant = when (info.variant) {
            "HISATURN" -> "HiSaturn"
            "VSATURN" -> "V-Saturn"
            "DEVKIT" -> "Dev kit"
            else -> "Saturn"
        }
        val region = when (info.region) {
            Region.US_EU -> context.getString(R.string.bios_region_us_eu)
            Region.JP -> context.getString(R.string.bios_region_jp)
            Region.KR -> context.getString(R.string.bios_region_kr)
            Region.NONE -> "?"
        }
        val regionText = if (info.regionFree) context.getString(R.string.bios_region_free, region) else region
        return "$variant ${info.version} · $regionText"
    }

    /* One line per BIOS for the setup and settings screens. */
    fun summary(context: Context, directory: File): String? =
        listBiosFiles(directory)
            .takeIf { it.isNotEmpty() }
            ?.joinToString("\n") { "${label(context, identify(it))}  (${it.name})" }

    /* True when another file in the folder has the same contents. */
    fun isDuplicate(file: File, directory: File): Boolean {
        val digest = sha1(file)
        return listBiosFiles(directory).any { other ->
            other != file && other.length() == file.length() && sha1(other).contentEquals(digest)
        }
    }

    private fun sha1(file: File): ByteArray {
        val md = MessageDigest.getInstance("SHA-1")
        file.inputStream().use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                md.update(buffer, 0, read)
            }
        }
        return md.digest()
    }

    @JvmStatic
    private external fun nativeIdentify(path: String): String?
}
