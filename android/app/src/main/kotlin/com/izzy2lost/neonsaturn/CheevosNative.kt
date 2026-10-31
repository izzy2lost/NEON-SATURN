package com.izzy2lost.neonsaturn

/** RetroAchievements in libmain (cpp/cheevos_session.cpp). The session itself starts with the game. */
object CheevosNative {
    init {
        System.loadLibrary("SDL3")
        System.loadLibrary("main")
    }

    /** What the running game's session has queued for the player; see Achievements.Popups. */
    fun messages(): Array<String> = nativeMessages()

    /** Header row (state, game, game badge, unlocked, total, points, total points), then one row per achievement. */
    fun list(): Array<String> = nativeList()

    /** Blocking: { username, token, display name }, or { "", reason } on failure. */
    fun login(user: String, password: String): Array<String> = nativeLogin(user, password)

    private external fun nativeMessages(): Array<String>
    private external fun nativeList(): Array<String>
    private external fun nativeLogin(user: String, password: String): Array<String>
}
