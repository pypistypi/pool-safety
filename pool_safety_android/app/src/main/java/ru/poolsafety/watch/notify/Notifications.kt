package ru.poolsafety.watch.notify

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.media.RingtoneManager
import androidx.core.app.NotificationCompat
import androidx.core.content.getSystemService
import ru.poolsafety.watch.MainActivity
import ru.poolsafety.watch.Prefs
import ru.poolsafety.watch.R
import ru.poolsafety.watch.net.EventKind
import ru.poolsafety.watch.net.WatchEvent

// ---------------------------------------------------------------------------
//  Уведомления.
//
//  ТРИ КАНАЛА, А НЕ ОДИН. Android даёт настраивать важность отдельно для
//  каждого канала, и это ровно то разделение, на котором стоит вся система:
//  присутствие человека — спокойное сообщение, тревога — то, что обязано
//  разбудить. Свалив их в один канал, мы отдали бы это решение случаю: убавив
//  громкость надоевшему присутствию, оператор убавил бы её и тревоге.
//
//  ЗВУКИ БЕРЁМ СИСТЕМНЫЕ — так просил заказчик. Своих файлов нет: системный
//  звук оператор узнаёт, он одинаково слышен на любом телефоне и не пропадёт
//  при обновлении приложения.
//
//  ВАЖНОСТЬ КАНАЛА ЗАДАЁТСЯ ОДИН РАЗ. Android не позволяет повысить её потом
//  из кода — это защита пользователя от навязчивых приложений, и обходить её
//  мы не будем. Поэтому каналу тревоги важность задана наибольшей сразу.
// ---------------------------------------------------------------------------

object Notifications {

    /// Канал тревоги.
    ///
    /// ИМЯ С ДВОЙКОЙ — НЕ ОПЕЧАТКА. Настройки канала (звук, важность) Android
    /// разрешает задать только при создании: потом их меняет пользователь, а
    /// приложение — нет. Прежний канал звучал сам, одиночным сигналом, и
    /// оборвать его касанием шторки мог кто угодно. Чтобы отдать звук своей
    /// сирене, канал понадобилось завести заново, под новым именем.
    const val CHANNEL_ALARM = "alarm_v2"
    const val CHANNEL_ATTENTION = "attention"
    const val CHANNEL_PRESENCE = "presence"
    const val CHANNEL_SERVICE = "service"

    /// Постоянное уведомление службы. Номер занят навсегда — его нельзя
    /// использовать под события, иначе служба потеряет своё уведомление и
    /// будет убита системой.
    const val ID_SERVICE = 1

    /// События нумеруются по панели, чтобы новое сообщение о той же зоне
    /// заменяло прежнее, а не копилось столбиком.
    fun eventId(panelId: Int): Int = 100 + panelId.coerceAtLeast(0)

    /// Последняя тревога — чтобы перерисовать уведомление после нажатия
    /// кнопки, не выдумывая текст заново.
    @Volatile
    var lastAlarm: WatchEvent? = null

    fun ensureChannels(context: Context) {
        val manager = context.getSystemService<NotificationManager>() ?: return

        val noticeSound = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)

        val alarm = NotificationChannel(
            CHANNEL_ALARM,
            "Тревога",
            NotificationManager.IMPORTANCE_HIGH
        ).apply {
            description = "Найдено опасное положение. Требуется посмотреть немедленно."
            // Ни звука, ни вибрации от самого уведомления: и то и другое даёт
            // AlarmSiren — и держит, пока человек не примет решение.
            enableVibration(false)
            setSound(null, null)
            lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            setBypassDnd(true)
        }

        val attention = NotificationChannel(
            CHANNEL_ATTENTION,
            "Внимание",
            NotificationManager.IMPORTANCE_DEFAULT
        ).apply {
            description = "Признак, на который стоит взглянуть."
            enableVibration(true)
            setSound(noticeSound, null)
        }

        val presence = NotificationChannel(
            CHANNEL_PRESENCE,
            "Появление человека",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "В зоне наблюдения появились люди."
            enableVibration(false)
            setSound(noticeSound, null)
        }

        val service = NotificationChannel(
            CHANNEL_SERVICE,
            "Связь с постом",
            NotificationManager.IMPORTANCE_MIN
        ).apply {
            description = "Показывает, что приложение на связи и получает события."
            setShowBadge(false)
        }

