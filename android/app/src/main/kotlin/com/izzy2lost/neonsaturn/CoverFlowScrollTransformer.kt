package com.izzy2lost.neonsaturn

import androidx.recyclerview.widget.RecyclerView
import kotlin.math.abs
import kotlin.math.sign

class CoverFlowScrollTransformer : RecyclerView.OnScrollListener() {

    override fun onScrolled(recyclerView: RecyclerView, dx: Int, dy: Int) {
        applyTransforms(recyclerView)
    }

    fun applyTransforms(recyclerView: RecyclerView) {
        val rvCenter = recyclerView.width / 2f
        val density = recyclerView.context.resources.displayMetrics.density

        for (i in 0 until recyclerView.childCount) {
            val child = recyclerView.getChildAt(i)
            val childCenter = (child.left + child.right) / 2f
            val distance = abs(childCenter - rvCenter)
            // Normalize distance: 0 = center, 1 = full half-width away
            val fraction = (distance / (recyclerView.width / 2f)).coerceIn(0f, 1f)

            val scale = lerp(SCALE_CENTER, SCALE_EDGE, fraction)
            val rotY = lerp(0f, ROT_Y_EDGE, fraction) * sign(childCenter - rvCenter)
            val elev = lerp(density * ELEVATION_CENTER_DP, 0f, fraction)

            child.scaleX = scale
            child.scaleY = scale
            child.rotationY = rotY
            child.translationZ = elev
        }
    }

    private fun lerp(start: Float, end: Float, t: Float) = start + (end - start) * t

    companion object {
        private const val SCALE_CENTER = 1.08f
        private const val SCALE_EDGE = 0.72f
        private const val ROT_Y_EDGE = 38f
        private const val ELEVATION_CENTER_DP = 12f
    }
}
