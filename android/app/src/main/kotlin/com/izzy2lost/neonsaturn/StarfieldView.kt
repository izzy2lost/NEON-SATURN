package com.izzy2lost.neonsaturn

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.provider.Settings
import android.util.AttributeSet
import android.view.View
import androidx.core.content.ContextCompat
import kotlin.math.hypot
import kotlin.math.sin
import kotlin.random.Random

/**
 * Animated deep-space backdrop for the game library.
 *
 * Stars are spread over three parallax depths: distant ones are small, dim and
 * barely move, near ones are larger, brighter and drift faster. Each twinkles on
 * its own sine phase, and a meteor streaks across every few seconds.
 *
 * The palette comes from resources, so the day/night variants keep whatever is
 * drawn on top of the field readable in both themes.
 *
 * The frame loop is driven by [postInvalidateOnAnimation] and only runs while the
 * view is attached, shown and not paused — see [updateRunning]. Call [pause] and
 * [resume] from the host activity so the field stops burning frames in the
 * background.
 */
class StarfieldView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private val density = resources.displayMetrics.density
    private val random = Random.Default

    private val skyTopColor = ContextCompat.getColor(context, R.color.starfield_sky_top)
    private val skyBottomColor = ContextCompat.getColor(context, R.color.starfield_sky_bottom)
    private val starColor = ContextCompat.getColor(context, R.color.starfield_star)
    private val accentColor = ContextCompat.getColor(context, R.color.starfield_accent)
    private val nebulaPrimaryColor = ContextCompat.getColor(context, R.color.starfield_nebula_primary)
    private val nebulaSecondaryColor = ContextCompat.getColor(context, R.color.starfield_nebula_secondary)

    private val skyPaint = Paint()
    private val nebulaPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val starPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val meteorPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeWidth = 1.6f * density
    }

    private var nebulaPrimaryShader: RadialGradient? = null
    private var nebulaSecondaryShader: RadialGradient? = null
    private var nebulaPrimaryRadius = 0f
    private var nebulaSecondaryRadius = 0f
    private var nebulaPrimaryX = 0f
    private var nebulaPrimaryY = 0f
    private var nebulaSecondaryX = 0f
    private var nebulaSecondaryY = 0f

    private var stars: Array<Star> = emptyArray()
    private val meteor = Meteor()
    private var nextMeteorSeconds = 0f

    private var attached = false
    private var paused = false
    private var running = false
    private var lastFrameNanos = 0L

    /** Stops the frame loop; the last drawn frame stays on screen. */
    fun pause() {
        if (paused) return
        paused = true
        updateRunning()
    }

    /** Restarts the frame loop if the view is otherwise eligible to animate. */
    fun resume() {
        if (!paused) return
        paused = false
        updateRunning()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        attached = true
        updateRunning()
    }

    override fun onDetachedFromWindow() {
        attached = false
        updateRunning()
        super.onDetachedFromWindow()
    }

    override fun onVisibilityChanged(changedView: View, visibility: Int) {
        super.onVisibilityChanged(changedView, visibility)
        updateRunning()
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        super.onWindowVisibilityChanged(visibility)
        updateRunning()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (w <= 0 || h <= 0) return

        skyPaint.shader = LinearGradient(
            0f, 0f, 0f, h.toFloat(),
            skyTopColor, skyBottomColor,
            Shader.TileMode.CLAMP
        )

        // Two soft off-screen-ish glows give the flat gradient some depth.
        nebulaPrimaryX = w * 0.22f
        nebulaPrimaryY = h * 0.18f
        nebulaPrimaryRadius = hypot(w.toFloat(), h.toFloat()) * 0.45f
        nebulaPrimaryShader = radialGlow(
            nebulaPrimaryX, nebulaPrimaryY, nebulaPrimaryRadius,
            nebulaPrimaryColor, NEBULA_PRIMARY_ALPHA
        )

        nebulaSecondaryX = w * 0.86f
        nebulaSecondaryY = h * 0.74f
        nebulaSecondaryRadius = hypot(w.toFloat(), h.toFloat()) * 0.38f
        nebulaSecondaryShader = radialGlow(
            nebulaSecondaryX, nebulaSecondaryY, nebulaSecondaryRadius,
            nebulaSecondaryColor, NEBULA_SECONDARY_ALPHA
        )

        stars = createStars(w.toFloat(), h.toFloat())
        scheduleNextMeteor()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return

        if (running) {
            val now = System.nanoTime()
            // Clamp so a long stall (or the very first frame) can't teleport the field.
            val dt = if (lastFrameNanos == 0L) {
                0f
            } else {
                ((now - lastFrameNanos) / 1_000_000_000f).coerceIn(0f, MAX_FRAME_SECONDS)
            }
            lastFrameNanos = now
            if (dt > 0f) {
                advance(dt, w, h)
            }
        }

        canvas.drawRect(0f, 0f, w, h, skyPaint)
        drawNebulae(canvas)
        drawStars(canvas)
        drawMeteor(canvas)

        if (running) {
            postInvalidateOnAnimation()
        }
    }

    private fun updateRunning() {
        val shouldRun = attached && !paused && isShown && animatorsEnabled()
        if (shouldRun == running) return
        running = shouldRun
        // Start the next frame from a clean slate rather than measuring against
        // whenever we happened to stop.
        lastFrameNanos = 0L
        if (running) {
            postInvalidateOnAnimation()
        }
    }

    /** Honours the system "remove animations" accessibility setting. */
    private fun animatorsEnabled(): Boolean = runCatching {
        Settings.Global.getFloat(
            context.contentResolver,
            Settings.Global.ANIMATOR_DURATION_SCALE,
            1f
        ) != 0f
    }.getOrDefault(true)

    private fun advance(dt: Float, w: Float, h: Float) {
        for (star in stars) {
            star.y -= star.speed * dt
            star.x += star.drift * dt
            star.twinklePhase = (star.twinklePhase + star.twinkleRate * dt) % TWO_PI

            if (star.y < -star.radius) {
                // Recycle off the bottom edge at a fresh horizontal position.
                star.y = h + star.radius
                star.x = random.nextFloat() * w
            }
            if (star.x < -star.radius) {
                star.x = w + star.radius
            } else if (star.x > w + star.radius) {
                star.x = -star.radius
            }
        }

        if (meteor.alive) {
            meteor.x += meteor.vx * dt
            meteor.y += meteor.vy * dt
            meteor.remaining -= dt
            if (!meteor.alive) {
                scheduleNextMeteor()
            }
        } else {
            nextMeteorSeconds -= dt
            if (nextMeteorSeconds <= 0f) {
                spawnMeteor(w, h)
            }
        }
    }

    private fun drawNebulae(canvas: Canvas) {
        nebulaPrimaryShader?.let { shader ->
            nebulaPaint.shader = shader
            canvas.drawCircle(nebulaPrimaryX, nebulaPrimaryY, nebulaPrimaryRadius, nebulaPaint)
        }
        nebulaSecondaryShader?.let { shader ->
            nebulaPaint.shader = shader
            canvas.drawCircle(nebulaSecondaryX, nebulaSecondaryY, nebulaSecondaryRadius, nebulaPaint)
        }
        nebulaPaint.shader = null
    }

    private fun drawStars(canvas: Canvas) {
        for (star in stars) {
            val twinkle = TWINKLE_FLOOR + TWINKLE_RANGE * sin(star.twinklePhase)
            val alpha = (star.alpha * twinkle * 255f).toInt().coerceIn(0, 255)
            if (alpha == 0) continue

            // Set the colour first — it resets the paint's alpha channel.
            starPaint.color = if (star.accent) accentColor else starColor
            if (star.glow) {
                starPaint.alpha = alpha / 5
                canvas.drawCircle(star.x, star.y, star.radius * GLOW_RADIUS_FACTOR, starPaint)
            }
            starPaint.alpha = alpha
            canvas.drawCircle(star.x, star.y, star.radius, starPaint)
        }
    }

    private fun drawMeteor(canvas: Canvas) {
        if (!meteor.alive) return

        val speed = hypot(meteor.vx, meteor.vy)
        if (speed <= 0f) return
        val tailX = meteor.x - meteor.vx / speed * meteor.length
        val tailY = meteor.y - meteor.vy / speed * meteor.length

        // Fade in over the first fifth of the life, then back out over the rest.
        val progress = 1f - (meteor.remaining / meteor.lifetime).coerceIn(0f, 1f)
        val fade = if (progress < 0.2f) progress / 0.2f else (1f - progress) / 0.8f
        val headAlpha = (fade * 255f).toInt().coerceIn(0, 255)
        if (headAlpha == 0) return

        meteorPaint.shader = LinearGradient(
            meteor.x, meteor.y, tailX, tailY,
            Color.argb(headAlpha, Color.red(accentColor), Color.green(accentColor), Color.blue(accentColor)),
            Color.TRANSPARENT,
            Shader.TileMode.CLAMP
        )
        canvas.drawLine(meteor.x, meteor.y, tailX, tailY, meteorPaint)
        meteorPaint.shader = null
    }

    private fun createStars(w: Float, h: Float): Array<Star> {
        val areaDp = (w / density) * (h / density)
        val count = (areaDp / DP2_PER_STAR).toInt().coerceIn(MIN_STARS, MAX_STARS)

        return Array(count) {
            // depth 0 = far away, 1 = close.
            val depth = random.nextFloat()
            Star().apply {
                x = random.nextFloat() * w
                y = random.nextFloat() * h
                radius = lerp(0.6f, 1.9f, depth) * density
                speed = lerp(2f, 15f, depth) * density
                drift = (random.nextFloat() - 0.5f) * lerp(0.5f, 3f, depth) * density
                alpha = lerp(0.30f, 0.95f, depth)
                twinklePhase = random.nextFloat() * TWO_PI
                twinkleRate = lerp(0.5f, 2.2f, random.nextFloat())
                accent = random.nextFloat() < ACCENT_STAR_CHANCE
                glow = depth > 0.82f
            }
        }
    }

    private fun spawnMeteor(w: Float, h: Float) {
        // Enter from the top edge and fall down-left, like the classic screensaver.
        val slant = lerp(0.6f, 1.0f, random.nextFloat())
        val fallSpeed = lerp(520f, 880f, random.nextFloat()) * density
        meteor.x = w * lerp(0.25f, 1.15f, random.nextFloat())
        meteor.y = -h * 0.05f
        meteor.vx = -fallSpeed * slant
        meteor.vy = fallSpeed
        meteor.length = lerp(70f, 150f, random.nextFloat()) * density
        meteor.lifetime = lerp(0.8f, 1.4f, random.nextFloat())
        meteor.remaining = meteor.lifetime
    }

    private fun scheduleNextMeteor() {
        nextMeteorSeconds = lerp(METEOR_MIN_GAP_SECONDS, METEOR_MAX_GAP_SECONDS, random.nextFloat())
    }

    private fun lerp(start: Float, end: Float, t: Float) = start + (end - start) * t

    private fun radialGlow(cx: Float, cy: Float, radius: Float, color: Int, alpha: Float) =
        RadialGradient(
            cx, cy, radius,
            Color.argb(
                (alpha * 255f).toInt(),
                Color.red(color),
                Color.green(color),
                Color.blue(color)
            ),
            Color.TRANSPARENT,
            Shader.TileMode.CLAMP
        )

    private class Star {
        var x = 0f
        var y = 0f
        var radius = 0f
        var speed = 0f
        var drift = 0f
        var alpha = 0f
        var twinklePhase = 0f
        var twinkleRate = 0f
        var accent = false
        var glow = false
    }

    private class Meteor {
        var x = 0f
        var y = 0f
        var vx = 0f
        var vy = 0f
        var length = 0f
        var lifetime = 1f
        var remaining = 0f

        val alive: Boolean get() = remaining > 0f
    }

    private companion object {
        const val TWO_PI = (Math.PI * 2).toFloat()
        const val MAX_FRAME_SECONDS = 1f / 15f

        /** One star per this many square dp of surface, clamped to the bounds below. */
        const val DP2_PER_STAR = 2200f
        const val MIN_STARS = 60
        const val MAX_STARS = 200

        const val TWINKLE_FLOOR = 0.62f
        const val TWINKLE_RANGE = 0.38f
        const val GLOW_RADIUS_FACTOR = 3.4f
        const val ACCENT_STAR_CHANCE = 0.16f

        const val NEBULA_PRIMARY_ALPHA = 0.16f
        const val NEBULA_SECONDARY_ALPHA = 0.11f

        const val METEOR_MIN_GAP_SECONDS = 5f
        const val METEOR_MAX_GAP_SECONDS = 14f
    }
}
