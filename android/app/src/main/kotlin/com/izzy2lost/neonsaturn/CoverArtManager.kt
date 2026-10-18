package com.izzy2lost.neonsaturn

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import coil.ImageLoader
import coil.annotation.ExperimentalCoilApi
import coil.disk.DiskCache
import coil.memory.MemoryCache
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.text.Normalizer
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

/*
 * Covers work offline the way x1box's do: the index ships in the APK (and
 * the last downloaded copy is kept), and every cover that loads is saved as
 * a plain file that later binds use instead of the network.
 */
object CoverArtManager {
    // Update if the repo ever moves
    private const val GITHUB_BASE = "https://raw.githubusercontent.com/izzy2lost/saturn-covers/main"
    private const val NA_PATH = "na"
    private const val JAPAN_PATH = "japan"
    private const val COVERS_DIR = "covers"
    private const val INDEX_FILE = "index.json"
    private const val ALIASES_FILE = "aliases.json"

    // Trailing parenthetical groups ROM tools add: (USA), (Japan), (Disc 1), (Rev A), etc.
    private val TRAILING_PARENS = Regex("""\s*\([^)]+\)\s*$""")
    private val UNSAFE_FILE_CHARS = Regex("""[\\/:*?"<>|]""")

    // For match keys: every (...) or [...] group, "Name, The - Subtitle" articles, and all punctuation and case
    private val BRACKETED = Regex("""\s*[(\[][^)\]]*[)\]]""")
    private val TRAILING_ARTICLE = Regex("""^(.*?), (The|A|An)\b(.*)$""", RegexOption.IGNORE_CASE)
    private val NON_ALPHANUMERIC = Regex("""[^a-z0-9]""")

    @Volatile private var loader: ImageLoader? = null

    // Match key -> cover title. Replaced whole, never mutated, so binds on the UI thread can read without locking
    @Volatile private var naIndex: Map<String, String> = emptyMap()
    @Volatile private var japanIndex: Map<String, String> = emptyMap()

    private enum class IndexState { IDLE, LOADING, READY, FAILED }
    @Volatile private var naState    = IndexState.IDLE
    @Volatile private var japanState = IndexState.IDLE

    // Single pending callback per mode — only LauncherActivity ever registers one at a time
    @Volatile private var naCallback:    (() -> Unit)? = null
    @Volatile private var japanCallback: (() -> Unit)? = null

    private val saveExecutor = Executors.newSingleThreadExecutor()
    private val savesInFlight = ConcurrentHashMap.newKeySet<String>()

    fun imageLoader(context: Context, paths: NeonSaturnPaths): ImageLoader =
        loader ?: synchronized(this) {
            loader ?: ImageLoader.Builder(context.applicationContext)
                .memoryCache {
                    MemoryCache.Builder(context.applicationContext)
                        .maxSizePercent(0.20)
                        .build()
                }
                .diskCache {
                    DiskCache.Builder()
                        .directory(File(paths.root, "covers-cache"))
                        .maxSizeBytes(300L * 1024 * 1024)
                        .build()
                }
                // GitHub raw sends max-age=300; honouring it would refetch (and fail offline)
                .respectCacheHeaders(false)
                .build()
                .also { loader = it }
        }

    /*
     * The saved or bundled index is ready at once, offline included; the
     * network copy then replaces it and the callback runs again if it changed.
     * aliases.json beside each index maps titles the covers are not named
     * after (mostly Redump's Japanese titles, like "Bio Hazard") to a cover.
     */
    fun ensureIndexLoaded(context: Context, paths: NeonSaturnPaths, mode: Int, onReady: () -> Unit) {
        // Register callback regardless — if already loading this replaces any stale one
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naCallback    = onReady
        else                                             japanCallback = onReady

        val state = stateFor(mode)
        if (state == IndexState.READY) { onReady(); return }
        if (state == IndexState.LOADING) return

        setState(mode, IndexState.LOADING)

        val appContext = context.applicationContext
        val subPath = subPathFor(mode)
        val savedDir = File(File(paths.root, COVERS_DIR), subPath)

        Thread {
            // The saved copy, else the one bundled in the APK
            fun <T> local(name: String, parse: (String) -> T?): T? =
                parseOrNull(parse) { File(savedDir, name).readText() }
                    ?: parseOrNull(parse) {
                        appContext.assets.open("$COVERS_DIR/${subPath}_$name").bufferedReader().use { it.readText() }
                    }

            val localIndex = local(INDEX_FILE, ::parseIndex)
            val localAliases = local(ALIASES_FILE, ::parseAliases) ?: emptyMap()
            val local = localIndex?.let { withAliases(it, localAliases) }
            if (local != null) publishIndex(mode, local)

            val remoteIndex = fetchAndSave(savedDir, subPath, INDEX_FILE, ::parseIndex)
            val remoteAliases = fetchAndSave(savedDir, subPath, ALIASES_FILE, ::parseAliases)
            val remote = (remoteIndex ?: localIndex)?.let { withAliases(it, remoteAliases ?: localAliases) }
            if (remote != null) {
                if (remote != local) publishIndex(mode, remote)
            } else if (local == null) {
                setState(mode, IndexState.FAILED)
            }
        }.start()
    }

