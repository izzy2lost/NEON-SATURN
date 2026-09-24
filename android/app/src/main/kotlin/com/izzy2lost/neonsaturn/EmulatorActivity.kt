package com.izzy2lost.neonsaturn

import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.os.Bundle
import android.text.format.DateUtils
import android.view.ContextThemeWrapper
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.libsdl.app.SDLActivity
import java.io.File

class EmulatorActivity : SDLActivity() {
    private lateinit var store: BootstrapStore
    private var quickActionsDialog: AlertDialog? = null
    private var saveStateSlotsDialog: AlertDialog? = null
    private var saveStateOperationRunning = false
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
        if (saveStateSlotsDialog?.isShowing == true) {
            saveStateSlotsDialog?.dismiss()
        }
        super.onPause()
    }

    override fun onDestroy() {
        if (quickActionsDialog?.isShowing == true) {
            quickActionsDialog?.dismiss()
        }
        if (saveStateSlotsDialog?.isShowing == true) {
            saveStateSlotsDialog?.dismiss()
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
        arguments += "--upscale-filter=${intent.getIntExtra(EXTRA_UPSCALE_FILTER, BootstrapStore.UPSCALE_OFF)}"
        arguments += "--deinterlace=${intent.getBooleanExtra(EXTRA_DEINTERLACE, false)}"
        arguments += "--transparent-meshes=${intent.getBooleanExtra(EXTRA_TRANSPARENT_MESHES, false)}"
        arguments += "--rewind=${intent.getBooleanExtra(EXTRA_REWIND_ENABLED, false)}"
        return arguments.toTypedArray()
    }

    private fun isMenuOpen(): Boolean =
        quickActionsDialog?.isShowing == true ||
            saveStateSlotsDialog?.isShowing == true ||
            saveStateOperationRunning

    private fun showQuickActionsDialog() {
        if (isFinishing || isDestroyed || isMenuOpen()) {
            return
        }

        nativeSetPaused(true)
        suspendTouchControls()

        // SDLActivity is not an AppCompat activity, so it cannot follow the app's theme
        // preference. This dialog floats over a fullscreen black game, so pin it to the
        // same dark surface the library uses instead of letting the system theme decide.
        val dialogContext = ContextThemeWrapper(this, R.style.ThemeOverlay_NeonSaturn_LibrarySurface)

        val content = LayoutInflater.from(dialogContext).inflate(R.layout.dialog_quick_actions, null, false)
        content.findViewById<TextView>(R.id.quickActionsSubtitleText).text =
            intent.getStringExtra(EXTRA_DISC_PATH)?.substringAfterLast('/')
                ?.substringAfterLast('\\')
                ?: getString(R.string.quick_actions_default_subtitle)

        var resumeOnDismiss = true

        val dialog = MaterialAlertDialogBuilder(dialogContext, R.style.ThemeOverlay_NeonSaturn_QuickActionsDialog)
            .setView(content)
            .setCancelable(true)
            .create()

        content.findViewById<MaterialButton>(R.id.resumeButton).setOnClickListener {
            dialog.dismiss()
        }
        // The slot picker takes over from here and stays paused until it is done.
        content.findViewById<MaterialButton>(R.id.saveStateButton).setOnClickListener {
            resumeOnDismiss = false
            dialog.dismiss()
            showSaveStateSlotsDialog(save = true)
        }
        content.findViewById<MaterialButton>(R.id.loadStateButton).setOnClickListener {
            resumeOnDismiss = false
            dialog.dismiss()
            showSaveStateSlotsDialog(save = false)
        }
        content.findViewById<MaterialButton>(R.id.exitGameButton).setOnClickListener {
            resumeOnDismiss = false
            nativeExitEmulator()
            dialog.dismiss()
            finish()
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

    private fun resumeEmulation() {
        nativeSetPaused(false)
        resumeTouchControls()
    }

    private fun showSaveStateSlotsDialog(save: Boolean) {
        val directory = nativeGetSaveStatesDirectory()
        if (isFinishing || isDestroyed || directory.isEmpty()) {
            if (directory.isEmpty()) {
                Toast.makeText(this, R.string.save_state_not_ready, Toast.LENGTH_SHORT).show()
            }
            resumeEmulation()
            return
        }

        val dialogContext = ContextThemeWrapper(this, R.style.ThemeOverlay_NeonSaturn_LibrarySurface)
        val inflater = LayoutInflater.from(dialogContext)
        val content = inflater.inflate(R.layout.dialog_save_state_slots, null, false)
        val slotList = content.findViewById<LinearLayout>(R.id.saveStateSlotList)

        var operationStarted = false
        lateinit var dialog: AlertDialog
        val thumbnailWidthPx = resources.getDimensionPixelSize(R.dimen.save_state_thumbnail_width)

        for (slot in SaveStateSlot.listIn(File(directory))) {
            val row = inflater.inflate(R.layout.item_save_state_slot, slotList, false)
            val thumbnail = row.findViewById<ImageView>(R.id.saveStateThumbnail)
            val hint = row.findViewById<TextView>(R.id.saveStateHint)

            row.findViewById<TextView>(R.id.saveStateSlotLabel).text =
                getString(R.string.save_state_slot_label, slot.number)
            row.findViewById<TextView>(R.id.saveStateTimestamp).text =
                if (slot.exists) getString(R.string.save_state_saved_at, formatSavedAt(slot)) else
                    getString(R.string.save_state_empty_slot)

            val bitmap = slot.decodeThumbnail(thumbnailWidthPx)
            thumbnail.setImageBitmap(bitmap)
            if (bitmap != null) {
                hint.setText(R.string.save_state_tap_to_enlarge)
                thumbnail.setOnClickListener { showSaveStatePreview(dialogContext, slot) }
            } else {
                hint.text = if (slot.exists) getString(R.string.save_state_no_preview) else ""
                hint.visibility = if (slot.exists) View.VISIBLE else View.GONE
            }

            // Saving can target any slot; loading only makes sense for filled ones.
            val selectable = save || slot.exists
            row.isEnabled = selectable
            row.isClickable = selectable
            row.isFocusable = selectable
            row.alpha = if (selectable) 1f else 0.45f
            if (selectable) {
                row.setOnClickListener {
                    operationStarted = true
                    dialog.dismiss()
                    runSaveStateOperation(slot, save)
                }
            }
            slotList.addView(row)
        }

        dialog = MaterialAlertDialogBuilder(dialogContext, R.style.ThemeOverlay_NeonSaturn_QuickActionsDialog)
            .setTitle(if (save) R.string.save_state_select_save_slot else R.string.save_state_select_load_slot)
            .setView(content)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        dialog.setOnDismissListener {
            saveStateSlotsDialog = null
            if (!operationStarted) {
                resumeEmulation()
            }
        }

        saveStateSlotsDialog = dialog
        dialog.show()
    }

    private fun formatSavedAt(slot: SaveStateSlot): String =
        DateUtils.formatDateTime(
            this,
            slot.savedAtMillis,
            DateUtils.FORMAT_SHOW_DATE or DateUtils.FORMAT_SHOW_TIME or DateUtils.FORMAT_ABBREV_MONTH,
        )

    private fun showSaveStatePreview(dialogContext: Context, slot: SaveStateSlot) {
        val bitmap = slot.decodeThumbnail() ?: return
        val content = LayoutInflater.from(dialogContext).inflate(R.layout.dialog_save_state_preview, null, false)
        content.findViewById<ImageView>(R.id.saveStatePreviewImage).setImageBitmap(bitmap)

        MaterialAlertDialogBuilder(dialogContext, R.style.ThemeOverlay_NeonSaturn_QuickActionsDialog)
            .setTitle(getString(R.string.save_state_preview_title, slot.number, formatSavedAt(slot)))
            .setView(content)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    /**
     * Serializing a state is several megabytes of work, so it runs off the UI thread. The
     * emulator stays paused throughout and resumes once the result is known.
     */
    private fun runSaveStateOperation(slot: SaveStateSlot, save: Boolean) {
        saveStateOperationRunning = true
        Thread({
            val message = runCatching {
                if (save) nativeSaveState(slot.index) else nativeLoadState(slot.index)
            }.getOrElse { it.message ?: "Save state operation failed" }

            runOnUiThread {
                saveStateOperationRunning = false
                if (isFinishing || isDestroyed) {
                    return@runOnUiThread
                }
                Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
                resumeEmulation()
            }
        }, "NeonSaturn save state").start()
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
            pushSpeedControls(rewind = false, fastForward = false)
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
                pushSpeedControls(state.rewind, state.fastForward)
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

        overlay.rewindAvailable = store.loadRewindEnabled()

        val isPortrait = resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        overlay.touchControlsLayout = if (isPortrait) {
            store.loadTouchControlsLayoutPortrait()
        } else {
            store.loadTouchControlsLayout()
        }
        overlay.inputSuspended = isMenuOpen()
    }

    private fun removeTouchControlsOverlay() {
        touchControlsView?.resetRuntimeState()
        (touchControlsView?.parent as? ViewGroup)?.removeView(touchControlsView)
        touchControlsView = null
    }

    private fun suspendTouchControls() {
        touchControlsView?.inputSuspended = true
        pushTouchControlsState(0, 0, 0, 0f, 0f)
        pushSpeedControls(rewind = false, fastForward = false)
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

    private fun pushSpeedControls(rewind: Boolean, fastForward: Boolean) {
        runCatching {
            nativeSetSpeedControls(rewind, fastForward)
        }
    }

    private external fun nativeSetPaused(paused: Boolean)
    private external fun nativeSetSpeedControls(rewind: Boolean, fastForward: Boolean)
    private external fun nativeSaveState(slotIndex: Int): String
    private external fun nativeLoadState(slotIndex: Int): String
    private external fun nativeGetSaveStatesDirectory(): String
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
        private const val EXTRA_UPSCALE_FILTER =
            "com.izzy2lost.neonsaturn.extra.UPSCALE_FILTER"
        private const val EXTRA_DEINTERLACE =
            "com.izzy2lost.neonsaturn.extra.DEINTERLACE"
        private const val EXTRA_TRANSPARENT_MESHES =
            "com.izzy2lost.neonsaturn.extra.TRANSPARENT_MESHES"
        private const val EXTRA_REWIND_ENABLED =
            "com.izzy2lost.neonsaturn.extra.REWIND_ENABLED"

        fun createIntent(
            context: Context,
            selection: StoredLaunchSelection,
            paths: NeonSaturnPaths,
            gameControllerDbPath: String? = null,
            aspectRatio: String = BootstrapStore.ASPECT_4_3,
            textureFilter: String = BootstrapStore.FILTER_NEAREST,
            upscaleFilter: Int = BootstrapStore.UPSCALE_OFF,
            deinterlace: Boolean = false,
            transparentMeshes: Boolean = false,
            rewindEnabled: Boolean = false
        ): Intent =
            Intent(context, EmulatorActivity::class.java).apply {
                putExtra(EXTRA_IPL_PATH, selection.iplPath)
                putExtra(EXTRA_CDB_PATH, selection.cdbPath)
                putExtra(EXTRA_DISC_PATH, selection.discPath)
                putExtra(EXTRA_DATA_ROOT, paths.root.absolutePath)
                putExtra(EXTRA_GAME_CONTROLLER_DB_PATH, gameControllerDbPath)
                putExtra(EXTRA_ASPECT_RATIO, aspectRatio)
                putExtra(EXTRA_TEXTURE_FILTER, textureFilter)
                putExtra(EXTRA_UPSCALE_FILTER, upscaleFilter)
                putExtra(EXTRA_DEINTERLACE, deinterlace)
                putExtra(EXTRA_TRANSPARENT_MESHES, transparentMeshes)
                putExtra(EXTRA_REWIND_ENABLED, rewindEnabled)
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
