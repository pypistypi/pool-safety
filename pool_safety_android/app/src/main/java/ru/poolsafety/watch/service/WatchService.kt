package ru.poolsafety.watch.service

import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.content.getSystemService
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import ru.poolsafety.watch.Prefs
import ru.poolsafety.watch.notify.AlarmSiren
import ru.poolsafety.watch.notify.Notifications
import ru.poolsafety.watch.net.AlarmCommand
import ru.poolsafety.watch.net.AlarmPhase
import ru.poolsafety.watch.net.Connection
import ru.poolsafety.watch.net.EventClient
import ru.poolsafety.watch.net.EventKind
import ru.poolsafety.watch.net.Panel
import ru.poolsafety.watch.net.WatchEvent

// ---------------------------------------------------------------------------
//  Служба, которая держит связь с компьютером.
//
//  ПОЧЕМУ СЛУЖБА, А НЕ ЭКРАН. Оператор не будет держать приложение открытым:
//  он положит телефон в карман. Связь, живущая в окне, умрёт вместе с ним, и
//  тревога не придёт — а именно ради неё всё и затевалось. Служба переднего
//  плана — единственный способ, которым Android разрешает приложению
//  оставаться на связи, и постоянное уведомление — плата за это. Плата
//  честная: строка «на связи» заодно и показывает оператору, что наблюдение
//  работает.
//
//  ЧТО СЛУЖБА НЕ ДЕЛАЕТ. Она не показывает видео. Поток с камеры включается
//  только тогда, когда на него смотрят: держать четыре видеопотока в кармане
//  означало бы сажать батарею и нагружать сеть впустую.
// ---------------------------------------------------------------------------

class WatchService : Service() {

    companion object {
        const val ACTION_START = "ru.poolsafety.watch.START"
        const val ACTION_STOP = "ru.poolsafety.watch.STOP"

        /// Живое состояние службы — его читает экран.
        ///
        /// Разделяемое поле, а не привязка через binder: экран и служба живут
        /// в одном процессе, а связывание ради чтения двух значений добавило
        /// бы жизненный цикл, который пришлось бы отлаживать.
        val connection = MutableStateFlow<Connection>(Connection.Offline)
        val panels = MutableStateFlow<List<Panel>>(emptyList())
        val lastEvent = MutableStateFlow<WatchEvent?>(null)
        val alarmPhase = MutableStateFlow(AlarmPhase.Idle)
        val alarmPanel = MutableStateFlow(-1)

        /// Отправить решение по тревоге. Живёт здесь, потому что связь держит
        /// служба, а нажимают кнопку на экране.
        @Volatile
        var sendCommand: ((AlarmCommand) -> Unit)? = null

        fun start(context: Context) {
            val intent = Intent(context, WatchService::class.java).setAction(ACTION_START)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            context.startService(
                Intent(context, WatchService::class.java).setAction(ACTION_STOP)
            )
        }
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private lateinit var prefs: Prefs
    private lateinit var client: EventClient
    private var wakeLock: PowerManager.WakeLock? = null

    /// К кому подключены прямо сейчас. Пусто — связи нет.
    private var watching: Pair<String, Int>? = null

    override fun onCreate() {
        super.onCreate()
        prefs = Prefs(this)
        Notifications.ensureChannels(this)

        client = EventClient(scope)
        client.onEvent = ::onEvent

        scope.launch {
            client.connection.collect { state ->
                connection.value = state
                updateServiceNotification(state)
            }
        }
        scope.launch {
            client.panels.collect { panels.value = it }
        }
        scope.launch {
            client.alarmPhase.collect { phase ->
                alarmPhase.value = phase
                // Тревогу погасили на посту (или с другого телефона) — снимаем
                // уведомление. Иначе оно висит красным над уже закрытой бедой.
                if (!phase.isActive) {
                    // Тревога снята — сирена замолкает вместе с ней.
                    AlarmSiren.stop()
                    clearAlarmNotifications()
                }
            }
        }
        scope.launch {
            client.alarmPanel.collect { alarmPanel.value = it }
        }

        sendCommand = { action -> client.send(action) }
    }

    /// Убрать все уведомления о тревоге.
    private fun clearAlarmNotifications() {
        val manager = getSystemService<NotificationManager>() ?: return
        for (panel in -1..3)
            manager.cancel(Notifications.eventId(panel))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // УВЕДОМЛЕНИЕ СТАВИТСЯ ПЕРВЫМ ДЕЙСТВИЕМ, до разбора команды.
        //
        // Android даёт службе пять секунд с момента startForegroundService()
        // на то, чтобы показать своё уведомление, и убивает приложение, если
        // та не успела. Ветка остановки этого не делала — и падало всё
        // приложение целиком, а не служба.
        startForeground(
            Notifications.ID_SERVICE,
            Notifications.buildService(this, "подключение…")
        )

        when (intent?.action) {
            ACTION_STOP -> {
                stopEverything(startId)
                return START_NOT_STICKY
            }
            else -> beginWatching()
        }
        // START_STICKY: если системе понадобится память и она нас снимет,
        // служба должна вернуться сама. Наблюдение, тихо переставшее работать
        // после ночи в кармане, хуже, чем не установленное вовсе.
        return START_STICKY
    }

    private fun beginWatching() {
        if (!prefs.isConfigured) {
            updateServiceNotification(Connection.Failed("компьютер не выбран"))
            return
        }

        val wanted = prefs.host to prefs.port

        // СВЯЗЬ НЕ ТРОГАЕМ, ЕСЛИ ОНА УЖЕ ИДЁТ К ТОМУ ЖЕ КОМПЬЮТЕРУ.
        //
        // Команду «начать наблюдение» служба получает часто: при каждом
        // открытии экрана, после перезагрузки, при возврате системой. Раньше
        // каждая такая команда рвала живое соединение и устанавливала его
        // заново — в журнале «Socket closed», на экране «нет связи», а
        // тревога, пришедшая в эту секунду, попадала в разрыв.
        if (watching == wanted && client.connection.value is Connection.Online) {
            acquireWakeLock()
            return
        }

        watching = wanted
        acquireWakeLock()
        client.start(wanted.first, wanted.second)
    }

    private fun stopEverything(startId: Int) {
        watching = null
        client.stop()
        releaseWakeLock()
        stopForeground(STOP_FOREGROUND_REMOVE)
        // Останавливаемся по своему номеру команды, а не безусловно: иначе
        // служба уходит вместе с командой запуска, пришедшей следом.
        stopSelf(startId)
    }

    /// Частичный замок не даёт процессору уснуть настолько, чтобы разорвать
    /// соединение. Экран при этом не горит — батарея расходуется на связь, а
    /// не на подсветку.
    private fun acquireWakeLock() {
        if (wakeLock != null) return
        val power = getSystemService<PowerManager>() ?: return
        wakeLock = power.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK, "PoolSafety::watch"
        ).apply { setReferenceCounted(false); acquire() }
    }

