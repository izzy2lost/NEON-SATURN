package com.izzy2lost.neonsaturn

import android.content.Context
import java.io.File

data class StoredLaunchSelection(
    val iplPath: String? = null,
    val cdbPath: String? = null,
    val discPath: String? = null,
    val gamesFolderUri: String? = null
)

class BootstrapStore(context: Context) {
    private val preferences =
        context.getSharedPreferences("neonsaturn.bootstrap", Context.MODE_PRIVATE)

    fun load(): StoredLaunchSelection =
        StoredLaunchSelection(
            iplPath = preferences.getString(KEY_IPL_PATH, null),
            cdbPath = preferences.getString(KEY_CDB_PATH, null),
            discPath = preferences.getString(KEY_DISC_PATH, null),
            gamesFolderUri = preferences.getString(KEY_GAMES_FOLDER_URI, null)
        )

    fun save(selection: StoredLaunchSelection) {
        preferences.edit()
            .putString(KEY_IPL_PATH, selection.iplPath)
            .putString(KEY_CDB_PATH, selection.cdbPath)
            .putString(KEY_DISC_PATH, selection.discPath)
            .putString(KEY_GAMES_FOLDER_URI, selection.gamesFolderUri)
            .apply()
    }

    fun saveIpl(path: String?) {
        preferences.edit().putString(KEY_IPL_PATH, path).apply()
    }

    fun saveCdb(path: String?) {
        preferences.edit().putString(KEY_CDB_PATH, path).apply()
    }

    fun saveDisc(path: String?) {
        preferences.edit().putString(KEY_DISC_PATH, path).apply()
    }

    fun saveGamesFolderUri(uri: String?) {
        preferences.edit().putString(KEY_GAMES_FOLDER_URI, uri).apply()
    }

    fun migratePaths(fromRoot: File, toRoot: File) {
        val current = load()
        val migrated = StoredLaunchSelection(
            iplPath = remapIfUnderRoot(current.iplPath, fromRoot, toRoot),
            cdbPath = remapIfUnderRoot(current.cdbPath, fromRoot, toRoot),
            discPath = remapIfUnderRoot(current.discPath, fromRoot, toRoot)
        )
        if (migrated != current) {
            save(migrated)
        }
    }

    private fun remapIfUnderRoot(path: String?, fromRoot: File, toRoot: File): String? {
        path ?: return null
        val relativePath = runCatching {
            File(path).relativeTo(fromRoot).path
        }.getOrNull() ?: return path
        return File(toRoot, relativePath).absolutePath
    }

    private companion object {
        private const val KEY_IPL_PATH = "ipl_path"
        private const val KEY_CDB_PATH = "cdb_path"
        private const val KEY_DISC_PATH = "disc_path"
        private const val KEY_GAMES_FOLDER_URI = "games_folder_uri"
    }
}
