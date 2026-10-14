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
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
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

    // Trailing parenthetical groups ROM tools add: (USA), (Japan), (Disc 1), (Rev A), etc.
    private val TRAILING_PARENS = Regex("""\s*\([^)]+\)\s*$""")
    private val UNSAFE_FILE_CHARS = Regex("""[\\/:*?"<>|]""")

    @Volatile private var loader: ImageLoader? = null

    // Replaced whole, never mutated, so binds on the UI thread can read without locking
    @Volatile private var naIndex: Set<String> = emptySet()
    @Volatile private var japanIndex: Set<String> = emptySet()

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
        val savedIndex = File(File(File(paths.root, COVERS_DIR), subPath), INDEX_FILE)

        Thread {
            val local = parseIndex { savedIndex.readText() }
                ?: parseIndex { appContext.assets.open("$COVERS_DIR/${subPath}_index.json").bufferedReader().use { it.readText() } }
            if (local != null) publishIndex(mode, local)

            var remoteText: String? = null
            val remote = parseIndex { URL("$GITHUB_BASE/$subPath/$INDEX_FILE").readText().also { remoteText = it } }
            if (remote != null) {
                try {
                    savedIndex.parentFile?.mkdirs()
                    val tmp = File(savedIndex.path + ".tmp")
                    tmp.writeText(remoteText!!)
                    tmp.renameTo(savedIndex)
                } catch (_: IOException) {}
                if (remote != local) publishIndex(mode, remote)
            } else if (local == null) {
                setState(mode, IndexState.FAILED)
            }
        }.start()
    }

    /*
     * What a cover binds to: the saved file when there is one, else the URL
     * (save it with saveForOffline once it loads), or null when the index is
     * loaded and has no match (skip request).
     */
    fun coverModel(paths: NeonSaturnPaths, title: String, mode: Int): Any? {
        val coverTitle = coverTitleFor(title, mode) ?: return null
        val file = savedCoverFile(paths, coverTitle, mode)
        if (file.length() > 0L) return file
        return urlFor(coverTitle, mode)
    }

    /* Saves a cover that just loaded from the network, from Coil's cache when it has it. */
    fun saveForOffline(paths: NeonSaturnPaths, title: String, mode: Int) {
        // Only matches checked against an index; a best-effort guess may be the wrong name
        if (stateFor(mode) != IndexState.READY) return
        val coverTitle = coverTitleFor(title, mode) ?: return
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
            val coverTitle = coverTitleFor(entry.title, mode) ?: return@forEach
            saveCover(paths, coverTitle, mode)
        }
    }

    @OptIn(ExperimentalCoilApi::class)
    fun clearCache(paths: NeonSaturnPaths) {
        loader?.diskCache?.clear()
        loader?.memoryCache?.clear()
        naIndex = emptySet();    naState    = IndexState.IDLE
        japanIndex = emptySet(); japanState = IndexState.IDLE
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

    private fun publishIndex(mode: Int, index: Set<String>) {
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naIndex = index else japanIndex = index
        setState(mode, IndexState.READY)
        val cb = if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naCallback else japanCallback
        cb?.let { Handler(Looper.getMainLooper()).post(it) }
    }

    private inline fun parseIndex(read: () -> String): Set<String>? = try {
        val array = JSONArray(read())
        HashSet<String>(array.length()).apply {
            repeat(array.length()) { add(array.getString(it)) }
        }.takeIf { it.isNotEmpty() }
    } catch (_: Exception) { null }

    private fun coverTitleFor(title: String, mode: Int): String? =
        if (stateFor(mode) == IndexState.READY) findMatch(title, indexFor(mode))
        else normalizeTitle(title)   // index not ready yet — best-effort

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

    private fun findMatch(title: String, index: Set<String>): String? {
        var t = title.replace('/', '-').trim()
        if (index.contains(t)) return t
        while (TRAILING_PARENS.containsMatchIn(t)) {
            t = TRAILING_PARENS.replace(t, "").trim()
            if (index.contains(t)) return t
        }
        return null
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
