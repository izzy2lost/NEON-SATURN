package com.izzy2lost.neonsaturn

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

        editorView.interactionMode = TouchControlsView.InteractionMode.EDIT
        editorView.touchControlsLayout = store.loadTouchControlsLayout()
        editorView.onLayoutChanged = { layout ->
            store.saveTouchControlsLayout(layout)
        }

        findViewById<MaterialButton>(R.id.resetTouchControlsButton).setOnClickListener {
            store.resetTouchControlsLayout()
            editorView.touchControlsLayout = store.loadTouchControlsLayout()
        }

        findViewById<MaterialButton>(R.id.doneTouchControlsButton).setOnClickListener {
            finish()
        }
    }
}
