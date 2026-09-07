package ru.poolsafety.watch.net

import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import kotlin.coroutines.coroutineContext

// ---------------------------------------------------------------------------
//  Постоянная связь с компьютером.
//
//  ПЕРЕПОДКЛЮЧЕНИЕ — НЕ УКРАШЕНИЕ, А СМЫСЛ. Телефон уходит в сон, сеть Wi-Fi
//  переключается на другую точку, компьютер перезагружают. Если после каждого
//  такого случая приложение молча замолкает, оператор узнает об этом только
//  тогда, когда не придёт тревога. Поэтому связь восстанавливается сама, а её
//  состояние всегда видно на экране.
//
//  ТИШИНА ТОЖЕ СЧИТАЕТСЯ ОБРЫВОМ. Разорванное соединение TCP умеет молчать
//  часами, не сообщая об ошибке: сокет цел, данных нет. Поэтому если от
//  компьютера долго ничего не слышно, связь считается потерянной и
//  устанавливается заново.
// ---------------------------------------------------------------------------

sealed interface Connection {
    data object Offline : Connection
    data class Connecting(val host: String, val attempt: Int) : Connection
    data class Online(val host: String) : Connection
    data class Failed(val reason: String) : Connection
}

class EventClient(private val scope: CoroutineScope) {

    companion object {
        private const val TAG = "PoolSafety"

        /// Сколько молчания считаем обрывом. Программа на компьютере шлёт
        /// сведения о панелях при каждом изменении, но в спокойной смене
        /// изменений может не быть подолгу, поэтому срок щедрый.
        private const val SILENCE_LIMIT_MS = 90_000L

        private const val CONNECT_TIMEOUT_MS = 5_000
        private const val READ_TIMEOUT_MS = 10_000
        private const val MAX_PAUSE_MS = 30_000L
    }

    val connection = MutableStateFlow<Connection>(Connection.Offline)
    val panels = MutableStateFlow<List<Panel>>(emptyList())

    /// Куда уходят события. Обработчик ставит служба: ей решать, показывать
    /// уведомление или нет.
    var onEvent: ((WatchEvent) -> Unit)? = null

    /// Состояние тревоги на посту.
    val alarmPhase = MutableStateFlow(AlarmPhase.Idle)
    val alarmPanel = MutableStateFlow(-1)

    private var job: Job? = null

    /// Открытый сокет держим под рукой, чтобы уметь его закрыть.
    ///
    /// БЕЗ ЭТОГО ОСТАНОВКА НЕ РАБОТАЕТ. Чтение из сокета блокирует поток, и
    /// отмена корутины сама по себе его не прерывает: команда «остановись»
    /// дошла бы только после следующего сообщения — то есть, возможно, никогда.
    @Volatile
    private var socket: Socket? = null

    fun start(host: String, port: Int) {
        stop()
        job = scope.launch(Dispatchers.IO) { loop(host, port) }
    }

