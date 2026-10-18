package com.izzy2lost.neonsaturn

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import androidx.documentfile.provider.DocumentFile
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.CRC32

/*
 * Names games by the Redump datfile, from the CRC32 of their first track,
 * so covers match however the files are named. Hashing reads the whole data
 * track, so results are kept per file (by URI, size and mtime) and a file is
 * only hashed again when it changes. ISO and MDS images aren't hashed: an
 * ISO lacks the raw sector bytes Redump hashes, and MDS is rare enough to
 * leave to the filename.
 */
object DiscIdentifier {
    private const val CACHE_FILE = "disc-ids.json"
    private const val REDUMP_ASSET = "covers/redump_track1.json"
    private const val SECTOR_SIZE = 2352L
    private const val FRAMES_PER_SECOND = 75
    private const val SECONDS_PER_MINUTE = 60

    private val CUE_FILE = Regex("""^\s*FILE\s+"([^"]+)"""", RegexOption.IGNORE_CASE)
    private val CUE_TRACK = Regex("""^\s*TRACK\s+(\d+)\s+(\S+)""", RegexOption.IGNORE_CASE)
    private val CUE_INDEX = Regex("""^\s*INDEX\s+\d+\s+(\d+):(\d+):(\d+)""", RegexOption.IGNORE_CASE)
    private val CCD_SECTION = Regex("""^\s*\[(.+)]\s*$""")
    private val CCD_INDEX = Regex("""^\s*INDEX\s+([01])\s*=\s*(\d+)""", RegexOption.IGNORE_CASE)

    private val nativeLoaded: Boolean by lazy {
        runCatching { System.loadLibrary("romid") }.isSuccess
    }

    @Volatile private var redumpNames: Map<String, String>? = null

    // Launch file URI -> Redump name, for what has been identified so far
    private val names = ConcurrentHashMap<String, String>()

    // "uri|size|mtime" -> "crc|size", or "" when the file couldn't be hashed
    private val hashes = ConcurrentHashMap<String, String>()
    @Volatile private var cacheLoaded = false

    private val executor = Executors.newSingleThreadExecutor()
    @Volatile private var generation = 0

    /* The Redump name of a game identified so far, or null. */
    fun redumpName(entry: GameLibraryEntry): String? =
        entry.sourceFiles.firstOrNull { it.name == entry.launchFileName }?.uri?.let { names[it.toString()] }

    /*
     * Identifies every game in the background. onIdentified runs on the main
     * thread once games get names, coalesced so a burst of cached results
     * refreshes the covers once. Cached games are named before any new file
     * is hashed.
     */
    fun identifyAll(context: Context, paths: NeonSaturnPaths, entries: List<GameLibraryEntry>, onIdentified: () -> Unit) {
        val appContext = context.applicationContext
        val run = ++generation
        val notifyPending = AtomicBoolean(false)
        val main = Handler(Looper.getMainLooper())

        executor.execute {
            loadCache(paths)
            val table = redumpTable(appContext)
            fun name(launchUri: Uri, hash: String) {
                val name = table[hash] ?: return
                names[launchUri.toString()] = name
                if (notifyPending.compareAndSet(false, true)) {
                    main.post {
                        notifyPending.set(false)
                        if (run == generation) onIdentified()
                    }
                }
            }

            val toHash = mutableListOf<Triple<GameLibraryEntry, Uri, String>>()
            for (entry in entries) {
                if (run != generation) return@execute
                val launchUri = entry.sourceFiles.firstOrNull { it.name == entry.launchFileName }?.uri ?: continue
                val doc = DocumentFile.fromSingleUri(appContext, launchUri) ?: continue
                val key = "$launchUri|${doc.length()}|${doc.lastModified()}"
                val cached = hashes[key]
                if (cached != null) name(launchUri, cached) else toHash += Triple(entry, launchUri, key)
            }

            for ((entry, launchUri, key) in toHash) {
                if (run != generation) break
                // "" records a file that can't be hashed, so it isn't read again
                val hash = hashTrack1(appContext, entry) ?: ""
                hashes[key] = hash
                saveCache(paths)
                name(launchUri, hash)
            }
        }
    }

    private fun hashTrack1(context: Context, entry: GameLibraryEntry): String? = try {
        when (entry.launchFileName.substringAfterLast('.', "").lowercase(Locale.ROOT)) {
            "chd" -> hashChd(context, entry)
            "cue" -> hashCue(context, entry)
            "ccd" -> hashCcd(context, entry)
            else -> null
        }
    } catch (_: Exception) {
        null
    }

    private fun hashChd(context: Context, entry: GameLibraryEntry): String? {
        if (!nativeLoaded) return null
        val uri = entry.sourceFiles.first { it.name == entry.launchFileName }.uri
        return context.contentResolver.openFileDescriptor(uri, "r")?.use { nativeHashChdTrack1(it.fd) }
    }

    private data class CueTrack(val number: Int, val mode: String, val file: String?, var start: Long? = null)

    /* Track 1 is its file from its first index to the next track's first index in the same file, or to the end. */
    private fun hashCue(context: Context, entry: GameLibraryEntry): String? {
        val cueUri = entry.sourceFiles.first { it.name == entry.launchFileName }.uri
        val lines = context.contentResolver.openInputStream(cueUri)?.bufferedReader()?.use { it.readLines() }
            ?: return null

        val tracks = mutableListOf<CueTrack>()
        var file: String? = null
        for (line in lines) {
            CUE_FILE.find(line)?.let { file = it.groupValues[1] }
            CUE_TRACK.find(line)?.let { tracks += CueTrack(it.groupValues[1].toInt(), it.groupValues[2], file) }
            CUE_INDEX.find(line)?.let {
                val track = tracks.lastOrNull() ?: return@let
                if (track.start != null) return@let
                val (m, s, f) = it.destructured
                track.start = ((m.toLong() * SECONDS_PER_MINUTE + s.toLong()) * FRAMES_PER_SECOND + f.toLong()) * SECTOR_SIZE
            }
        }

        val track1Index = tracks.indexOfFirst { it.number == 1 }
        val track1 = tracks.getOrNull(track1Index) ?: return null
        // Only raw sectors match Redump's bins
        if (!track1.mode.endsWith("/2352")) return null
        val next = tracks.getOrNull(track1Index + 1)?.takeIf { it.file == track1.file }

        val binName = track1.file?.substringAfterLast('/')?.substringAfterLast('\\') ?: return null
        val bin = entry.sourceFiles.firstOrNull { it.name.equals(binName, ignoreCase = true) } ?: return null
        return crcOf(context, bin.uri, track1.start ?: 0L, next?.start)
    }

    /* Track 1 of a CloneCD image runs from the start of the .img to track 2's first index. */
    private fun hashCcd(context: Context, entry: GameLibraryEntry): String? {
        val ccdUri = entry.sourceFiles.first { it.name == entry.launchFileName }.uri
        val lines = context.contentResolver.openInputStream(ccdUri)?.bufferedReader()?.use { it.readLines() }
            ?: return null

        var section = ""
        var track2Start: Long? = null
        for (line in lines) {
            CCD_SECTION.find(line)?.let { section = it.groupValues[1].trim().uppercase(Locale.ROOT) }
            if (section != "TRACK 2") continue
            CCD_INDEX.find(line)?.let {
                val lba = it.groupValues[2].toLong()
                track2Start = minOf(track2Start ?: lba, lba)
            }
        }

        val imgName = entry.launchFileName.substringBeforeLast('.') + ".img"
        val img = entry.sourceFiles.firstOrNull { it.name.equals(imgName, ignoreCase = true) } ?: return null
        return crcOf(context, img.uri, 0L, track2Start?.let { it * SECTOR_SIZE })
    }

    /* "crc|size" of bytes [start, end) of a file, end null meaning to the end of the file. */
    private fun crcOf(context: Context, uri: Uri, start: Long, end: Long?): String? {
        val input = context.contentResolver.openInputStream(uri) ?: return null
        input.use {
            skipFully(it, start)
            val crc = CRC32()
            val buffer = ByteArray(1 shl 20)
            var remaining = end?.let { e -> e - start } ?: Long.MAX_VALUE
            var size = 0L
            while (remaining > 0) {
                val read = it.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                if (read < 0) break
                crc.update(buffer, 0, read)
                size += read
                remaining -= read
            }
            if (end != null && remaining > 0) return null
            return String.format(Locale.ROOT, "%08x|%d", crc.value, size)
        }
    }

    private fun skipFully(input: InputStream, count: Long) {
        var left = count
        while (left > 0) {
            val skipped = input.skip(left)
            if (skipped <= 0) {
                if (input.read() < 0) throw IOException("Unexpected end of file")
                left--
            } else {
                left -= skipped
            }
        }
    }

    private fun redumpTable(context: Context): Map<String, String> =
        redumpNames ?: synchronized(this) {
            redumpNames ?: try {
                val json = JSONObject(context.assets.open(REDUMP_ASSET).bufferedReader().use { it.readText() })
                HashMap<String, String>(json.length()).apply {
                    json.keys().forEach { put(it, json.getString(it)) }
                }
            } catch (_: Exception) {
                emptyMap()
            }.also { redumpNames = it }
        }

    private fun loadCache(paths: NeonSaturnPaths) {
        if (cacheLoaded) return
        synchronized(this) {
            if (cacheLoaded) return
            try {
                val json = JSONObject(File(paths.root, CACHE_FILE).readText())
                json.keys().forEach { hashes[it] = json.getString(it) }
            } catch (_: Exception) {}
            cacheLoaded = true
        }
    }

    private fun saveCache(paths: NeonSaturnPaths) {
        try {
            val target = File(paths.root, CACHE_FILE)
            val tmp = File(target.path + ".tmp")
            tmp.writeText(JSONObject(hashes.toMap()).toString())
            tmp.renameTo(target)
        } catch (_: IOException) {}
    }

    @JvmStatic private external fun nativeHashChdTrack1(fd: Int): String?
}
