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
        val cameraDist = density * CAMERA_DISTANCE_DP
        val tuckPx = density * TUCK_DP

        for (i in 0 until recyclerView.childCount) {
            val child = recyclerView.getChildAt(i)
            val childWidth = child.width.toFloat().coerceAtLeast(1f)
            val childCenter = (child.left + child.right) / 2f
            val signedOffset = childCenter - rvCenter
            // Normalize by item width so the falloff is consistent regardless of screen size.
            // FALLOFF_CARDS controls how many cards out reaches full-edge transform.
            val raw = abs(signedOffset) / (childWidth * FALLOFF_CARDS)
            val linear = raw.coerceIn(0f, 1f)
            // Smoothstep easing — softer near center, firmer toward edges.
            val eased = linear * linear * (3f - 2f * linear)
            val side = sign(signedOffset)

            // Soften perspective so steep rotationY doesn't clip the near edge.
            child.cameraDistance = cameraDist

            val scale = lerp(SCALE_CENTER, SCALE_EDGE, eased)
            child.scaleX = scale
            child.scaleY = scale
            child.rotationY = lerp(0f, ROT_Y_EDGE, eased) * side
            // Pull side cards inward for the classic stacked overlap.
            child.translationX = -side * eased * tuckPx
            child.translationZ = lerp(density * ELEVATION_CENTER_DP, 0f, eased)
            // Recede neighbors slightly so the focused cover pops.
            child.alpha = lerp(1f, ALPHA_EDGE, eased)
        }
    }

    private fun lerp(start: Float, end: Float, t: Float) = start + (end - start) * t

    companion object {
        private const val SCALE_CENTER = 1.12f
        private const val SCALE_EDGE = 0.78f
        private const val ROT_Y_EDGE = 52f
        private const val ELEVATION_CENTER_DP = 16f
        private const val CAMERA_DISTANCE_DP = 8000f
        private const val TUCK_DP = 32f
        private const val ALPHA_EDGE = 0.55f
        private const val FALLOFF_CARDS = 1.25f
    }
}
