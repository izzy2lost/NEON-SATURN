package com.izzy2lost.neonsaturn

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PointF
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
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
            invalidate()
        }

    var onStateChanged: ((TouchControlsState) -> Unit)? = null
    var onLayoutChanged: ((TouchControlsLayout) -> Unit)? = null
    var onMenuPressed: (() -> Unit)? = null

    var inputSuspended: Boolean = false
        set(value) {
            field = value
            if (value) {
                resetRuntimeState()
            }
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
    private var currentState = TouchControlsState()

    private val controlHitOrder = listOf(
        TouchControlId.MENU,
        TouchControlId.START,
        TouchControlId.L,
        TouchControlId.R,
        TouchControlId.Z,
        TouchControlId.Y,
        TouchControlId.X,
        TouchControlId.C,
        TouchControlId.B,
        TouchControlId.A,
        TouchControlId.DPAD,
        TouchControlId.ANALOG,
    )

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
                val x = event.getX(index)
                val y = event.getY(index)
                val controlId = hitTestControl(x, y) ?: return false
                val center = controlCenter(controlId)
                pointerInteractions[event.getPointerId(index)] = PointerInteraction(
                    pointerType = PointerType.EDIT,
                    controlId = controlId,
                    offsetX = center.x - x,
                    offsetY = center.y - y,
                )
                invalidate()
                true
            }

            MotionEvent.ACTION_MOVE -> {
                var handled = false
                for (index in 0 until event.pointerCount) {
                    val pointerId = event.getPointerId(index)
                    val interaction = pointerInteractions[pointerId] ?: continue
                    if (interaction.pointerType != PointerType.EDIT) {
                        continue
                    }

                    val clampedPoint = clampControlCenter(
                        controlId = interaction.controlId,
                        x = event.getX(index) + interaction.offsetX,
                        y = event.getY(index) + interaction.offsetY,
                    )
                    val updatedLayout = touchControlsLayout.withPlacement(
                        interaction.controlId,
                        TouchControlPlacement(
                            x = (clampedPoint.x / width.toFloat()).coerceIn(0f, 1f),
                            y = (clampedPoint.y / height.toFloat()).coerceIn(0f, 1f),
                        ),
                    )
                    touchControlsLayout = updatedLayout
                    onLayoutChanged?.invoke(updatedLayout)
                    handled = true
                }
                handled
            }

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP -> {
                pointerInteractions.remove(event.getPointerId(event.actionIndex)) != null
            }

            MotionEvent.ACTION_CANCEL -> {
                val handled = pointerInteractions.isNotEmpty()
                pointerInteractions.clear()
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
            // Portrait: game fills full width at 4:3, anchored at the top
            val gameHeight = width * (3f / 4f)
            tempRect.set(0f, 0f, width.toFloat(), gameHeight)
        }
        canvas.drawRoundRect(tempRect, 24f * density, 24f * density, guideFillPaint)
        canvas.drawRoundRect(tempRect, 24f * density, 24f * density, guidePaint)

        textPaint.textSize = 14f * density
        textPaint.color = Color.argb(180, 255, 255, 255)
        val textY = tempRect.centerY() - ((textPaint.descent() + textPaint.ascent()) / 2f)
        canvas.drawText("Game image area", tempRect.centerX(), textY, textPaint)
    }

    private fun drawEditorInstruction(canvas: Canvas) {
        textPaint.textSize = 15f * density
        textPaint.color = Color.argb(220, 255, 255, 255)
        canvas.drawText("Drag any control to reposition it", width / 2f, 28f * density, textPaint)
    }

    private fun drawRoundButton(canvas: Canvas, controlId: TouchControlId, color: Int, textColor: Int = Color.WHITE) {
        val center = controlCenter(controlId)
        val radius = faceButtonRadius()
        val pressed = isControlPressed(controlId)
        fillPaint.color = fillColorFor(color, pressed)
        strokePaint.color = outlineColorFor(color, pressed)
        canvas.drawCircle(center.x, center.y, radius, fillPaint)
        canvas.drawCircle(center.x, center.y, radius, strokePaint)

        textPaint.textSize = 23f * density
        textPaint.color = textColor
        val baseline = center.y - ((textPaint.descent() + textPaint.ascent()) / 2f)
        canvas.drawText(controlId.label, center.x, baseline, textPaint)

        if (interactionMode == InteractionMode.EDIT) {
            drawCaption(canvas, controlId, center.x, center.y + radius + (18f * density))
        }
    }

    private fun drawCapsuleButton(canvas: Canvas, controlId: TouchControlId, color: Int) {
        val center = controlCenter(controlId)
        val width = if (controlId == TouchControlId.MENU) 92f * density else 86f * density
        val height = 34f * density
        tempRect.set(center.x - (width / 2f), center.y - (height / 2f), center.x + (width / 2f), center.y + (height / 2f))
        val pressed = isControlPressed(controlId)
        fillPaint.color = fillColorFor(color, pressed)
        strokePaint.color = outlineColorFor(color, pressed)
        canvas.drawRoundRect(tempRect, height / 2f, height / 2f, fillPaint)
        canvas.drawRoundRect(tempRect, height / 2f, height / 2f, strokePaint)

        textPaint.textSize = if (controlId == TouchControlId.MENU) 13f * density else 15f * density
        textPaint.color = Color.WHITE
        val baseline = center.y - ((textPaint.descent() + textPaint.ascent()) / 2f)
        canvas.drawText(controlId.label.uppercase(), center.x, baseline, textPaint)

        if (interactionMode == InteractionMode.EDIT) {
            drawCaption(canvas, controlId, center.x, tempRect.bottom + (16f * density))
        }
    }

    private fun drawShoulderButton(canvas: Canvas, controlId: TouchControlId, color: Int) {
        val center = controlCenter(controlId)
        val width = 110f * density
        val height = 30f * density
        tempRect.set(center.x - (width / 2f), center.y - (height / 2f), center.x + (width / 2f), center.y + (height / 2f))
        val pressed = isControlPressed(controlId)
        fillPaint.color = fillColorFor(color, pressed)
        strokePaint.color = outlineColorFor(color, pressed)
        canvas.drawRoundRect(tempRect, 18f * density, 18f * density, fillPaint)
        canvas.drawRoundRect(tempRect, 18f * density, 18f * density, strokePaint)

        textPaint.textSize = 18f * density
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
        pointerInteractions.values
            .filter { interaction ->
                (interaction.pointerType == PointerType.BUTTON || interaction.pointerType == PointerType.MENU) &&
                    interaction.inside
            }
            .forEach { interaction ->
                buttonMask = buttonMask or buttonMaskFor(interaction.controlId)
            }
        updateState(currentState.copy(buttonMask = buttonMask))
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

            else -> (currentState.buttonMask and buttonMaskFor(controlId)) != 0
        }

    private fun hitTestControl(x: Float, y: Float): TouchControlId? =
        controlHitOrder.firstOrNull { controlId ->
            isPointInsideControl(controlId, x, y)
        }

    private fun isPointInsideControl(controlId: TouchControlId, x: Float, y: Float, hitExpansion: Float = 1f): Boolean {
        val center = controlCenter(controlId)
        return when (controlId) {
            TouchControlId.DPAD -> hypot(x - center.x, y - center.y) <= dpadRadius() * hitExpansion
            TouchControlId.ANALOG -> hypot(x - center.x, y - center.y) <= analogRadius() * hitExpansion
            TouchControlId.L,
            TouchControlId.R -> {
                tempRect.set(
                    center.x - (55f * density * hitExpansion),
                    center.y - (18f * density * hitExpansion),
                    center.x + (55f * density * hitExpansion),
                    center.y + (18f * density * hitExpansion),
                )
                tempRect.contains(x, y)
            }

            TouchControlId.START,
            TouchControlId.MENU -> {
                val width = if (controlId == TouchControlId.MENU) 46f * density else 43f * density
                val height = 18f * density
                tempRect.set(
                    center.x - (width * hitExpansion),
                    center.y - (height * hitExpansion),
                    center.x + (width * hitExpansion),
                    center.y + (height * hitExpansion),
                )
                tempRect.contains(x, y)
            }

            else -> hypot(x - center.x, y - center.y) <= faceButtonRadius() * hitExpansion
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
            TouchControlId.R -> (56f * density) to (20f * density)
            TouchControlId.START -> (44f * density) to (20f * density)
            TouchControlId.MENU -> (48f * density) to (20f * density)
            else -> faceButtonRadius() to faceButtonRadius()
        }

    private fun faceButtonRadius(): Float = 29f * density

    private fun dpadRadius(): Float = 58f * density

    private fun analogRadius(): Float = 56f * density

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
