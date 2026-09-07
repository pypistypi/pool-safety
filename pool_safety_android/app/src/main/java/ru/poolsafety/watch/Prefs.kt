package ru.poolsafety.watch

import android.content.Context
import ru.poolsafety.watch.net.EventKind
import ru.poolsafety.watch.net.Protocol

// ---------------------------------------------------------------------------
//  Настройки приложения.
//
//  ЧТО ЗДЕСЬ ЕСТЬ И ЧЕГО НЕТ. Есть адрес компьютера, звук и выбор того, о чём
//  уведомлять. Нет ни одной настройки, способной выключить тревогу совсем:
//  оператор может убрать звук у присутствия и у внимания, но тревога звучит
//  всегда. Настройка, которой можно тихо выключить сирену, рано или поздно
//  окажется выключенной — и никто не вспомнит, когда.
// ---------------------------------------------------------------------------

class Prefs(context: Context) {

    private val store = context.getSharedPreferences("pool_safety", Context.MODE_PRIVATE)

    /// Адрес компьютера. Пусто — ещё не искали и не вводили.
    var host: String
        get() = store.getString(KEY_HOST, "") ?: ""
        set(value) = store.edit().putString(KEY_HOST, value.trim()).apply()

    var port: Int
        get() = store.getInt(KEY_PORT, Protocol.DEFAULT_PORT)
        set(value) = store.edit().putInt(KEY_PORT, value).apply()

    val isConfigured: Boolean get() = host.isNotBlank()

    /// Звук уведомлений вообще.
    var soundEnabled: Boolean
        get() = store.getBoolean(KEY_SOUND, true)
        set(value) = store.edit().putBoolean(KEY_SOUND, value).apply()

    /// Громкость — от 0 до 1. Применяется к присутствию и вниманию; тревога
    /// звучит на громкости, назначенной каналу уведомлений системы.
    var volume: Float
        get() = store.getFloat(KEY_VOLUME, 0.8f).coerceIn(0f, 1f)
        set(value) = store.edit().putFloat(KEY_VOLUME, value.coerceIn(0f, 1f)).apply()

    /// Уведомлять ли о появлении человека.
    var notifyPresence: Boolean
        get() = store.getBoolean(KEY_PRESENCE, true)
        set(value) = store.edit().putBoolean(KEY_PRESENCE, value).apply()

    /// Уведомлять ли о признаках, требующих взгляда.
    var notifyAttention: Boolean
        get() = store.getBoolean(KEY_ATTENTION, true)
        set(value) = store.edit().putBoolean(KEY_ATTENTION, value).apply()

    /// Уведомлять ли о том, что зона опустела. По умолчанию нет: это событие
    /// спокойное, и дёргать им оператора незачем.
    var notifyClear: Boolean
        get() = store.getBoolean(KEY_CLEAR, false)
        set(value) = store.edit().putBoolean(KEY_CLEAR, value).apply()

    /// Надо ли показывать уведомление о таком событии.
    ///
    /// ТРЕВОГА НЕ СПРАШИВАЕТ. Для неё ответ всегда «да», и ветки, способной
    /// вернуть «нет», здесь нет вовсе — не по недосмотру, а нарочно.
    fun shouldNotify(kind: EventKind): Boolean = when (kind) {
        EventKind.Alarm -> true
        EventKind.Attention -> notifyAttention
        EventKind.Presence -> notifyPresence
        EventKind.Clear -> notifyClear
        EventKind.Unknown -> false
    }

    private companion object {
        const val KEY_HOST = "host"
        const val KEY_PORT = "port"
        const val KEY_SOUND = "sound"
        const val KEY_VOLUME = "volume"
        const val KEY_PRESENCE = "notify_presence"
        const val KEY_ATTENTION = "notify_attention"
        const val KEY_CLEAR = "notify_clear"
    }
}
