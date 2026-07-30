package com.izzy2lost.neonsaturn

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.Shader
import android.util.AttributeSet
import android.widget.FrameLayout

/**
 * Fades its children out towards the bottom edge by masking them with a vertical
 * alpha gradient, used for the coverflow reflection.
 *
 * The alternative — overlaying a gradient that ends in the background colour —
 * only works on a flat background; over [StarfieldView] it would paint a solid
 * block across the stars.
 */
class ReflectionFadeLayout @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : FrameLayout(context, attrs, defStyleAttr) {

    private val fadePaint = Paint().apply {
        xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_IN)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (w <= 0 || h <= 0) return
        fadePaint.shader = LinearGradient(
            0f, 0f, 0f, h.toFloat(),
            Color.BLACK, Color.TRANSPARENT,
            Shader.TileMode.CLAMP
        )
    }

    override fun dispatchDraw(canvas: Canvas) {
        val shader = fadePaint.shader
        if (shader == null) {
            super.dispatchDraw(canvas)
            return
        }

        val w = width.toFloat()
        val h = height.toFloat()
        val layer = canvas.saveLayer(0f, 0f, w, h, null)
        super.dispatchDraw(canvas)
        canvas.drawRect(0f, 0f, w, h, fadePaint)
        canvas.restoreToCount(layer)
    }
}
