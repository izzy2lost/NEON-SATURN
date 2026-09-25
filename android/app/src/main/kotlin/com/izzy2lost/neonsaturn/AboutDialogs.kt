package com.izzy2lost.neonsaturn

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.util.TypedValue
import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.annotation.StringRes
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * Credits and open-source licence screens reached from the About section of Settings.
 *
 * Licence texts live in `assets/licenses/`. When a library is added to or removed from the
 * APK (native code under vendor/, vcpkg ports, or Gradle dependencies), update [components]
 * and the files there to match.
 */
object AboutDialogs {
    const val SOURCE_URL = "https://github.com/izzy2lost/NEON-SATURN"

    private data class Credit(val name: String, val role: String, val url: String? = null)

    private data class Component(
        val name: String,
        val licence: String,
        val url: String,
        /** Shown in order, separated by a rule. */
        val assetFiles: List<String>,
    )

    private val credits = listOf(
        Credit("izzy2lost", "NEON SATURN Android port", SOURCE_URL),
        Credit(
            "StrikerX3 and the Ymir contributors",
            "Ymir, the Sega Saturn emulator whose core runs every game here",
            "https://github.com/StrikerX3/Ymir",
        ),
        Credit(
            "Support Ymir",
            "Back the upstream author on Patreon",
            "https://www.patreon.com/StrikerX3",
        ),
        Credit(
            "Kronos and Yabause authors",
            "6x xBRZ upscaling shader",
            "https://github.com/FCare/Kronos",
        ),
        Credit("The SDL team", "Windowing, audio, input and the Android activity", "https://www.libsdl.org"),
        Credit(
            "SDL_GameControllerDB contributors",
            "Gamepad mappings",
            "https://github.com/mdqinc/SDL_GameControllerDB",
        ),
        Credit("saturn-covers", "Box and jewel case artwork", "https://github.com/izzy2lost/saturn-covers"),
    )

    private val components = listOf(
        Component("NEON SATURN", "GPL-3.0", SOURCE_URL, listOf("GPL-3.0.txt")),
        Component("Ymir", "GPL-3.0", "https://github.com/StrikerX3/Ymir", listOf("GPL-3.0.txt")),
        Component(
            "6x xBRZ shader (Kronos / Yabause)",
            "GPL-2.0-or-later",
            "https://github.com/FCare/Kronos",
            listOf("xBRZ-shader-NOTICE.txt", "GPL-2.0.txt"),
        ),
        Component("SDL3", "zlib", "https://github.com/libsdl-org/SDL", listOf("SDL3-LICENSE.txt")),
        Component(
            "SDL_GameControllerDB",
            "zlib",
            "https://github.com/mdqinc/SDL_GameControllerDB",
            listOf("SDL3-LICENSE.txt"),
        ),
        Component("{fmt}", "MIT", "https://github.com/fmtlib/fmt", listOf("fmt-LICENSE.txt")),
        Component("cereal", "BSD-3-Clause", "https://github.com/USCiLab/cereal", listOf("cereal-LICENSE.txt")),
        Component("mio", "MIT", "https://github.com/StrikerX3/mio", listOf("mio-LICENSE.txt")),
        Component(
            "moodycamel::ConcurrentQueue",
            "BSD-2-Clause / Boost",
            "https://github.com/cameron314/concurrentqueue",
            listOf("concurrentqueue-LICENSE.txt"),
        ),
        Component("xxHash", "BSD-2-Clause", "https://github.com/Cyan4973/xxHash", listOf("xxHash-LICENSE.txt")),
        Component("LZ4", "BSD-2-Clause", "https://github.com/lz4/lz4", listOf("lz4-LICENSE.txt")),
        Component("libchdr", "BSD-3-Clause", "https://github.com/rtissera/libchdr", listOf("libchdr-LICENSE.txt")),
        Component("LZMA SDK", "Public domain", "https://www.7-zip.org/sdk.html", listOf("lzma-LICENSE.txt")),
        Component("miniz", "MIT", "https://github.com/richgel999/miniz", listOf("miniz-LICENSE.txt")),
        Component("Zstandard", "BSD-3-Clause", "https://github.com/facebook/zstd", listOf("zstd-LICENSE.txt")),
        Component("stb", "MIT / Public domain", "https://github.com/nothings/stb", listOf("stb-LICENSE.txt")),
        Component("dr_libs", "Public domain / MIT-0", "https://github.com/mackron/dr_libs", listOf("dr_libs-LICENSE.txt")),
        Component("AndroidX", "Apache-2.0", "https://developer.android.com/jetpack/androidx", listOf("Apache-2.0.txt")),
        Component(
            "Material Components for Android",
            "Apache-2.0",
            "https://github.com/material-components/material-components-android",
            listOf("Apache-2.0.txt"),
        ),
        Component("Coil", "Apache-2.0", "https://github.com/coil-kt/coil", listOf("Apache-2.0.txt")),
        Component("Kotlin standard library", "Apache-2.0", "https://kotlinlang.org", listOf("Apache-2.0.txt")),
    )

