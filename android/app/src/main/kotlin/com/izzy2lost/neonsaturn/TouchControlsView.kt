package com.izzy2lost.neonsaturn

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PointF
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import androidx.appcompat.content.res.AppCompatResources
import androidx.core.graphics.ColorUtils
import kotlin.math.abs
import kotlin.math.hypot

class TouchControlsView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {
    enum class InteractionMode {
        PLAY,
        EDIT,
    }

    private enum class PointerType {
        BUTTON,
        MENU,
        DPAD,
        ANALOG,
        EDIT,
    }

    private data class PointerInteraction(
        val pointerType: PointerType,
        val controlId: TouchControlId,
        val offsetX: Float = 0f,
        val offsetY: Float = 0f,
        val inside: Boolean = true,
    )

    /** Editor two-finger resize of one control. */
    private data class PinchGesture(
        val controlId: TouchControlId,
        val pointerA: Int,
        val pointerB: Int,
        val startDistance: Float,
        val startScale: Float,
    )

    var interactionMode: InteractionMode = InteractionMode.PLAY
        set(value) {
            if (field == value) {
                return
            }
            field = value
            resetRuntimeState()
            invalidate()
        }

    var touchControlsLayout: TouchControlsLayout = TouchControlsLayout.DEFAULT
        set(value) {
            field = value
            notifyGameBandChanged()
            invalidate()
        }

    /** Editor only: the control the size slider applies to, or null for all controls. */
    var selectedControl: TouchControlId? = null
        set(value) {
            if (field == value) {
                return
            }
            field = value
            onSelectionChanged?.invoke(value)
            invalidate()
        }

    var onSelectionChanged: ((TouchControlId?) -> Unit)? = null

    var onStateChanged: ((TouchControlsState) -> Unit)? = null
    var onLayoutChanged: ((TouchControlsLayout) -> Unit)? = null
    var onMenuPressed: (() -> Unit)? = null

    /**
     * Portrait only: the free band between the top row of controls and the rest, as fractions of the view height,
     * or null in landscape. The game image is placed in it so it does not sit under the buttons.
     */
    var onGameBandChanged: ((top: Float, bottom: Float) -> Unit)? = null

    /** Width over height of the game image, for the editor's guide; null when stretched to fill the screen. */
    var gameAspect: Float? = 4f / 3f
        set(value) {
            field = value
            invalidate()
        }

    var inputSuspended: Boolean = false
        set(value) {
            field = value
            if (value) {
                resetRuntimeState()
            }
            invalidate()
        }

    /**
     * Whether the rewind button is shown at all. Rewinding needs the emulator to be
     * recording a timeline, which is opt-in, so the button stays hidden and untappable
     * until that setting is on.
     */
    var rewindAvailable: Boolean = false
        set(value) {
            if (field == value) {
                return
            }
            field = value
            if (!value) {
                pointerInteractions.entries.removeAll { it.value.controlId == TouchControlId.REWIND }
                rebuildStateFromPointers()
            }
            notifyGameBandChanged()
            invalidate()
        }

