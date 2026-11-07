package com.izzy2lost.neonsaturn

import android.content.res.Configuration
import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.slider.Slider
import kotlin.math.roundToInt

class TouchControlsEditorActivity : AppCompatActivity() {
    private lateinit var store: BootstrapStore
    private lateinit var editorView: TouchControlsView
    private lateinit var sizeSlider: Slider
    private lateinit var sizeLabel: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_touch_controls_editor)

        store = BootstrapStore(this)
        editorView = findViewById(R.id.touchControlsEditorView)

        val isPortrait = resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        editorView.interactionMode = TouchControlsView.InteractionMode.EDIT
        // No point positioning a button that gameplay will not show.
        editorView.rewindAvailable = store.loadRewindEnabled()
        editorView.gameAspect = when (store.loadAspectRatio()) {
            BootstrapStore.ASPECT_16_9 -> 16f / 9f
            BootstrapStore.ASPECT_STRETCH -> null
            else -> 4f / 3f
        }
        editorView.touchControlsLayout = if (isPortrait) {
            store.loadTouchControlsLayoutPortrait()
        } else {
            store.loadTouchControlsLayout()
        }
        sizeSlider = findViewById(R.id.touchControlsSizeSlider)
        sizeLabel = findViewById(R.id.touchControlsSizeLabel)
        sizeSlider.setLabelFormatter { value -> "${value.roundToInt()}%" }
        sizeSlider.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
                editorView.setScaleForSelection(value / 100f)
            }
        }

        editorView.onLayoutChanged = { layout ->
            if (isPortrait) {
                store.saveTouchControlsLayoutPortrait(layout)
            } else {
                store.saveTouchControlsLayout(layout)
            }
            syncSizeControls()
        }
        editorView.onSelectionChanged = { syncSizeControls() }
        syncSizeControls()

        findViewById<MaterialButton>(R.id.resetTouchControlsButton).setOnClickListener {
            editorView.selectedControl = null
            if (isPortrait) {
                store.resetTouchControlsLayoutPortrait()
                editorView.touchControlsLayout = store.loadTouchControlsLayoutPortrait()
            } else {
                store.resetTouchControlsLayout()
                editorView.touchControlsLayout = store.loadTouchControlsLayout()
            }
            syncSizeControls()
        }

        findViewById<MaterialButton>(R.id.doneTouchControlsButton).setOnClickListener {
            finish()
        }
    }

    private fun syncSizeControls() {
        val percent = (editorView.scaleForSelection() * 100f).roundToInt()
        val snapped = ((percent / 5f).roundToInt() * 5).coerceIn(
            (TouchControlsLayout.MIN_SCALE * 100).roundToInt(),
            (TouchControlsLayout.MAX_SCALE * 100).roundToInt(),
        )
        if (sizeSlider.value.roundToInt() != snapped) {
            sizeSlider.value = snapped.toFloat()
        }
        val selected = editorView.selectedControl
        sizeLabel.text = if (selected == null) {
            getString(R.string.touch_controls_size_all, percent)
        } else {
            getString(R.string.touch_controls_size_one, selected.label, percent)
        }
    }
}
