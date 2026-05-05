package com.izzy2lost.neonsaturn

import android.content.Context
import android.net.Uri
import coil.ImageLoader
import coil.disk.DiskCache
import coil.memory.MemoryCache
import coil.request.ImageRequest
import coil.request.CachePolicy
import java.io.File

object CoverArtManager {
    // Update these once your covers repo is live on GitHub
    private const val GITHUB_BASE = "https://raw.githubusercontent.com/izzy2lost/saturn-covers/main"
    private const val NA_PATH = "na"
    private const val JAPAN_PATH = "japan"

    @Volatile private var loader: ImageLoader? = null

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
                .build()
                .also { loader = it }
        }

    fun coverUrl(title: String, mode: Int): String {
        val subPath = if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) NA_PATH else JAPAN_PATH
        val encoded = Uri.encode(title)
        return "$GITHUB_BASE/$subPath/$encoded.png"
    }

    fun prefetchAll(
        context: Context,
        entries: List<GameLibraryEntry>,
        mode: Int,
        paths: NeonSaturnPaths
    ) {
        val il = imageLoader(context, paths)
        entries.forEach { entry ->
            val req = ImageRequest.Builder(context)
                .data(coverUrl(entry.title, mode))
                .memoryCachePolicy(CachePolicy.DISABLED)
                .diskCachePolicy(CachePolicy.ENABLED)
                .build()
            il.enqueue(req)
        }
    }

    fun clearCache(paths: NeonSaturnPaths) {
        loader?.diskCache?.clear()
        loader?.memoryCache?.clear()
        File(paths.root, "covers-cache").deleteRecursively()
    }
}
