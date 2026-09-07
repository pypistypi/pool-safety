package ru.poolsafety.watch

import android.app.Application
import ru.poolsafety.watch.notify.Notifications

class WatchApp : Application() {
    override fun onCreate() {
        super.onCreate()
        // Каналы уведомлений заводятся один раз при первом запуске. Сделать
        // это позже, к приходу первого события, нельзя: канал, созданный в
        // момент показа, применит свои настройки только со следующего раза.
        Notifications.ensureChannels(this)
    }
}
