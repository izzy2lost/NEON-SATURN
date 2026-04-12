package com.izzy2lost.neonsaturn

import android.content.Context
import java.io.File

private const val NEON_SATURN_ROOT_DIR = "neonsaturn"

data class NeonSaturnPaths(
    val root: File,
    val iplDir: File,
    val cdbDir: File,
    val discDir: File,
    val stateDir: File,
    val savesDir: File
) {
    fun ensureAll() {
        listOf(root, iplDir, cdbDir, discDir, stateDir, savesDir).forEach { directory ->
            if (!directory.exists()) {
                directory.mkdirs()
            }
        }
    }
}

fun Context.neonSaturnPaths(): NeonSaturnPaths {
    val baseDir = getExternalFilesDir(null) ?: filesDir
    return buildNeonSaturnPaths(baseDir)
}

fun Context.legacyNeonSaturnPaths(): NeonSaturnPaths =
    buildNeonSaturnPaths(filesDir)

private fun buildNeonSaturnPaths(baseDir: File): NeonSaturnPaths {
    val root = File(baseDir, NEON_SATURN_ROOT_DIR)
    return NeonSaturnPaths(
        root = root,
        iplDir = File(root, "ipl"),
        cdbDir = File(root, "cdb"),
        discDir = File(root, "disc"),
        stateDir = File(root, "state"),
        savesDir = File(root, "saves")
    )
}
