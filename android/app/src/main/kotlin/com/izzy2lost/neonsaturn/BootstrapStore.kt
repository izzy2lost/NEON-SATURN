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

    fun clearSetup() {
        preferences.edit()
            .remove(KEY_IPL_PATH)
            .remove(KEY_CDB_PATH)
            .remove(KEY_GAMES_FOLDER_URI)
            .apply()
    }

    fun saveAspectRatio(value: String) {
        preferences.edit().putString(KEY_ASPECT_RATIO, value).apply()
    }

    fun loadAspectRatio(): String =
        preferences.getString(KEY_ASPECT_RATIO, ASPECT_4_3) ?: ASPECT_4_3

    fun saveTextureFilter(value: String) {
        preferences.edit().putString(KEY_TEXTURE_FILTER, value).apply()
    }

    fun loadTextureFilter(): String =
        preferences.getString(KEY_TEXTURE_FILTER, FILTER_NEAREST) ?: FILTER_NEAREST

    fun saveTouchControlsEnabled(enabled: Boolean) {
        preferences.edit().putBoolean(KEY_TOUCH_CONTROLS_ENABLED, enabled).apply()
    }

    fun loadTouchControlsEnabled(): Boolean =
        preferences.getBoolean(KEY_TOUCH_CONTROLS_ENABLED, true)

    fun loadTouchControlsLayout(): TouchControlsLayout =
        TouchControlsLayout(
            TouchControlId.values().associateWith { controlId ->
                val defaultPlacement = TouchControlsLayout.DEFAULT.placementFor(controlId)
                TouchControlPlacement(
                    x = preferences.getFloat(
                        "${KEY_TOUCH_CONTROLS_POSITION_PREFIX}${controlId.preferenceKey}_x",
                        defaultPlacement.x,
                    ),
                    y = preferences.getFloat(
                        "${KEY_TOUCH_CONTROLS_POSITION_PREFIX}${controlId.preferenceKey}_y",
                        defaultPlacement.y,
                    ),
                )
            }
        )

    fun saveTouchControlsLayout(layout: TouchControlsLayout) {
        preferences.edit().apply {
            TouchControlId.values().forEach { controlId ->
                val placement = layout.placementFor(controlId)
                putFloat("${KEY_TOUCH_CONTROLS_POSITION_PREFIX}${controlId.preferenceKey}_x", placement.x)
                putFloat("${KEY_TOUCH_CONTROLS_POSITION_PREFIX}${controlId.preferenceKey}_y", placement.y)
            }
        }.apply()
    }

    fun resetTouchControlsLayout() {
        preferences.edit().apply {
            TouchControlId.values().forEach { controlId ->
                remove("${KEY_TOUCH_CONTROLS_POSITION_PREFIX}${controlId.preferenceKey}_x")
                remove("${KEY_TOUCH_CONTROLS_POSITION_PREFIX}${controlId.preferenceKey}_y")
            }
        }.apply()
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

    companion object {
        const val ASPECT_4_3 = "4:3"
        const val ASPECT_16_9 = "16:9"
        const val ASPECT_STRETCH = "stretch"

        const val FILTER_NEAREST = "nearest"
        const val FILTER_BILINEAR = "bilinear"

        private const val KEY_IPL_PATH = "ipl_path"
        private const val KEY_CDB_PATH = "cdb_path"
        private const val KEY_DISC_PATH = "disc_path"
        private const val KEY_GAMES_FOLDER_URI = "games_folder_uri"
        private const val KEY_ASPECT_RATIO = "aspect_ratio"
        private const val KEY_TEXTURE_FILTER = "texture_filter"
        private const val KEY_TOUCH_CONTROLS_ENABLED = "touch_controls_enabled"
        private const val KEY_TOUCH_CONTROLS_POSITION_PREFIX = "touch_controls_position_"
    }
}
