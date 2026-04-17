package com.izzy2lost.neonsaturn

import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.libsdl.app.SDLActivity

class EmulatorActivity : SDLActivity() {
    private lateinit var store: BootstrapStore
    private var quickActionsDialog: AlertDialog? = null
    private var touchControlsView: TouchControlsView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = BootstrapStore(this)
        updateCurrentInstance(this)
        refreshTouchControlsOverlay()
    }

    override fun onResume() {
        super.onResume()
        updateCurrentInstance(this)
        refreshTouchControlsOverlay()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        refreshTouchControlsOverlay()
    }

    override fun onPause() {
        suspendTouchControls()
        if (quickActionsDialog?.isShowing == true) {
            quickActionsDialog?.dismiss()
        }
        super.onPause()
    }

    override fun onDestroy() {
        if (quickActionsDialog?.isShowing == true) {
            quickActionsDialog?.dismiss()
        }
        removeTouchControlsOverlay()
        if (currentInstance === this) {
            updateCurrentInstance(null)
        }
        super.onDestroy()
    }

    @Suppress("DEPRECATION")
    override fun onBackPressed() {
        showQuickActionsDialog()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (isQuickActionsKey(event)) {
            if (quickActionsDialog?.isShowing == true) {
                if (event.action == KeyEvent.ACTION_UP) {
                    quickActionsDialog?.dismiss()
                }
                return true
            }

            if (event.action == KeyEvent.ACTION_UP && event.repeatCount == 0) {
                showQuickActionsDialog()
            }
            return true
        }

        return super.dispatchKeyEvent(event)
    }

    override fun getArguments(): Array<String> {
        val arguments = mutableListOf<String>()
        intent.getStringExtra(EXTRA_IPL_PATH)?.let { arguments += "--ipl=$it" }
        intent.getStringExtra(EXTRA_DISC_PATH)?.let { arguments += "--disc=$it" }
        intent.getStringExtra(EXTRA_DATA_ROOT)?.let { arguments += "--data-root=$it" }
        intent.getStringExtra(EXTRA_CDB_PATH)?.let { arguments += "--cdb=$it" }
        intent.getStringExtra(EXTRA_GAME_CONTROLLER_DB_PATH)?.let { arguments += "--gamecontrollerdb=$it" }
        intent.getStringExtra(EXTRA_ASPECT_RATIO)?.let { arguments += "--aspect-ratio=$it" }
        intent.getStringExtra(EXTRA_TEXTURE_FILTER)?.let { arguments += "--texture-filter=$it" }
        return arguments.toTypedArray()
    }

    private fun showQuickActionsDialog() {
        if (isFinishing || isDestroyed || quickActionsDialog?.isShowing == true) {
            return
        }

        nativeSetPaused(true)
        suspendTouchControls()

        val content = LayoutInflater.from(this).inflate(R.layout.dialog_quick_actions, null, false)
        content.findViewById<TextView>(R.id.quickActionsSubtitleText).text =
            intent.getStringExtra(EXTRA_DISC_PATH)?.substringAfterLast('/')
                ?.substringAfterLast('\\')
                ?: getString(R.string.quick_actions_default_subtitle)

        var resumeOnDismiss = true

        val dialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_NeonSaturn_QuickActionsDialog)
            .setView(content)
            .setCancelable(true)
            .create()

        content.findViewById<MaterialButton>(R.id.resumeButton).setOnClickListener {
            dialog.dismiss()
        }
        content.findViewById<MaterialButton>(R.id.saveStateButton).setOnClickListener {
            Toast.makeText(this, nativeSaveState(0), Toast.LENGTH_SHORT).show()
            dialog.dismiss()
        }
        content.findViewById<MaterialButton>(R.id.loadStateButton).setOnClickListener {
            Toast.makeText(this, nativeLoadState(0), Toast.LENGTH_SHORT).show()
            dialog.dismiss()
        }
        content.findViewById<MaterialButton>(R.id.exitGameButton).setOnClickListener {
            resumeOnDismiss = false
            nativeExitEmulator()
            dialog.dismiss()
        }

        dialog.setOnDismissListener {
            quickActionsDialog = null
            if (resumeOnDismiss) {
                nativeSetPaused(false)
                resumeTouchControls()
            }
        }

        quickActionsDialog = dialog
        dialog.show()
    }

    private fun isQuickActionsKey(event: KeyEvent): Boolean =
        when (event.keyCode) {
            KeyEvent.KEYCODE_BACK,
            KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_ESCAPE -> true
            else -> false
        }

    private fun refreshTouchControlsOverlay() {
        if (!store.loadTouchControlsEnabled()) {
            removeTouchControlsOverlay()
            pushTouchControlsState(0, 0, 0, 0f, 0f)
            return
        }

        val overlay = touchControlsView ?: TouchControlsView(this).also { view ->
            view.interactionMode = TouchControlsView.InteractionMode.PLAY
            view.onStateChanged = { state ->
                pushTouchControlsState(
                    state.buttonMask,
                    state.dpadX,
                    state.dpadY,
                    state.analogX,
                    state.analogY,
                )
            }
            view.onMenuPressed = {
                showQuickActionsDialog()
            }
            addContentView(
                view,
                ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT,
                ),
            )
            touchControlsView = view
        }

        val isPortrait = resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        overlay.touchControlsLayout = if (isPortrait) {
            store.loadTouchControlsLayoutPortrait()
        } else {
            store.loadTouchControlsLayout()
        }
        overlay.inputSuspended = quickActionsDialog?.isShowing == true
    }

    private fun removeTouchControlsOverlay() {
        touchControlsView?.resetRuntimeState()
        (touchControlsView?.parent as? ViewGroup)?.removeView(touchControlsView)
        touchControlsView = null
    }

    private fun suspendTouchControls() {
        touchControlsView?.inputSuspended = true
        pushTouchControlsState(0, 0, 0, 0f, 0f)
    }

    private fun resumeTouchControls() {
        touchControlsView?.inputSuspended = false
    }

    private fun pushTouchControlsState(
        buttonMask: Int,
        dpadX: Int,
        dpadY: Int,
        analogX: Float,
        analogY: Float,
    ) {
        runCatching {
            nativeUpdateTouchControls(buttonMask, dpadX, dpadY, analogX, analogY)
        }
    }

    private external fun nativeSetPaused(paused: Boolean)
    private external fun nativeSaveState(slotIndex: Int): String
    private external fun nativeLoadState(slotIndex: Int): String
    private external fun nativeExitEmulator()
    private external fun nativeUpdateTouchControls(
        buttonMask: Int,
        dpadX: Int,
        dpadY: Int,
        analogX: Float,
        analogY: Float,
    )

    companion object {
        @Volatile
        private var currentInstance: EmulatorActivity? = null

        private const val EXTRA_IPL_PATH = "com.izzy2lost.neonsaturn.extra.IPL_PATH"
        private const val EXTRA_CDB_PATH = "com.izzy2lost.neonsaturn.extra.CDB_PATH"
        private const val EXTRA_DISC_PATH = "com.izzy2lost.neonsaturn.extra.DISC_PATH"
        private const val EXTRA_DATA_ROOT = "com.izzy2lost.neonsaturn.extra.DATA_ROOT"
        private const val EXTRA_GAME_CONTROLLER_DB_PATH =
            "com.izzy2lost.neonsaturn.extra.GAME_CONTROLLER_DB_PATH"
        private const val EXTRA_ASPECT_RATIO =
            "com.izzy2lost.neonsaturn.extra.ASPECT_RATIO"
        private const val EXTRA_TEXTURE_FILTER =
            "com.izzy2lost.neonsaturn.extra.TEXTURE_FILTER"

        fun createIntent(
            context: Context,
            selection: StoredLaunchSelection,
            paths: NeonSaturnPaths,
            gameControllerDbPath: String? = null,
            aspectRatio: String = BootstrapStore.ASPECT_4_3,
            textureFilter: String = BootstrapStore.FILTER_NEAREST
        ): Intent =
            Intent(context, EmulatorActivity::class.java).apply {
                putExtra(EXTRA_IPL_PATH, selection.iplPath)
                putExtra(EXTRA_CDB_PATH, selection.cdbPath)
                putExtra(EXTRA_DISC_PATH, selection.discPath)
                putExtra(EXTRA_DATA_ROOT, paths.root.absolutePath)
                putExtra(EXTRA_GAME_CONTROLLER_DB_PATH, gameControllerDbPath)
                putExtra(EXTRA_ASPECT_RATIO, aspectRatio)
                putExtra(EXTRA_TEXTURE_FILTER, textureFilter)
            }

        private fun updateCurrentInstance(instance: EmulatorActivity?) {
            currentInstance = instance
        }

        @JvmStatic
        fun requestQuickActionsFromNative() {
            currentInstance?.runOnUiThread {
                currentInstance?.showQuickActionsDialog()
            }
        }
    }
}
