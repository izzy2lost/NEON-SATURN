package com.izzy2lost.neonsaturn

enum class TouchControlId(
    val label: String,
    val preferenceKey: String
) {
    DPAD("D-Pad", "dpad"),
    ANALOG("Stick", "analog"),
    START("Start", "start"),
    MENU("Menu", "menu"),
    A("A", "a"),
    B("B", "b"),
    C("C", "c"),
    X("X", "x"),
    Y("Y", "y"),
    Z("Z", "z"),
    L("L", "l"),
    R("R", "r"),
}

data class TouchControlPlacement(
    val x: Float,
    val y: Float,
)

data class TouchControlsLayout(
    val placements: Map<TouchControlId, TouchControlPlacement>
) {
    fun placementFor(controlId: TouchControlId): TouchControlPlacement =
        placements[controlId] ?: DEFAULT.placementFor(controlId)

    fun withPlacement(controlId: TouchControlId, placement: TouchControlPlacement): TouchControlsLayout =
        copy(placements = placements + (controlId to placement))

    companion object {
        val DEFAULT = TouchControlsLayout(
            mapOf(
                TouchControlId.DPAD to TouchControlPlacement(0.17f, 0.72f),
                TouchControlId.ANALOG to TouchControlPlacement(0.34f, 0.82f),
                TouchControlId.START to TouchControlPlacement(0.50f, 0.81f),
                TouchControlId.MENU to TouchControlPlacement(0.59f, 0.66f),
                TouchControlId.A to TouchControlPlacement(0.73f, 0.77f),
                TouchControlId.B to TouchControlPlacement(0.81f, 0.67f),
                TouchControlId.C to TouchControlPlacement(0.91f, 0.58f),
                TouchControlId.X to TouchControlPlacement(0.68f, 0.57f),
                TouchControlId.Y to TouchControlPlacement(0.77f, 0.49f),
                TouchControlId.Z to TouchControlPlacement(0.86f, 0.41f),
                TouchControlId.L to TouchControlPlacement(0.17f, 0.17f),
                TouchControlId.R to TouchControlPlacement(0.83f, 0.17f),
            )
        )
    }
}

data class TouchControlsState(
    val buttonMask: Int = 0,
    val dpadX: Int = 0,
    val dpadY: Int = 0,
    val analogX: Float = 0f,
    val analogY: Float = 0f,
)

object TouchButtonMask {
    const val A = 1 shl 0
    const val B = 1 shl 1
    const val C = 1 shl 2
    const val X = 1 shl 3
    const val Y = 1 shl 4
    const val Z = 1 shl 5
    const val L = 1 shl 6
    const val R = 1 shl 7
    const val START = 1 shl 8
}
