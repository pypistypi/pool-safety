package ru.poolsafety.watch.notify

import android.app.NotificationManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.getSystemService

// ---------------------------------------------------------------------------
//  Кнопки прямо в уведомлении о тревоге.
//
//  ЗАЧЕМ ОНИ. Телефон в кармане, тревога звучит. Чтобы её заглушить, человек
//  раньше должен был найти приложение и открыть его — а сигнал при этом
//  обрывался от любого касания шторки сам, без всякого решения. Получалось
//  худшее: звук пропал, а никто ничего не решил.
//
//  ДВЕ КНОПКИ, И РАЗНИЦА МЕЖДУ НИМИ ВАЖНА:
//
//    «Заглушить» — выключает звук ТОЛЬКО на этом телефоне. Тревога на посту
//    продолжается, уведомление остаётся на экране, сирена у оператора звучит.
//    Это для того, кто уже бежит к бассейну и кому звук больше не нужен.
//
//    «Принять» — тоже выключает звук и сразу открывает приложение на нужной
//    камере, где есть три решения: принял, ложная, помощь вызвана. Она
//    устроена иначе — не через этот приёмник, а прямым открытием экрана:
//    Android 10 и новее запрещает приложению открывать окна из фона.
//
//  НИ ОДНА ИЗ НИХ НЕ СНИМАЕТ ТРЕВОГУ. Снять её можно только осознанным
//  решением в приложении или на посту — кнопка в шторке для этого слишком
//  легко нажимается случайно.
// ---------------------------------------------------------------------------

class AlarmActionReceiver : BroadcastReceiver() {

    companion object {
        const val ACTION_MUTE = "ru.poolsafety.watch.MUTE_ALARM"
        const val EXTRA_PANEL = "panel"
    }

    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            ACTION_MUTE -> {
                Log.i("PoolSafety", "сирена заглушена с уведомления")
                AlarmSiren.stop()
                refreshNotification(context, intent)
            }

        }
    }

    /// Перерисовать уведомление без кнопки «Заглушить»: звук уже выключен, и
    /// повторное нажатие ничего не изменит, а место занимает.
    private fun refreshNotification(context: Context, intent: Intent) {
        val panel = intent.getIntExtra(EXTRA_PANEL, -1)
        val manager = context.getSystemService<NotificationManager>() ?: return
        val last = Notifications.lastAlarm ?: return
        manager.notify(
            Notifications.eventId(panel),
            Notifications.buildEvent(context, last, ru.poolsafety.watch.Prefs(context))
        )
    }
}