    /// Отправить решение по тревоге на компьютер.
    ///
    /// Пишем прямо в сокет из отдельной задачи: команда короткая, а ждать
    /// её отправки, держа палец на кнопке, спасателю некогда.
    fun send(action: AlarmCommand) {
        val open = socket
        if (open == null || open.isClosed) {
            Log.w(TAG, "решение «${action.wire}» отправить некуда: связи нет")
            return
        }
        scope.launch(Dispatchers.IO) {
            runCatching {
                open.getOutputStream().apply {
                    write(Protocol.command(action))
                    flush()
                }
                Log.i(TAG, "решение отправлено: ${action.wire}")
            }.onFailure { Log.w(TAG, "команда не ушла: ${it.message}") }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
        runCatching { socket?.close() }
        socket = null
        connection.value = Connection.Offline
    }

    private suspend fun loop(host: String, port: Int) {
        var attempt = 0
        while (coroutineContext.isActive) {
            attempt++
            connection.value = Connection.Connecting(host, attempt)

            try {
                session(host, port)
                attempt = 0
            } catch (stop: CancellationException) {
                // Нас остановили — это не сбой связи, и повторять не надо.
                throw stop
            } catch (failure: Exception) {
                // Связи нет — о тревоге мы больше ничего не знаем. Держать
                // красную кнопку «Принял», которая никуда не дойдёт, хуже, чем
                // честно показать, что связь потеряна.
                alarmPhase.value = AlarmPhase.Idle
                val reason = shortReason(failure)
                Log.w(TAG, "связь с $host:$port оборвалась: ${failure.message ?: failure}")
                connection.value = Connection.Failed(reason)
            }

            // Пауза перед новой попыткой растёт, но не бесконечно: раз в
            // полминуты пробовать надо всегда, иначе после долгого отсутствия
            // сети связь не восстановится вовсе.
            delay(minOf(1_000L * attempt, MAX_PAUSE_MS))
        }
    }

    /// Короткая, понятная причина обрыва — для показа на экране.
    ///
    /// БЕЗ ЭТОГО ОПЕРАТОР ВИДЕЛ БЫ СЫРОЕ СООБЩЕНИЕ JAVA: «failed to connect to
    /// /10.0.2.2 (port 8765) from /10.0.2.16 (port 42342) after 5000ms:
    /// isConnected failed: ECONNREFUSED (Connection refused)» — шесть строк
    /// вместо одной, которые расталкивают весь экран и ничего не объясняют
    /// человеку без опыта в сетях. Причина обрыва в подробностях остаётся в
    /// журнале для отладки, а на экране — фраза по-русски.
    private fun shortReason(failure: Exception): String = when {
        failure is SocketTimeoutException ->
            "компьютер не отвечает"
        failure is java.net.ConnectException
            || failure.message?.contains("ECONNREFUSED") == true ->
            "компьютер не запущен или порт закрыт"
        failure is java.net.UnknownHostException ->
            "адрес не найден"
        failure is java.net.NoRouteToHostException
            || failure.message?.contains("ENETUNREACH") == true
            || failure.message?.contains("EHOSTUNREACH") == true ->
            "сеть недоступна — телефон не в той же сети, что компьютер"
        else -> failure.message?.takeIf { it.isNotBlank() && it.length <= 60 }
            ?: "связь потеряна"
    }

    private suspend fun session(host: String, port: Int) = withContext(Dispatchers.IO) {
        Socket().use { open ->
            socket = open
            open.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
            open.soTimeout = READ_TIMEOUT_MS
            open.keepAlive = true
            connection.value = Connection.Online(host)

            val reader: BufferedReader = open.getInputStream().bufferedReader()
            var lastHeard = System.currentTimeMillis()

            while (isActive) {
                val line = try {
                    reader.readLine()
                } catch (_: SocketTimeoutException) {
                    // Ожидание истекло — это не ошибка сама по себе. Ошибка,
                    // если молчание затянулось.
                    if (System.currentTimeMillis() - lastHeard > SILENCE_LIMIT_MS) {
                        throw IllegalStateException("компьютер молчит")
                    }
                    continue
                }

                // Конец потока: другая сторона закрыла соединение.
                if (line == null) throw IllegalStateException("соединение закрыто")

                lastHeard = System.currentTimeMillis()
                handle(Protocol.parse(line))
            }
        }
        socket = null
    }

    private fun handle(message: Message) {
        when (message) {
            is Message.Panels -> panels.value = message.items
            is Message.Event -> onEvent?.invoke(message.event)
            is Message.Hello -> Log.i(TAG, "компьютер представился: ${message.version}")
            is Message.Alarm -> {
                alarmPhase.value = message.phase
                alarmPanel.value = message.panelId
            }
            is Message.Unsupported -> Log.i(TAG, "непонятное сообщение: ${message.raw}")
        }
    }
}
