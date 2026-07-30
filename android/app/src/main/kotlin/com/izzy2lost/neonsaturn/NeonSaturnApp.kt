package com.izzy2lost.neonsaturn

import android.app.Application
import androidx.appcompat.app.AppCompatDelegate

/**
 * Applies the stored theme preference before any activity inflates, so the first frame is
 * already in the right mode rather than flashing the system default and recreating.
 */
class NeonSaturnApp : Application() {
    override fun onCreate() {
        super.onCreate()
        AppCompatDelegate.setDefaultNightMode(
            BootstrapStore.nightModeFor(BootstrapStore(this).loadThemeMode())
        )
    }
}
