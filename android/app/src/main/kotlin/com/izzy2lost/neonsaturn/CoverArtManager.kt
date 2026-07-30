package com.izzy2lost.neonsaturn

import android.net.Uri
import android.os.Handler
import android.os.Looper
import coil.ImageLoader
import coil.annotation.ExperimentalCoilApi
import coil.disk.DiskCache
import coil.memory.MemoryCache
import coil.request.CachePolicy
import coil.request.ImageRequest
import org.json.JSONArray
import java.io.File
import java.net.URL

object CoverArtManager {
    // Update if the repo ever moves
    private const val GITHUB_BASE = "https://raw.githubusercontent.com/izzy2lost/saturn-covers/main"
    private const val NA_PATH = "na"
    private const val JAPAN_PATH = "japan"

    // Trailing parenthetical groups ROM tools add: (USA), (Japan), (Disc 1), (Rev A), etc.
    private val TRAILING_PARENS = Regex("""\s*\([^)]+\)\s*$""")

    @Volatile private var loader: ImageLoader? = null

    private val naIndex   = HashSet<String>()
    private val japanIndex = HashSet<String>()

    private enum class IndexState { IDLE, LOADING, READY, FAILED }
    @Volatile private var naState    = IndexState.IDLE
    @Volatile private var japanState = IndexState.IDLE

    // Single pending callback per mode — only LauncherActivity ever registers one at a time
    @Volatile private var naCallback:    (() -> Unit)? = null
    @Volatile private var japanCallback: (() -> Unit)? = null

    fun imageLoader(context: android.content.Context, paths: NeonSaturnPaths): ImageLoader =
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
                .build()
                .also { loader = it }
        }

    fun ensureIndexLoaded(mode: Int, onReady: () -> Unit) {
        val state = stateFor(mode)
        if (state == IndexState.READY) { onReady(); return }

        // Register callback regardless — if already loading this replaces any stale one
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naCallback    = onReady
        else                                             japanCallback = onReady

        if (state == IndexState.LOADING) return

        setState(mode, IndexState.LOADING)

        val subPath = if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) NA_PATH else JAPAN_PATH
        val indexUrl = "$GITHUB_BASE/$subPath/index.json"

        Thread {
            val success = try {
                val text  = URL(indexUrl).readText()
                val array = JSONArray(text)
                val set   = indexFor(mode)
                synchronized(set) {
                    set.clear()
                    repeat(array.length()) { set.add(array.getString(it)) }
                }
                true
            } catch (_: Exception) { false }

            setState(mode, if (success) IndexState.READY else IndexState.FAILED)

            if (success) {
                val cb = if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naCallback else japanCallback
                cb?.let { Handler(Looper.getMainLooper()).post(it) }
            }
        }.start()
    }

    // Returns a fully-formed URL, or null if the index is loaded and has no match (skip request)
    fun coverUrl(title: String, mode: Int): String? {
        val subPath = if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) NA_PATH else JAPAN_PATH
        val index   = indexFor(mode)
        val state   = stateFor(mode)

        val coverTitle = when {
            state == IndexState.READY -> findMatch(title, index) ?: return null
            else -> normalizeTitle(title)   // index not ready yet — best-effort
        }

        return "$GITHUB_BASE/$subPath/${Uri.encode(coverTitle)}.png"
    }

    fun prefetchAll(
        context: android.content.Context,
        entries: List<GameLibraryEntry>,
        mode: Int,
        paths: NeonSaturnPaths
    ) {
        val il = imageLoader(context, paths)
        entries.forEach { entry ->
            val url = coverUrl(entry.title, mode) ?: return@forEach
            il.enqueue(
                ImageRequest.Builder(context)
                    .data(url)
                    .memoryCachePolicy(CachePolicy.DISABLED)
                    .diskCachePolicy(CachePolicy.ENABLED)
                    .build()
            )
        }
    }

    @OptIn(ExperimentalCoilApi::class)
    fun clearCache(paths: NeonSaturnPaths) {
        loader?.diskCache?.clear()
        loader?.memoryCache?.clear()
        naIndex.clear();   naState    = IndexState.IDLE
        japanIndex.clear(); japanState = IndexState.IDLE
        File(paths.root, "covers-cache").deleteRecursively()
    }

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

    private fun indexFor(mode: Int) =
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naIndex else japanIndex

    private fun stateFor(mode: Int) =
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naState else japanState

    private fun setState(mode: Int, state: IndexState) {
        if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) naState    = state
        else                                             japanState = state
    }
}
