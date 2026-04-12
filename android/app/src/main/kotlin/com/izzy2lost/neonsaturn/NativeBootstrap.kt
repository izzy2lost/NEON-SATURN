package com.izzy2lost.neonsaturn

object NativeBootstrap {
    init {
        System.loadLibrary("SDL3")
        System.loadLibrary("main")
    }

    fun configure(selection: StoredLaunchSelection, paths: NeonSaturnPaths) {
        val iplPath = requireNotNull(selection.iplPath) { "IPL path is required" }
        val discPath = requireNotNull(selection.discPath) { "Disc path is required" }

        nativeConfigure(
            iplPath = iplPath,
            cdbPath = selection.cdbPath,
            discPath = discPath,
            dataRoot = paths.root.absolutePath
        )
    }

    @JvmStatic
    private external fun nativeConfigure(
        iplPath: String,
        cdbPath: String?,
        discPath: String,
        dataRoot: String
    )
}

