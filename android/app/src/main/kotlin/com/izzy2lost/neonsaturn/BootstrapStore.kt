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

    fun saveResolutionScale(value: Int) {
        preferences.edit().putInt(KEY_RESOLUTION_SCALE, value.coerceIn(RESOLUTION_SCALE_1X, RESOLUTION_SCALE_8X)).apply()
    }

    fun loadResolutionScale(): Int =
        preferences.getInt(KEY_RESOLUTION_SCALE, RESOLUTION_SCALE_1X)
            .coerceIn(RESOLUTION_SCALE_1X, RESOLUTION_SCALE_8X)

    fun saveDeinterlaceEnabled(enabled: Boolean) {
        preferences.edit().putBoolean(KEY_DEINTERLACE_ENABLED, enabled).apply()
    }

    fun loadDeinterlaceEnabled(): Boolean =
        preferences.getBoolean(KEY_DEINTERLACE_ENABLED, false)

    fun saveTransparentMeshesEnabled(enabled: Boolean) {
        preferences.edit().putBoolean(KEY_TRANSPARENT_MESHES_ENABLED, enabled).apply()
    }

    fun loadTransparentMeshesEnabled(): Boolean =
        preferences.getBoolean(KEY_TRANSPARENT_MESHES_ENABLED, false)

    fun saveLibraryViewMode(mode: Int) {
        preferences.edit().putInt(KEY_LIBRARY_VIEW_MODE, mode).apply()
    }

    fun loadLibraryViewMode(): Int =
        preferences.getInt(KEY_LIBRARY_VIEW_MODE, VIEW_MODE_LIST)

    fun saveTouchControlsEnabled(enabled: Boolean) {
        preferences.edit().putBoolean(KEY_TOUCH_CONTROLS_ENABLED, enabled).apply()
    }

    fun loadTouchControlsEnabled(): Boolean =
        preferences.getBoolean(KEY_TOUCH_CONTROLS_ENABLED, true)

    fun loadTouchControlsLayout(): TouchControlsLayout =
        loadTouchControlsLayoutWithDefaults(KEY_TOUCH_CONTROLS_POSITION_PREFIX, TouchControlsLayout.DEFAULT)

    fun saveTouchControlsLayout(layout: TouchControlsLayout) {
        saveTouchControlsLayoutWithPrefix(KEY_TOUCH_CONTROLS_POSITION_PREFIX, layout)
    }

    fun resetTouchControlsLayout() {
        resetTouchControlsLayoutWithPrefix(KEY_TOUCH_CONTROLS_POSITION_PREFIX)
    }

    fun loadTouchControlsLayoutPortrait(): TouchControlsLayout =
        loadTouchControlsLayoutWithDefaults(KEY_TOUCH_CONTROLS_PORTRAIT_PREFIX, TouchControlsLayout.DEFAULT_PORTRAIT)

    fun saveTouchControlsLayoutPortrait(layout: TouchControlsLayout) {
        saveTouchControlsLayoutWithPrefix(KEY_TOUCH_CONTROLS_PORTRAIT_PREFIX, layout)
    }

    fun resetTouchControlsLayoutPortrait() {
        resetTouchControlsLayoutWithPrefix(KEY_TOUCH_CONTROLS_PORTRAIT_PREFIX)
    }

    private fun loadTouchControlsLayoutWithDefaults(prefix: String, defaults: TouchControlsLayout): TouchControlsLayout =
        TouchControlsLayout(
            TouchControlId.values().associateWith { controlId ->
                val defaultPlacement = defaults.placementFor(controlId)
                TouchControlPlacement(
                    x = preferences.getFloat("${prefix}${controlId.preferenceKey}_x", defaultPlacement.x),
                    y = preferences.getFloat("${prefix}${controlId.preferenceKey}_y", defaultPlacement.y),
                )
            }
        )

    private fun saveTouchControlsLayoutWithPrefix(prefix: String, layout: TouchControlsLayout) {
        preferences.edit().apply {
            TouchControlId.values().forEach { controlId ->
                val placement = layout.placementFor(controlId)
                putFloat("${prefix}${controlId.preferenceKey}_x", placement.x)
                putFloat("${prefix}${controlId.preferenceKey}_y", placement.y)
            }
        }.apply()
    }

    private fun resetTouchControlsLayoutWithPrefix(prefix: String) {
        preferences.edit().apply {
            TouchControlId.values().forEach { controlId ->
                remove("${prefix}${controlId.preferenceKey}_x")
                remove("${prefix}${controlId.preferenceKey}_y")
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

        const val RESOLUTION_SCALE_1X = 1
        const val RESOLUTION_SCALE_2X = 2
        const val RESOLUTION_SCALE_3X = 3
        const val RESOLUTION_SCALE_4X = 4
        const val RESOLUTION_SCALE_5X = 5
        const val RESOLUTION_SCALE_6X = 6
        const val RESOLUTION_SCALE_7X = 7
        const val RESOLUTION_SCALE_8X = 8

        private const val KEY_IPL_PATH = "ipl_path"
        private const val KEY_CDB_PATH = "cdb_path"
        private const val KEY_DISC_PATH = "disc_path"
        private const val KEY_GAMES_FOLDER_URI = "games_folder_uri"
        private const val KEY_ASPECT_RATIO = "aspect_ratio"
        private const val KEY_TEXTURE_FILTER = "texture_filter"
        private const val KEY_RESOLUTION_SCALE = "resolution_scale"
        private const val KEY_DEINTERLACE_ENABLED = "deinterlace_enabled"
        private const val KEY_TRANSPARENT_MESHES_ENABLED = "transparent_meshes_enabled"
        const val VIEW_MODE_LIST = 0
        const val VIEW_MODE_NA_COVERS = 1
        const val VIEW_MODE_JAPAN_COVERS = 2

        private const val KEY_LIBRARY_VIEW_MODE = "library_view_mode"
        private const val KEY_TOUCH_CONTROLS_ENABLED = "touch_controls_enabled"
        private const val KEY_TOUCH_CONTROLS_POSITION_PREFIX = "touch_controls_position_"
        private const val KEY_TOUCH_CONTROLS_PORTRAIT_PREFIX = "touch_controls_portrait_position_"
    }
}