    private fun releaseWakeLock() {
        runCatching { wakeLock?.takeIf { it.isHeld }?.release() }
        wakeLock = null
    }

    private fun onEvent(event: WatchEvent) {
        lastEvent.value = event

        if (!prefs.shouldNotify(event.kind)) return

        val manager = getSystemService<NotificationManager>() ?: return
        val id = Notifications.eventId(event.panelId)

        // Опустевшая зона снимает прежнее сообщение о той же зоне, а не
        // кладёт поверх него новое: иначе в шторке остаётся «человек в зоне»
        // после того, как человек ушёл.
        if (event.kind == EventKind.Clear && !prefs.notifyClear) {
            manager.cancel(id)
            return
        }

        // СИРЕНА ЗАПУСКАЕТСЯ ДО УВЕДОМЛЕНИЯ. Уведомление строится с учётом
        // того, звучит ли сирена: пока звучит, в нём есть кнопка «Заглушить».
        if (event.kind == EventKind.Alarm) AlarmSiren.start(this)

        manager.notify(id, Notifications.buildEvent(this, event, prefs))
    }

    private fun updateServiceNotification(state: Connection) {
        val text = when (state) {
            is Connection.Online -> "на связи с ${state.host}"
            is Connection.Connecting ->
                if (state.attempt <= 1) "подключение к ${state.host}…"
                else "переподключение к ${state.host} (попытка ${state.attempt})…"
            is Connection.Failed -> "нет связи: ${state.reason}"
            Connection.Offline -> "наблюдение остановлено"
        }
        val manager = getSystemService<NotificationManager>() ?: return
        manager.notify(Notifications.ID_SERVICE, Notifications.buildService(this, text))
    }

    override fun onDestroy() {
        sendCommand = null
        watching = null
        AlarmSiren.stop()
        client.stop()
        releaseWakeLock()
        scope.cancel()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
