package com.izzy2lost.neonsaturn

import android.content.res.Configuration
import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton

class TouchControlsEditorActivity : AppCompatActivity() {
    private lateinit var store: BootstrapStore
    private lateinit var editorView: TouchControlsView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_touch_controls_editor)

        store = BootstrapStore(this)
        editorView = findViewById(R.id.touchControlsEditorView)

        val isPortrait = resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        editorView.interactionMode = TouchControlsView.InteractionMode.EDIT
        // No point positioning a button that gameplay will not show.
        editorView.rewindAvailable = store.loadRewindEnabled()
        editorView.touchControlsLayout = if (isPortrait) {
            store.loadTouchControlsLayoutPortrait()
        } else {
            store.loadTouchControlsLayout()
        }
        editorView.onLayoutChanged = { layout ->
            if (isPortrait) {
                store.saveTouchControlsLayoutPortrait(layout)
            } else {
                store.saveTouchControlsLayout(layout)
            }
        }

        findViewById<MaterialButton>(R.id.resetTouchControlsButton).setOnClickListener {
            if (isPortrait) {
                store.resetTouchControlsLayoutPortrait()
                editorView.touchControlsLayout = store.loadTouchControlsLayoutPortrait()
            } else {
                store.resetTouchControlsLayout()
                editorView.touchControlsLayout = store.loadTouchControlsLayout()
            }
        }

        findViewById<MaterialButton>(R.id.doneTouchControlsButton).setOnClickListener {
            finish()
        }
    }
}
