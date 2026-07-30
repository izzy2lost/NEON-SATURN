# Release shrinking rules.
#
# SDL3 ships consumer rules inside SDL3-3.4.0.aar (proguard.txt), but they are INCOMPLETE:
# SDLControllerManager.nativeSetupJNI() resolves joystickSetLED(IIII)V by name, and that
# method is absent from the AAR's keep list, so R8 strips it and startup dies with
#   NoSuchMethodError: no static method Lorg/libsdl/app/SDLControllerManager;.joystickSetLED
# The list is stale in general, so keep SDL's whole Java layer rather than patching it
# method by method - it is only a few hundred KB of dex and it cannot silently rot again.
-keep class org.libsdl.app.** { *; }

#
# The default proguard-android-optimize.txt keeps `native <methods>` and their declaring
# classes, so the `external fun` declarations in NativeBootstrap and EmulatorActivity are
# safe without extra rules here.

# Resolved from C++ by name via GetStaticMethodID, so R8 cannot see the call site.
# See RequestQuickActionsDialog() in app/src/main/cpp/emulator_main.cpp.
-keepclassmembers class com.izzy2lost.neonsaturn.EmulatorActivity {
    public static void requestQuickActionsFromNative();
}
