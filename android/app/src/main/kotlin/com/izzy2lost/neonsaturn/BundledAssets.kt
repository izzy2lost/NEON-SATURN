package com.izzy2lost.neonsaturn

import android.content.Context
import java.io.File
import java.io.IOException

private const val GAME_CONTROLLER_DB_ASSET = "gamecontrollerdb.txt"

fun Context.installBundledGameControllerDb(paths: NeonSaturnPaths = neonSaturnPaths()): File {
    paths.ensureAll()

    val target = File(paths.supportDir, GAME_CONTROLLER_DB_ASSET)
    val staging = File(paths.supportDir, "$GAME_CONTROLLER_DB_ASSET.tmp")

    assets.open(GAME_CONTROLLER_DB_ASSET).use { input ->
        staging.outputStream().use { output ->
            input.copyTo(output)
        }
    }

    if (target.exists() && !target.delete()) {
        staging.delete()
        throw IOException("Unable to replace the bundled controller database")
    }

    if (!staging.renameTo(target)) {
        staging.delete()
        throw IOException("Unable to install the bundled controller database")
    }

    return target
}