    /* Downloads a repo file and saves it when it parses; null when offline or missing. */
    private fun <T> fetchAndSave(savedDir: File, subPath: String, name: String, parse: (String) -> T?): T? {
        val text = try {
            URL("$GITHUB_BASE/$subPath/$name").readText()
        } catch (_: Exception) {
            return null
        }
        val parsed = parseOrNull(parse) { text } ?: return null
        try {
            savedDir.mkdirs()
            val target = File(savedDir, name)
            val tmp = File(target.path + ".tmp")
            tmp.writeText(text)
            tmp.renameTo(target)
        } catch (_: IOException) {}
        return parsed
    }

    /*
     * What a cover binds to: the saved file when there is one, else the URL
     * (save it with saveForOffline once it loads), or null when the index is
     * loaded and has no match (skip request).
     */
    fun coverModel(paths: NeonSaturnPaths, entry: GameLibraryEntry, mode: Int): Any? {
        val coverTitle = coverTitleFor(entry, mode) ?: return null
        val file = savedCoverFile(paths, coverTitle, mode)
        if (file.length() > 0L) return file
        return urlFor(coverTitle, mode)
    }

    /* Saves a cover that just loaded from the network, from Coil's cache when it has it. */
    fun saveForOffline(paths: NeonSaturnPaths, entry: GameLibraryEntry, mode: Int) {
        // Only matches checked against an index; a best-effort guess may be the wrong name
        if (stateFor(mode) != IndexState.READY) return
        val coverTitle = coverTitleFor(entry, mode) ?: return
        saveCover(paths, coverTitle, mode)
    }

    /* Downloads every library game's cover to its saved file. */
    fun prefetchAll(
        context: Context,
        entries: List<GameLibraryEntry>,
        mode: Int,
        paths: NeonSaturnPaths
    ) {
        imageLoader(context, paths)
        entries.forEach { entry ->
            val coverTitle = coverTitleFor(entry, mode) ?: return@forEach
            saveCover(paths, coverTitle, mode)
        }
    }

    @OptIn(ExperimentalCoilApi::class)
    fun clearCache(paths: NeonSaturnPaths) {
        loader?.diskCache?.clear()
        loader?.memoryCache?.clear()
        naIndex = emptyMap();    naState    = IndexState.IDLE
        japanIndex = emptyMap(); japanState = IndexState.IDLE
        File(paths.root, "covers-cache").deleteRecursively()
        File(paths.root, COVERS_DIR).deleteRecursively()
    }

    @OptIn(ExperimentalCoilApi::class)
    private fun saveCover(paths: NeonSaturnPaths, coverTitle: String, mode: Int) {
        val target = savedCoverFile(paths, coverTitle, mode)
        if (target.length() > 0L || !savesInFlight.add(target.path)) return
        val url = urlFor(coverTitle, mode)
        saveExecutor.execute {
            try {
                target.parentFile?.mkdirs()
                val tmp = File(target.path + ".tmp")
                val cached = loader?.diskCache?.openSnapshot(url)
                if (cached != null) {
                    cached.use { it.data.toFile().copyTo(tmp, overwrite = true) }
                } else {
                    val connection = URL(url).openConnection() as HttpURLConnection
                    try {
                        connection.connectTimeout = 5000
                        connection.readTimeout = 10000
                        if (connection.responseCode != HttpURLConnection.HTTP_OK) return@execute
                        connection.inputStream.use { input ->
                            tmp.outputStream().use { input.copyTo(it) }
                        }
                    } finally {
                        connection.disconnect()
                    }
                }
                if (tmp.length() > 0L && tmp.renameTo(target)) return@execute
                tmp.delete()
            } catch (_: IOException) {
            } finally {
                savesInFlight.remove(target.path)
            }
        }
    }

