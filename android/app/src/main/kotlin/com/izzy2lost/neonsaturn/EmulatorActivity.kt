package com.izzy2lost.neonsaturn

import android.content.Context
import android.content.Intent
import org.libsdl.app.SDLActivity

class EmulatorActivity : SDLActivity() {
    override fun getArguments(): Array<String> {
        val arguments = mutableListOf<String>()
        intent.getStringExtra(EXTRA_IPL_PATH)?.let { arguments += "--ipl=$it" }
        intent.getStringExtra(EXTRA_DISC_PATH)?.let { arguments += "--disc=$it" }
        intent.getStringExtra(EXTRA_DATA_ROOT)?.let { arguments += "--data-root=$it" }
        intent.getStringExtra(EXTRA_CDB_PATH)?.let { arguments += "--cdb=$it" }
        return arguments.toTypedArray()
    }

    companion object {
        private const val EXTRA_IPL_PATH = "com.izzy2lost.neonsaturn.extra.IPL_PATH"
        private const val EXTRA_CDB_PATH = "com.izzy2lost.neonsaturn.extra.CDB_PATH"
        private const val EXTRA_DISC_PATH = "com.izzy2lost.neonsaturn.extra.DISC_PATH"
        private const val EXTRA_DATA_ROOT = "com.izzy2lost.neonsaturn.extra.DATA_ROOT"

        fun createIntent(
            context: Context,
            selection: StoredLaunchSelection,
            paths: NeonSaturnPaths
        ): Intent =
            Intent(context, EmulatorActivity::class.java).apply {
                putExtra(EXTRA_IPL_PATH, selection.iplPath)
                putExtra(EXTRA_CDB_PATH, selection.cdbPath)
                putExtra(EXTRA_DISC_PATH, selection.discPath)
                putExtra(EXTRA_DATA_ROOT, paths.root.absolutePath)
            }
    }
}
