package com.micah.ds13r

import android.app.Application
import com.micah.ds13r.library.GameLibrary
import com.micah.ds13r.settings.SettingsRepository

class App : Application() {
    lateinit var settings: SettingsRepository
        private set
    lateinit var library: GameLibrary
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        settings = SettingsRepository(this)
        library = GameLibrary(this)
    }

    companion object {
        lateinit var instance: App
            private set
    }
}