    fun appVersion(activity: Activity): String =
        runCatching { activity.packageManager.getPackageInfo(activity.packageName, 0).versionName }
            .getOrNull() ?: "?"

    fun showCredits(activity: Activity) {
        val list = listContainer(activity)
        for (credit in credits) {
            list.addView(entryView(activity, credit.name, credit.role, credit.url) {
                credit.url?.let { openUrl(activity, it) }
            })
        }
        list.addView(bodyText(activity, activity.getString(R.string.about_trademark_notice)).apply {
            setPadding(dp(activity, 24), dp(activity, 16), dp(activity, 24), dp(activity, 8))
            alpha = 0.7f
        })
        show(activity, R.string.about_credits, list)
    }

    fun showLicences(activity: Activity) {
        val list = listContainer(activity)
        for (component in components) {
            list.addView(entryView(activity, component.name, component.licence, component.url) {
                showLicenceText(activity, component)
            })
        }
        show(activity, R.string.about_licenses, list)
    }

    fun openSource(activity: Activity) = openUrl(activity, SOURCE_URL)

    private fun showLicenceText(activity: Activity, component: Component) {
        val text = component.assetFiles.joinToString("\n\n────────────────────\n\n") { file ->
            runCatching {
                activity.assets.open("licenses/$file").bufferedReader().use { it.readText().trimEnd() }
            }.getOrElse { "($file is missing from this build)" }
        }
        val body = TextView(activity).apply {
            this.text = text
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            setTextIsSelectable(true)
            setPadding(dp(activity, 24), dp(activity, 8), dp(activity, 24), dp(activity, 8))
        }
        MaterialAlertDialogBuilder(activity, R.style.ThemeOverlay_NeonSaturn_LibrarySettingsDialog)
            .setTitle("${component.name} · ${component.licence}")
            .setView(ScrollView(activity).apply { addView(body) })
            .setNeutralButton(R.string.about_open_website) { _, _ -> openUrl(activity, component.url) }
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun show(activity: Activity, @StringRes title: Int, list: LinearLayout) {
        MaterialAlertDialogBuilder(activity, R.style.ThemeOverlay_NeonSaturn_LibrarySettingsDialog)
            .setTitle(title)
            .setView(ScrollView(activity).apply { addView(list) })
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun listContainer(activity: Activity) = LinearLayout(activity).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, dp(activity, 8), 0, dp(activity, 8))
    }

    private fun entryView(
        activity: Activity,
        title: String,
        subtitle: String,
        url: String?,
        onClick: () -> Unit,
    ): View {
        val row = LayoutInflater.from(activity).inflate(R.layout.item_about_entry, null, false)
        row.findViewById<TextView>(R.id.aboutEntryTitle).text = title
        row.findViewById<TextView>(R.id.aboutEntrySubtitle).text = subtitle
        row.findViewById<TextView>(R.id.aboutEntryLink).apply {
            text = url?.removePrefix("https://")
            visibility = if (url != null) View.VISIBLE else View.GONE
        }
        row.setOnClickListener { onClick() }
        return row
    }

    private fun bodyText(activity: Activity, text: String) = TextView(activity).apply {
        this.text = text
        setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodySmall)
    }

    private fun openUrl(activity: Activity, url: String) {
        try {
            activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (_: ActivityNotFoundException) {
            Toast.makeText(activity, url, Toast.LENGTH_LONG).show()
        }
    }

    private fun dp(activity: Activity, value: Int): Int =
        (value * activity.resources.displayMetrics.density).toInt()
}