    private fun publishIndex(mode: Int, index: Map<String, String>) {
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naIndex = index else japanIndex = index
        setState(mode, IndexState.READY)
        val cb = if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naCallback else japanCallback
        cb?.let { Handler(Looper.getMainLooper()).post(it) }
    }

    private inline fun <T> parseOrNull(parse: (String) -> T?, read: () -> String): T? = try {
        parse(read())
    } catch (_: Exception) { null }

    // Match key -> cover title
    private fun parseIndex(text: String): Map<String, String>? {
        val array = JSONArray(text)
        return HashMap<String, String>(array.length()).apply {
            repeat(array.length()) {
                val coverTitle = array.getString(it)
                val key = matchKey(coverTitle)
                if (key.isNotEmpty()) putIfAbsent(key, coverTitle)
            }
        }.takeIf { it.isNotEmpty() }
    }

    // Match key of a game's title -> cover title
    private fun parseAliases(text: String): Map<String, String> {
        val json = JSONObject(text)
        return HashMap<String, String>(json.length()).apply {
            json.keys().forEach { title ->
                val key = matchKey(title)
                if (key.isNotEmpty()) put(key, json.getString(title))
            }
        }
    }

    // A cover named for the title still wins over an alias, and aliases to covers the index lacks are dropped
    private fun withAliases(index: Map<String, String>, aliases: Map<String, String>): Map<String, String> {
        if (aliases.isEmpty()) return index
        val titles = index.values.toHashSet()
        return HashMap(index).apply {
            aliases.forEach { (key, coverTitle) -> if (coverTitle in titles) putIfAbsent(key, coverTitle) }
        }
    }

    // The Redump name from the disc's hash when there is one, else the file's name
    private fun coverTitleFor(entry: GameLibraryEntry, mode: Int): String? {
        val redumpName = DiscIdentifier.redumpName(entry)
        if (stateFor(mode) != IndexState.READY) {
            return normalizeTitle(redumpName ?: entry.title)   // index not ready yet — best-effort
        }
        val index = indexFor(mode)
        return redumpName?.let { findMatch(it, index) } ?: findMatch(entry.title, index)
    }

    private fun urlFor(coverTitle: String, mode: Int) =
        "$GITHUB_BASE/${subPathFor(mode)}/${Uri.encode(coverTitle)}.png"

    private fun savedCoverFile(paths: NeonSaturnPaths, coverTitle: String, mode: Int) =
        File(File(File(paths.root, COVERS_DIR), subPathFor(mode)),
            UNSAFE_FILE_CHARS.replace(coverTitle, "_") + ".png")

    // Strip trailing (USA), (Japan), (Disc 1), (Rev A), … groups one at a time
    private fun normalizeTitle(title: String): String {
        var t = title.replace('/', '-').trim()
        while (TRAILING_PARENS.containsMatchIn(t)) {
            t = TRAILING_PARENS.replace(t, "").trim()
        }
        return t
    }

    /*
     * Covers are named like Redump titles with the punctuation dropped
     * ("Albert Odyssey Legend of Eldean" for "Albert Odyssey - Legend of
     * Eldean (USA)"), so both sides compare by matchKey. Failing that, drop
     * " - Subtitle" parts from the end: "Clockwork Knight - Pepperouchau's
     * Adventure" has the "Clockwork Knight" cover.
     */
    private fun findMatch(title: String, index: Map<String, String>): String? {
        index[matchKey(title)]?.let { return it }
        val parts = BRACKETED.replace(title, "").split(" - ")
        for (count in parts.size - 1 downTo 1) {
            index[matchKey(parts.take(count).joinToString(" - "))]?.let { return it }
        }
        return null
    }

    // "Lost World, The - Jurassic Park (USA)" -> "lostworldjurassicpark"
    private fun matchKey(title: String): String {
        var t = BRACKETED.replace(title, "").trim()
        TRAILING_ARTICLE.matchEntire(t)?.let { t = "${it.groupValues[2]} ${it.groupValues[1]}${it.groupValues[3]}" }
        t = Normalizer.normalize(t, Normalizer.Form.NFKD).lowercase(Locale.ROOT).replace("&", " and ")
        return NON_ALPHANUMERIC.replace(t.removePrefix("the "), "")
    }

    private fun subPathFor(mode: Int) =
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) NA_PATH else JAPAN_PATH

    private fun indexFor(mode: Int) =
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naIndex else japanIndex

    private fun stateFor(mode: Int) =
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naState else japanState

    private fun setState(mode: Int, state: IndexState) {
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naState    = state
        else                                             japanState = state
    }
}
