package ru.poolsafety.watch.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import ru.poolsafety.watch.Prefs

// ---------------------------------------------------------------------------
//  Возобновление наблюдения после перезагрузки телефона.
//
//  ЗАЧЕМ. Телефон перезагружается сам: обновление системы ночью, разрядка,
//  сбой. Если после этого приложение молчит, пока оператор не откроет его
//  руками, то наблюдение выключено — а никто об этом не знает. Это худший вид
//  отказа: он не виден.
//
//  ЗАПУСКАЕМСЯ ТОЛЬКО ЕСЛИ ЕСТЬ КУДА ПОДКЛЮЧАТЬСЯ. Пока компьютер не выбран,
//  служба всё равно ничего не сделает, а постоянное уведомление у человека,
//  который приложение ещё не настроил, вызовет только желание его удалить.
// ---------------------------------------------------------------------------

class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action
        if (action != Intent.ACTION_BOOT_COMPLETED &&
            action != Intent.ACTION_MY_PACKAGE_REPLACED
        ) {
            return
        }

        val prefs = Prefs(context)
        if (!prefs.isConfigured) {
            Log.i("PoolSafety", "после загрузки: компьютер не выбран, службу не поднимаем")
            return
        }
        if (prefs.manuallyDisconnected) {
            Log.i("PoolSafety", "после загрузки: отключено вручную, службу не поднимаем")
            return
        }

        Log.i("PoolSafety", "после загрузки: поднимаем наблюдение")
        runCatching { WatchService.start(context) }
            .onFailure { Log.w("PoolSafety", "не удалось поднять службу: ${it.message}") }
    }
}