    private val density = resources.displayMetrics.density
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2.5f * density
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(210, 255, 255, 255)
        textAlign = Paint.Align.CENTER
        textSize = 13f * density
    }
    private val guidePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(80, 255, 255, 255)
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * density
    }
    private val guideFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(30, 255, 255, 255)
        style = Paint.Style.FILL
    }
    private val tempRect = RectF()
    private val pointerInteractions = mutableMapOf<Int, PointerInteraction>()
    private var pinchGesture: PinchGesture? = null
    private var editDownPoint = PointF()
    private var editMoved = false
    private val selectionPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
        color = Color.parseColor("#3FD7FF")
        pathEffect = android.graphics.DashPathEffect(floatArrayOf(8f * density, 6f * density), 0f)
    }
    private var currentState = TouchControlsState()

    // The vector ships with a theme-resolved tint; force white so they read against the
    // button fill regardless of which theme the host activity is using.
    private val rewindIcon: Drawable? = loadIcon(R.drawable.fast_rewind_24px)

    private val controlHitOrder = listOf(
        TouchControlId.MENU,
        TouchControlId.START,
        TouchControlId.L,
        TouchControlId.R,
        // After L/R deliberately: if a custom layout overlaps them, a misplaced shoulder
        // press costs less than an accidental rewind mid-game.
        TouchControlId.REWIND,
        TouchControlId.Z,
        TouchControlId.Y,
        TouchControlId.X,
        TouchControlId.C,
        TouchControlId.B,
        TouchControlId.A,
        TouchControlId.DPAD,
        TouchControlId.ANALOG,
    )

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        notifyGameBandChanged()
    }

    private fun notifyGameBandChanged() {
        val callback = onGameBandChanged ?: return
        val band = portraitGameBand()
        if (band == null) {
            callback(-1f, -1f)
        } else {
            callback(band.first / height, band.second / height)
        }
    }

    /**
     * The free band in pixels: from below the controls in the top half (shoulders, menu, rewind) to above
     * the ones in the bottom half. Null in landscape, where the image fills the height between the side controls.
     */
    private fun portraitGameBand(): Pair<Float, Float>? {
        if (width == 0 || height == 0 || width > height) {
            return null
        }
        val margin = 8f * density
        var top = 0f
        var bottom = height.toFloat()
        for (controlId in TouchControlId.entries) {
            if (!isControlAvailable(controlId)) {
                continue
            }
            val centerY = height * touchControlsLayout.placementFor(controlId).y
            val extentY = controlExtents(controlId).second
            if (centerY < height / 2f) {
                top = maxOf(top, centerY + extentY + margin)
            } else {
                bottom = minOf(bottom, centerY - extentY - margin)
            }
        }
        return if (bottom > top) top to bottom else null
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        if (interactionMode == InteractionMode.EDIT) {
            canvas.drawColor(Color.parseColor("#06090D"))
            drawEditorGuide(canvas)
            drawEditorInstruction(canvas)
        }

        drawShoulderButton(canvas, TouchControlId.L, color = Color.parseColor("#CC0F6A"))
        drawShoulderButton(canvas, TouchControlId.R, color = Color.parseColor("#CC0F6A"))
        drawDpad(canvas)
        drawAnalogStick(canvas)

        drawRoundButton(canvas, TouchControlId.X, Color.parseColor("#878A92"))
        drawRoundButton(canvas, TouchControlId.Y, Color.parseColor("#878A92"))
        drawRoundButton(canvas, TouchControlId.Z, Color.parseColor("#878A92"))

        drawRoundButton(canvas, TouchControlId.A, Color.parseColor("#36A157"))
        drawRoundButton(canvas, TouchControlId.B, Color.parseColor("#F0CC2B"), textColor = Color.BLACK)
        drawRoundButton(canvas, TouchControlId.C, Color.parseColor("#2E4FD6"))

        drawCapsuleButton(canvas, TouchControlId.START, Color.parseColor("#CC0F6A"))
        drawCapsuleButton(canvas, TouchControlId.MENU, Color.parseColor("#394550"))

        // Slate like MENU, setting the emulator action apart from the Saturn buttons.
        if (rewindAvailable) {
            drawIconButton(canvas, TouchControlId.REWIND, rewindIcon, Color.parseColor("#394550"))
        }

        if (interactionMode == InteractionMode.EDIT) {
            selectedControl?.takeIf { isControlAvailable(it) }?.let { drawSelection(canvas, it) }
        }
    }

    private fun drawSelection(canvas: Canvas, controlId: TouchControlId) {
        val center = controlCenter(controlId)
        val (extentX, extentY) = controlExtents(controlId)
        val pad = 6f * density
        tempRect.set(center.x - extentX - pad, center.y - extentY - pad, center.x + extentX + pad, center.y + extentY + pad)
        val corner = minOf(tempRect.width(), tempRect.height()) / 2f
        canvas.drawRoundRect(tempRect, corner, corner, selectionPaint)
    }

    /** Sets the size of the selected control, or of every control when none is selected. */
    fun setScaleForSelection(scale: Float) {
        val clamped = TouchControlsLayout.clampScale(scale)
        val controlId = selectedControl
        val updated = if (controlId == null) {
            touchControlsLayout.copy(globalScale = clamped)
        } else {
            touchControlsLayout.withPlacement(controlId, touchControlsLayout.placementFor(controlId).copy(scale = clamped))
        }
        applyEditedLayout(reclamp(updated, controlId))
    }

    fun scaleForSelection(): Float {
        val controlId = selectedControl ?: return touchControlsLayout.globalScale
        return touchControlsLayout.placementFor(controlId).scale
    }

    private fun applyEditedLayout(layout: TouchControlsLayout) {
        touchControlsLayout = layout
        onLayoutChanged?.invoke(layout)
    }

    /**
     * Pulls a resized control (or every control, for [only] == null) back on screen after a size change pushed its
     * edges past the view bounds.
     */
    private fun reclamp(layout: TouchControlsLayout, only: TouchControlId?): TouchControlsLayout {
        if (width == 0 || height == 0) {
            return layout
        }
        val previous = touchControlsLayout
        touchControlsLayout = layout
        var result = layout
        for (controlId in only?.let { listOf(it) } ?: TouchControlId.entries) {
            val placement = result.placementFor(controlId)
            val clamped = clampControlCenter(controlId, width * placement.x, height * placement.y)
            result = result.withPlacement(
                controlId,
                placement.copy(
                    x = (clamped.x / width).coerceIn(0f, 1f),
                    y = (clamped.y / height).coerceIn(0f, 1f),
                ),
            )
        }
        touchControlsLayout = previous
        return result
    }

    override fun onTouchEvent(event: MotionEvent): Boolean =
        when (interactionMode) {
            InteractionMode.PLAY -> handlePlayTouchEvent(event)
            InteractionMode.EDIT -> handleEditTouchEvent(event)
        }

    fun resetRuntimeState() {
        if (pointerInteractions.isEmpty() && currentState == TouchControlsState()) {
            return
        }

        pointerInteractions.clear()
        updateState(TouchControlsState())
        invalidate()
    }

    private fun handlePlayTouchEvent(event: MotionEvent): Boolean {
        if (inputSuspended) {
            return false
        }

        return when (event.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN -> {
                val index = event.actionIndex
                handlePlayPointerDown(event.getPointerId(index), event.getX(index), event.getY(index))
            }

            MotionEvent.ACTION_MOVE -> {
                var handled = pointerInteractions.isNotEmpty()
                for (index in 0 until event.pointerCount) {
                    handled = handlePlayPointerMove(
                        event.getPointerId(index),
                        event.getX(index),
                        event.getY(index),
                    ) || handled
                }
                handled
            }

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP -> {
                val index = event.actionIndex
                handlePlayPointerUp(event.getPointerId(index), event.getX(index), event.getY(index))
            }

            MotionEvent.ACTION_CANCEL -> {
                val hadInteraction = pointerInteractions.isNotEmpty()
                resetRuntimeState()
                hadInteraction
            }

            else -> false
        }
    }

    private fun handlePlayPointerDown(pointerId: Int, x: Float, y: Float): Boolean {
        val controlId = hitTestControl(x, y) ?: return false
        val pointerType = when (controlId) {
            TouchControlId.DPAD -> {
                if (hasPointerOfType(PointerType.DPAD)) {
                    return false
                }
                PointerType.DPAD
            }

            TouchControlId.ANALOG -> {
                if (hasPointerOfType(PointerType.ANALOG)) {
                    return false
                }
                PointerType.ANALOG
            }

            TouchControlId.MENU -> PointerType.MENU
            else -> PointerType.BUTTON
        }

        pointerInteractions[pointerId] = PointerInteraction(pointerType = pointerType, controlId = controlId)

        when (pointerType) {
            PointerType.DPAD -> updateDpadState(x, y)
            PointerType.ANALOG -> updateAnalogState(x, y)
            PointerType.BUTTON,
            PointerType.MENU -> rebuildStateFromPointers()
            PointerType.EDIT -> Unit
        }

        invalidate()
        return true
    }

    private fun handlePlayPointerMove(pointerId: Int, x: Float, y: Float): Boolean {
        val interaction = pointerInteractions[pointerId] ?: return false
        when (interaction.pointerType) {
            PointerType.BUTTON,
            PointerType.MENU -> {
                val inside = isPointInsideControl(interaction.controlId, x, y, hitExpansion = 1.15f)
                if (inside != interaction.inside) {
                    pointerInteractions[pointerId] = interaction.copy(inside = inside)
                    rebuildStateFromPointers()
                }
            }

            PointerType.DPAD -> updateDpadState(x, y)
            PointerType.ANALOG -> updateAnalogState(x, y)
            PointerType.EDIT -> Unit
        }

        invalidate()
        return true
    }

    private fun handlePlayPointerUp(pointerId: Int, x: Float, y: Float): Boolean {
        val interaction = pointerInteractions.remove(pointerId) ?: return false
        if (interaction.pointerType == PointerType.MENU &&
            interaction.inside &&
            isPointInsideControl(interaction.controlId, x, y, hitExpansion = 1.10f)
        ) {
            onMenuPressed?.invoke()
        }

        when (interaction.pointerType) {
            PointerType.DPAD -> updateState(currentState.copy(dpadX = 0, dpadY = 0))
            PointerType.ANALOG -> updateState(currentState.copy(analogX = 0f, analogY = 0f))
            PointerType.BUTTON,
            PointerType.MENU -> rebuildStateFromPointers()
            PointerType.EDIT -> Unit
        }

        invalidate()
        return true
    }

    private fun handleEditTouchEvent(event: MotionEvent): Boolean {
        return when (event.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN -> {
                val index = event.actionIndex
                val pointerId = event.getPointerId(index)
                val x = event.getX(index)
                val y = event.getY(index)
                val controlId = hitTestControl(x, y)

                // A second finger on the held control, or on empty space, turns the drag into a pinch resize.
                val held = pointerInteractions.entries.singleOrNull()
                if (held != null && pinchGesture == null && (controlId == null || controlId == held.value.controlId)) {
                    val heldIndex = event.findPointerIndex(held.key)
                    if (heldIndex >= 0) {
                        val distance = hypot(x - event.getX(heldIndex), y - event.getY(heldIndex))
                        val target = held.value.controlId
                        pinchGesture = PinchGesture(
                            controlId = target,
                            pointerA = held.key,
                            pointerB = pointerId,
                            startDistance = distance.coerceAtLeast(1f),
                            startScale = touchControlsLayout.placementFor(target).scale,
                        )
                        selectedControl = target
                        editMoved = true
                        return true
                    }
                }

                if (controlId == null) {
                    if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                        // Tapping empty space goes back to sizing every control at once.
                        selectedControl = null
                    }
                    return event.actionMasked == MotionEvent.ACTION_DOWN
                }
                val center = controlCenter(controlId)
                pointerInteractions[pointerId] = PointerInteraction(
                    pointerType = PointerType.EDIT,
                    controlId = controlId,
                    offsetX = center.x - x,
                    offsetY = center.y - y,
                )
                if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                    editDownPoint = PointF(x, y)
                    editMoved = false
                }
                selectedControl = controlId
                invalidate()
                true
            }

            MotionEvent.ACTION_MOVE -> {
                val pinch = pinchGesture
                if (pinch != null) {
                    val a = event.findPointerIndex(pinch.pointerA)
                    val b = event.findPointerIndex(pinch.pointerB)
                    if (a >= 0 && b >= 0) {
                        val distance = hypot(event.getX(a) - event.getX(b), event.getY(a) - event.getY(b))
                        val scale = TouchControlsLayout.clampScale(pinch.startScale * distance / pinch.startDistance)
                        val placement = touchControlsLayout.placementFor(pinch.controlId)
                        if (scale != placement.scale) {
                            applyEditedLayout(
                                reclamp(
                                    touchControlsLayout.withPlacement(pinch.controlId, placement.copy(scale = scale)),
                                    pinch.controlId,
                                )
                            )
                        }
                    }
                    return true
                }

                var handled = false
                for (index in 0 until event.pointerCount) {
                    val pointerId = event.getPointerId(index)
                    val interaction = pointerInteractions[pointerId] ?: continue
                    if (interaction.pointerType != PointerType.EDIT) {
                        continue
                    }

                    if (!editMoved &&
                        hypot(event.getX(index) - editDownPoint.x, event.getY(index) - editDownPoint.y) < 6f * density
                    ) {
                        handled = true
                        continue
                    }
                    editMoved = true

                    val clampedPoint = clampControlCenter(
                        controlId = interaction.controlId,
                        x = event.getX(index) + interaction.offsetX,
                        y = event.getY(index) + interaction.offsetY,
                    )
                    val updatedLayout = touchControlsLayout.withPlacement(
                        interaction.controlId,
                        touchControlsLayout.placementFor(interaction.controlId).copy(
                            x = (clampedPoint.x / width.toFloat()).coerceIn(0f, 1f),
                            y = (clampedPoint.y / height.toFloat()).coerceIn(0f, 1f),
                        ),
                    )
                    applyEditedLayout(updatedLayout)
                    handled = true
                }
                handled
            }

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP -> {
                val pointerId = event.getPointerId(event.actionIndex)
                val pinch = pinchGesture
                if (pinch != null && (pointerId == pinch.pointerA || pointerId == pinch.pointerB)) {
                    // End the pinch; the remaining finger stays inert until lifted so the control does not jump.
                    pinchGesture = null
                    pointerInteractions.clear()
                    invalidate()
                    return true
                }
                pointerInteractions.remove(pointerId) != null
            }

            MotionEvent.ACTION_CANCEL -> {
                val handled = pointerInteractions.isNotEmpty() || pinchGesture != null
                pointerInteractions.clear()
                pinchGesture = null
                invalidate()
                handled
            }

            else -> false
        }
    }

    private fun drawEditorGuide(canvas: Canvas) {
        if (width > height) {
            // Landscape: game fills full height at 4:3, centered horizontally
            val gameWidth = height * (4f / 3f)
            val left = ((width - gameWidth) / 2f).coerceAtLeast(0f)
            val right = (left + gameWidth).coerceAtMost(width.toFloat())
            tempRect.set(left, 0f, right, height.toFloat())
        } else {
            // Portrait: game fills the width, placed in the band between the controls like the emulator does
            val aspect = gameAspect
            if (aspect == null) {
                tempRect.set(0f, 0f, width.toFloat(), height.toFloat())
            } else {
                val gameHeight = minOf(width / aspect, height.toFloat())
                val band = portraitGameBand()
                val top = if (band != null) {
                    placeGameInBand(height.toFloat(), gameHeight, band.first, band.second)
                } else {
                    (height - gameHeight) / 2f
                }
                tempRect.set(0f, top, width.toFloat(), top + gameHeight)
            }
        }
        canvas.drawRoundRect(tempRect, 24f * density, 24f * density, guideFillPaint)
        canvas.drawRoundRect(tempRect, 24f * density, 24f * density, guidePaint)

        textPaint.textSize = 14f * density
        textPaint.color = Color.argb(180, 255, 255, 255)
        val textY = tempRect.centerY() - ((textPaint.descent() + textPaint.ascent()) / 2f)
        canvas.drawText("Game image area", tempRect.centerX(), textY, textPaint)
    }

    companion object {
        /**
         * Top of the game image: right under the top row of controls, or right above the lower controls when the
         * image does not fit between them. Mirrors ComputeDestRect in emulator_main.cpp.
         */
        fun placeGameInBand(viewHeight: Float, gameHeight: Float, bandTop: Float, bandBottom: Float): Float {
            val top = if (bandTop + gameHeight <= bandBottom) bandTop else bandBottom - gameHeight
            return top.coerceIn(0f, maxOf(viewHeight - gameHeight, 0f))
        }
    }

    private fun drawEditorInstruction(canvas: Canvas) {
        textPaint.textSize = 15f * density
        textPaint.color = Color.argb(220, 255, 255, 255)
        canvas.drawText("Drag to move \u2022 tap to select \u2022 pinch to resize", width / 2f, 28f * density, textPaint)
    }

    private fun drawRoundButton(canvas: Canvas, controlId: TouchControlId, color: Int, textColor: Int = Color.WHITE) {
        val center = controlCenter(controlId)
        val radius = faceButtonRadius(controlId)
        val pressed = isControlPressed(controlId)
        fillPaint.color = fillColorFor(color, pressed)
        strokePaint.color = outlineColorFor(color, pressed)
        canvas.drawCircle(center.x, center.y, radius, fillPaint)
        canvas.drawCircle(center.x, center.y, radius, strokePaint)

        textPaint.textSize = 23f * unit(controlId)
        textPaint.color = textColor
        val baseline = center.y - ((textPaint.descent() + textPaint.ascent()) / 2f)
        canvas.drawText(controlId.label, center.x, baseline, textPaint)

        if (interactionMode == InteractionMode.EDIT) {
            drawCaption(canvas, controlId, center.x, center.y + radius + (18f * density))
        }
    }

    private fun drawCapsuleButton(canvas: Canvas, controlId: TouchControlId, color: Int) {
        val center = controlCenter(controlId)
        val width = if (controlId == TouchControlId.MENU) 92f * unit(controlId) else 86f * unit(controlId)
        val height = 34f * unit(controlId)
        tempRect.set(center.x - (width / 2f), center.y - (height / 2f), center.x + (width / 2f), center.y + (height / 2f))
        val pressed = isControlPressed(controlId)
        fillPaint.color = fillColorFor(color, pressed)
        strokePaint.color = outlineColorFor(color, pressed)
        canvas.drawRoundRect(tempRect, height / 2f, height / 2f, fillPaint)
        canvas.drawRoundRect(tempRect, height / 2f, height / 2f, strokePaint)

        textPaint.textSize = if (controlId == TouchControlId.MENU) 13f * unit(controlId) else 15f * unit(controlId)
        textPaint.color = Color.WHITE
        val baseline = center.y - ((textPaint.descent() + textPaint.ascent()) / 2f)
        canvas.drawText(controlId.label.uppercase(), center.x, baseline, textPaint)

        if (interactionMode == InteractionMode.EDIT) {
            drawCaption(canvas, controlId, center.x, tempRect.bottom + (16f * density))
        }
    }

    private fun loadIcon(resId: Int): Drawable? =
        AppCompatResources.getDrawable(context, resId)?.mutate()?.apply { setTint(Color.WHITE) }

    private fun drawIconButton(canvas: Canvas, controlId: TouchControlId, icon: Drawable?, color: Int) {
        val center = controlCenter(controlId)
        val radius = actionButtonRadius()
        val pressed = isControlPressed(controlId)
        fillPaint.color = fillColorFor(color, pressed)
        strokePaint.color = outlineColorFor(color, pressed)
        canvas.drawCircle(center.x, center.y, radius, fillPaint)
        canvas.drawCircle(center.x, center.y, radius, strokePaint)

        if (icon != null) {
            val extent = (radius * 0.62f).toInt()
            val cx = center.x.toInt()
            val cy = center.y.toInt()
            icon.setBounds(cx - extent, cy - extent, cx + extent, cy + extent)
            icon.draw(canvas)
        }

        if (interactionMode == InteractionMode.EDIT) {
            drawCaption(canvas, controlId, center.x, center.y + radius + (18f * density))
        }
    }

    private fun drawShoulderButton(canvas: Canvas, controlId: TouchControlId, color: Int) {
        val center = controlCenter(controlId)
        val width = 110f * unit(controlId)
        val height = 30f * unit(controlId)
        tempRect.set(center.x - (width / 2f), center.y - (height / 2f), center.x + (width / 2f), center.y + (height / 2f))
        val pressed = isControlPressed(controlId)
        fillPaint.color = fillColorFor(color, pressed)
        strokePaint.color = outlineColorFor(color, pressed)
        val corner = 18f * unit(controlId)
        canvas.drawRoundRect(tempRect, corner, corner, fillPaint)
        canvas.drawRoundRect(tempRect, corner, corner, strokePaint)

        textPaint.textSize = 18f * unit(controlId)
        textPaint.color = Color.WHITE
        val baseline = center.y - ((textPaint.descent() + textPaint.ascent()) / 2f)
        canvas.drawText(controlId.label, center.x, baseline, textPaint)

        if (interactionMode == InteractionMode.EDIT) {
            drawCaption(canvas, controlId, center.x, tempRect.bottom + (16f * density))
        }
    }

    private fun drawDpad(canvas: Canvas) {
        val center = controlCenter(TouchControlId.DPAD)
        val radius = dpadRadius()
        fillPaint.color = Color.argb(190, 26, 30, 36)
        strokePaint.color = Color.argb(210, 82, 90, 98)
        canvas.drawCircle(center.x, center.y, radius, fillPaint)
        canvas.drawCircle(center.x, center.y, radius, strokePaint)

        val armWidth = radius * 0.68f
        val armThickness = radius * 0.46f
        fillPaint.color = Color.argb(220, 40, 44, 51)
        strokePaint.color = Color.argb(210, 100, 109, 121)

        tempRect.set(center.x - (armWidth / 2f), center.y - radius * 0.84f, center.x + (armWidth / 2f), center.y + radius * 0.84f)
        canvas.drawRoundRect(tempRect, armThickness / 2f, armThickness / 2f, fillPaint)
        canvas.drawRoundRect(tempRect, armThickness / 2f, armThickness / 2f, strokePaint)

        tempRect.set(center.x - radius * 0.84f, center.y - (armWidth / 2f), center.x + radius * 0.84f, center.y + (armWidth / 2f))
        canvas.drawRoundRect(tempRect, armThickness / 2f, armThickness / 2f, fillPaint)
        canvas.drawRoundRect(tempRect, armThickness / 2f, armThickness / 2f, strokePaint)

        fillPaint.color = Color.argb(255, 19, 21, 25)
        canvas.drawCircle(center.x, center.y, radius * 0.20f, fillPaint)

        drawCaption(canvas, TouchControlId.DPAD, center.x, center.y + radius + (18f * density))
    }

    private fun drawAnalogStick(canvas: Canvas) {
        val center = controlCenter(TouchControlId.ANALOG)
        val radius = analogRadius()
        fillPaint.color = Color.argb(170, 32, 36, 41)
        strokePaint.color = Color.argb(200, 95, 106, 117)
        canvas.drawCircle(center.x, center.y, radius, fillPaint)
        canvas.drawCircle(center.x, center.y, radius, strokePaint)

        val knobTravel = radius * 0.45f
        val knobCenter = PointF(center.x + (currentState.analogX * knobTravel), center.y + (currentState.analogY * knobTravel))
        val knobRadius = radius * 0.42f
        fillPaint.color = if (abs(currentState.analogX) > 0.02f || abs(currentState.analogY) > 0.02f) {
            Color.argb(240, 110, 118, 126)
        } else {
            Color.argb(235, 78, 86, 94)
        }
        strokePaint.color = Color.argb(220, 188, 196, 203)
        canvas.drawCircle(knobCenter.x, knobCenter.y, knobRadius, fillPaint)
        canvas.drawCircle(knobCenter.x, knobCenter.y, knobRadius, strokePaint)

        drawCaption(canvas, TouchControlId.ANALOG, center.x, center.y + radius + (18f * density))
    }

    private fun drawCaption(canvas: Canvas, controlId: TouchControlId, x: Float, y: Float) {
        if (interactionMode != InteractionMode.EDIT &&
            controlId != TouchControlId.DPAD &&
            controlId != TouchControlId.ANALOG
        ) {
            return
        }
        canvas.drawText(controlId.label.uppercase(), x, y, labelPaint)
    }

    private fun updateDpadState(x: Float, y: Float) {
        val center = controlCenter(TouchControlId.DPAD)
        val radius = dpadRadius()
        val dx = ((x - center.x) / radius).coerceIn(-1f, 1f)
        val dy = ((y - center.y) / radius).coerceIn(-1f, 1f)
        updateState(currentState.copy(dpadX = quantizeAxis(dx), dpadY = quantizeAxis(dy)))
    }

    private fun updateAnalogState(x: Float, y: Float) {
        val center = controlCenter(TouchControlId.ANALOG)
        val radius = analogRadius()
        var dx = (x - center.x) / radius
        var dy = (y - center.y) / radius
        val magnitude = hypot(dx, dy)
        if (magnitude > 1f) {
            dx /= magnitude
            dy /= magnitude
        }
        if (abs(dx) < 0.07f) {
            dx = 0f
        }
        if (abs(dy) < 0.07f) {
            dy = 0f
        }
        updateState(currentState.copy(analogX = dx, analogY = dy))
    }

    private fun rebuildStateFromPointers() {
        var buttonMask = 0
        var rewind = false
        pointerInteractions.values
            .filter { interaction ->
                (interaction.pointerType == PointerType.BUTTON || interaction.pointerType == PointerType.MENU) &&
                    interaction.inside
            }
            .forEach { interaction ->
                when (interaction.controlId) {
                    TouchControlId.REWIND -> rewind = true
                    else -> buttonMask = buttonMask or buttonMaskFor(interaction.controlId)
                }
            }
        updateState(
            currentState.copy(
                buttonMask = buttonMask,
                rewind = rewind,
            )
        )
    }

    private fun updateState(nextState: TouchControlsState) {
        if (currentState == nextState) {
            return
        }
        currentState = nextState
        onStateChanged?.invoke(nextState)
    }

    private fun buttonMaskFor(controlId: TouchControlId): Int =
        when (controlId) {
            TouchControlId.A -> TouchButtonMask.A
            TouchControlId.B -> TouchButtonMask.B
            TouchControlId.C -> TouchButtonMask.C
            TouchControlId.X -> TouchButtonMask.X
            TouchControlId.Y -> TouchButtonMask.Y
            TouchControlId.Z -> TouchButtonMask.Z
            TouchControlId.L -> TouchButtonMask.L
            TouchControlId.R -> TouchButtonMask.R
            TouchControlId.START -> TouchButtonMask.START
            else -> 0
        }

    private fun isControlPressed(controlId: TouchControlId): Boolean =
        when (controlId) {
            TouchControlId.DPAD -> currentState.dpadX != 0 || currentState.dpadY != 0
            TouchControlId.ANALOG -> abs(currentState.analogX) > 0.01f || abs(currentState.analogY) > 0.01f
            TouchControlId.MENU -> pointerInteractions.values.any {
                it.controlId == controlId && it.pointerType == PointerType.MENU && it.inside
            }

            TouchControlId.REWIND -> currentState.rewind

            else -> (currentState.buttonMask and buttonMaskFor(controlId)) != 0
        }

    /** Reverse of the draw order in [onDraw], so the editor picks whichever control is visibly on top. */
    private val editHitOrder = listOf(
        TouchControlId.REWIND,
        TouchControlId.MENU,
        TouchControlId.START,
        TouchControlId.C,
        TouchControlId.B,
        TouchControlId.A,
        TouchControlId.Z,
        TouchControlId.Y,
        TouchControlId.X,
        TouchControlId.ANALOG,
        TouchControlId.DPAD,
        TouchControlId.R,
        TouchControlId.L,
    )

    private fun hitTestControl(x: Float, y: Float): TouchControlId? =
        (if (interactionMode == InteractionMode.EDIT) editHitOrder else controlHitOrder).firstOrNull { controlId ->
            isControlAvailable(controlId) && isPointInsideControl(controlId, x, y)
        }

    private fun isControlAvailable(controlId: TouchControlId): Boolean =
        controlId != TouchControlId.REWIND || rewindAvailable

    private fun isPointInsideControl(controlId: TouchControlId, x: Float, y: Float, hitExpansion: Float = 1f): Boolean {
        val center = controlCenter(controlId)
        return when (controlId) {
            TouchControlId.DPAD -> hypot(x - center.x, y - center.y) <= dpadRadius() * hitExpansion
            TouchControlId.ANALOG -> hypot(x - center.x, y - center.y) <= analogRadius() * hitExpansion
            TouchControlId.L,
            TouchControlId.R -> {
                tempRect.set(
                    center.x - (55f * unit(controlId) * hitExpansion),
                    center.y - (18f * unit(controlId) * hitExpansion),
                    center.x + (55f * unit(controlId) * hitExpansion),
                    center.y + (18f * unit(controlId) * hitExpansion),
                )
                tempRect.contains(x, y)
            }

            TouchControlId.REWIND ->
                hypot(x - center.x, y - center.y) <= actionButtonRadius() * hitExpansion

            TouchControlId.START,
            TouchControlId.MENU -> {
                val width = if (controlId == TouchControlId.MENU) 46f * unit(controlId) else 43f * unit(controlId)
                val height = 18f * unit(controlId)
                tempRect.set(
                    center.x - (width * hitExpansion),
                    center.y - (height * hitExpansion),
                    center.x + (width * hitExpansion),
                    center.y + (height * hitExpansion),
                )
                tempRect.contains(x, y)
            }

            else -> hypot(x - center.x, y - center.y) <= faceButtonRadius(controlId) * hitExpansion
        }
    }

    private fun hasPointerOfType(pointerType: PointerType): Boolean =
        pointerInteractions.values.any { it.pointerType == pointerType }

    private fun controlCenter(controlId: TouchControlId): PointF {
        val placement = touchControlsLayout.placementFor(controlId)
        return PointF(width * placement.x, height * placement.y)
    }

    private fun clampControlCenter(controlId: TouchControlId, x: Float, y: Float): PointF {
        val (extentX, extentY) = controlExtents(controlId)
        return PointF(
            x.coerceIn(extentX, width - extentX),
            y.coerceIn(extentY, height - extentY),
        )
    }

    private fun controlExtents(controlId: TouchControlId): Pair<Float, Float> =
        when (controlId) {
            TouchControlId.DPAD -> dpadRadius() to dpadRadius()
            TouchControlId.ANALOG -> analogRadius() to analogRadius()
            TouchControlId.L,
            TouchControlId.R -> (56f * unit(controlId)) to (20f * unit(controlId))
            TouchControlId.START -> (44f * unit(controlId)) to (20f * unit(controlId))
            TouchControlId.MENU -> (48f * unit(controlId)) to (20f * unit(controlId))
            TouchControlId.REWIND -> actionButtonRadius() to actionButtonRadius()
            else -> faceButtonRadius(controlId) to faceButtonRadius(controlId)
        }

    /** One dp at this control's user-chosen size. */
    private fun unit(controlId: TouchControlId): Float = density * touchControlsLayout.effectiveScale(controlId)

    private fun faceButtonRadius(controlId: TouchControlId): Float = 29f * unit(controlId)

    private fun actionButtonRadius(): Float = 22f * unit(TouchControlId.REWIND)

    private fun dpadRadius(): Float = 58f * unit(TouchControlId.DPAD)

    private fun analogRadius(): Float = 56f * unit(TouchControlId.ANALOG)

    private fun quantizeAxis(value: Float): Int =
        when {
            value <= -0.32f -> -1
            value >= 0.32f -> 1
            else -> 0
        }

    private fun fillColorFor(baseColor: Int, pressed: Boolean): Int =
        if (pressed) {
            ColorUtils.blendARGB(baseColor, Color.WHITE, 0.18f)
        } else {
            ColorUtils.setAlphaComponent(baseColor, 215)
        }

    private fun outlineColorFor(baseColor: Int, pressed: Boolean): Int =
        if (pressed) {
            ColorUtils.blendARGB(baseColor, Color.WHITE, 0.40f)
        } else {
            ColorUtils.blendARGB(baseColor, Color.WHITE, 0.15f)
        }
}