        manager.createNotificationChannels(listOf(alarm, attention, presence, service))
    }

    fun channelFor(kind: EventKind): String = when (kind) {
        EventKind.Alarm -> CHANNEL_ALARM
        EventKind.Attention -> CHANNEL_ATTENTION
        else -> CHANNEL_PRESENCE
    }

    /// Уведомление о событии.
    fun buildEvent(context: Context, event: WatchEvent, prefs: Prefs): Notification {
        val open = PendingIntent.getActivity(
            context,
            eventId(event.panelId),
            Intent(context, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
                putExtra(MainActivity.EXTRA_PANEL, event.panelId)
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val title = when (event.kind) {
            EventKind.Alarm -> "ТРЕВОГА — ${event.zone}"
            EventKind.Attention -> "Внимание — ${event.zone}"
            EventKind.Presence -> "Человек в зоне — ${event.zone}"
            EventKind.Clear -> "Зона пуста — ${event.zone}"
            EventKind.Unknown -> event.zone
        }

        val text = event.details.ifBlank { event.title }.ifBlank {
            when (event.kind) {
                EventKind.Presence -> "В зоне наблюдения появились люди."
                EventKind.Clear -> "Людей в зоне больше нет."
                else -> "Посмотрите на камеру."
            }
        }

        val builder = NotificationCompat.Builder(context, channelFor(event.kind))
            .setSmallIcon(R.drawable.ic_watch)
            .setContentTitle(title)
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setContentIntent(open)
            .setAutoCancel(true)
            .setCategory(
                if (event.kind == EventKind.Alarm) NotificationCompat.CATEGORY_ALARM
                else NotificationCompat.CATEGORY_STATUS
            )

        if (event.kind == EventKind.Alarm) {
            lastAlarm = event

            // Тревога разворачивается поверх экрана сама. Оператор мог
            // отложить телефон экраном вверх — и не увидеть строку в шторке.
            builder.setPriority(NotificationCompat.PRIORITY_MAX)
            builder.setFullScreenIntent(open, true)

            // Смахнуть тревогу нельзя. Она уходит с экрана только когда её
            // сняли на посту или с телефона — случайное движение пальцем не
            // должно убирать сообщение о беде.
            builder.setOngoing(true)
            builder.setAutoCancel(false)

            // «Принять» — заглушить и открыть приложение с решениями.
            //
            // ОТКРЫВАЕТ ЭКРАН НАПРЯМУЮ, А НЕ ЧЕРЕЗ ПРИЁМНИК. Начиная с
            // Android 10 приложению запрещено открывать окна из фона: приёмник
            // отрабатывал, писал в журнал, а экран не появлялся. Нажатие же на
            // само уведомление считается действием человека — и окно
            // открывается. Сирену в этом случае глушит сам экран при запуске.
            builder.addAction(
                0,
                "Принять",
                PendingIntent.getActivity(
                    context,
                    eventId(event.panelId) + 500,
                    Intent(context, MainActivity::class.java).apply {
                        flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                            Intent.FLAG_ACTIVITY_CLEAR_TOP
                        putExtra(MainActivity.EXTRA_PANEL, event.panelId)
                        putExtra(MainActivity.EXTRA_MUTE, true)
                    },
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
                )
            )

            // «Заглушить» — выключить звук ТОЛЬКО на этом телефоне. Тревога на
            // посту продолжается. Кнопка исчезает, когда сирена уже молчит:
            // нажимать её второй раз незачем.
            if (AlarmSiren.isSounding) {
                builder.addAction(
                    0,
                    "Заглушить",
                    actionIntent(context, AlarmActionReceiver.ACTION_MUTE, event.panelId)
                )
            }
        } else {
            builder.setPriority(
                if (event.kind == EventKind.Attention) NotificationCompat.PRIORITY_DEFAULT
                else NotificationCompat.PRIORITY_LOW
            )
        }

        // Тревога и так беззвучна на уровне канала — звук даёт сирена.
        // Остальное молчит, если оператор выключил звук в настройках.
        if (!prefs.soundEnabled && event.kind != EventKind.Alarm) {
            builder.setSilent(true)
        }

        return builder.build()
    }

    private fun actionIntent(context: Context, action: String, panelId: Int): PendingIntent {
        val intent = Intent(context, AlarmActionReceiver::class.java).apply {
            this.action = action
            putExtra(AlarmActionReceiver.EXTRA_PANEL, panelId)
        }
        return PendingIntent.getBroadcast(
            context,
            action.hashCode() + panelId,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
    }

    /// Постоянное уведомление службы: одна строка о состоянии связи.
    fun buildService(context: Context, text: String): Notification {
        val open = PendingIntent.getActivity(
            context,
            0,
            Intent(context, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(context, CHANNEL_SERVICE)
            .setSmallIcon(R.drawable.ic_watch)
            .setContentTitle("Наблюдение за бассейном")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .build()
    }
}
