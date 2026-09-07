package ru.poolsafety.watch.net

import org.json.JSONObject

// ---------------------------------------------------------------------------
//  Язык, на котором программа с компьютера говорит с телефоном.
//
//  Разбор написан на встроенном org.json, а не на библиотеке сериализации.
//  Причин две. Сообщений наперечёт, и описывать их через размеченные классы —
//  больше кода, чем разобрать руками. А главное: приходящее по сети
//  нельзя считать правильным. Любое поле может отсутствовать или иметь не тот
//  тип, и разбор обязан пережить это молча, а не уронить наблюдение.
// ---------------------------------------------------------------------------

/// Уровень обстановки. Числа те же, что и в программе на компьютере.
enum class Level(val code: Int) {
    Normal(0),      // норма
    Attention(1),   // внимание
    Alarm(2);       // тревога

    companion object {
        fun of(code: Int): Level = entries.firstOrNull { it.code == code } ?: Normal
    }
}

/// Что за событие. Присутствие и тревога — разные вещи, и в приложении они
/// тоже разведены: присутствие никогда не поднимается до тревоги.
enum class EventKind(val wire: String) {
    Presence("presence"),     // в зоне появились люди
    Attention("attention"),   // признак, требующий взгляда
    Alarm("alarm"),           // тревога
    Clear("clear"),           // зона опустела
    Unknown("");

    companion object {
        fun of(text: String?): EventKind =
            entries.firstOrNull { it.wire == text } ?: Unknown
    }
}

/// Панель наблюдения: зона и адрес её камеры.
data class Panel(
    val id: Int,
    val zone: String,
    val stream: String,
    val people: Int,
    val level: Level
) {
    /// Есть ли что показывать. Панель без адреса потока — это либо пустая
    /// клетка, либо источник не сетевой (файл, камера самого компьютера):
    /// такой телефону не достать, и делать вид, что достанем, незачем.
    val hasVideo: Boolean get() = stream.isNotBlank()
}

/// Событие от компьютера.
data class WatchEvent(
    val kind: EventKind,
    val panelId: Int,
    val zone: String,
    val title: String,
    val details: String,
    val level: Level,
    val time: String
)

/// В каком состоянии тревога на посту.
///
/// ЗАЧЕМ ЭТО ОТДЕЛЬНО ОТ СОБЫТИЙ. Событие «тревога» говорит, что случилось.
/// Состояние говорит, что с ней делают: приняли, признали ложной, закрыли.
/// Без него уведомление на телефоне висело красным и после того, как на посту
/// всё погасили, — то есть спасатель считал, что беда продолжается.
enum class AlarmPhase(val wire: String) {
    Idle("idle"),                 // тревоги нет
    Raised("raised"),             // объявлена, ещё не принята
    Acknowledged("acknowledged"), // принята, решение не принято
    Confirmed("confirmed");       // подтверждена, помощь вызвана

    val isActive: Boolean get() = this != Idle

    companion object {
        fun of(text: String?): AlarmPhase =
            entries.firstOrNull { it.wire == text } ?: Idle
    }
}

/// Что телефон может сделать с тревогой — ровно то же, что оператор на посту.
enum class AlarmCommand(val wire: String) {
    Acknowledge("acknowledge"),
    FalseAlarm("false_alarm"),
    Confirm("confirm"),

    /// Происшествие закончено, можно возвращаться к наблюдению.
    ///
    /// ОТДЕЛЬНО ОТ «ПОМОЩЬ ВЫЗВАНА»: между вызовом скорой и её отъездом
    /// проходит время, и всё это время тревога должна оставаться на экране.
    Close("close")
}

/// Разобранное сообщение. Всё, что не разобралось, — [Unsupported]: молчаливое
/// исчезновение неизвестного сообщения хуже, чем запись о нём в журнал.
sealed interface Message {
    data class Panels(val items: List<Panel>) : Message
    data class Event(val event: WatchEvent) : Message
    data class Hello(val version: String) : Message
    data class Alarm(val phase: AlarmPhase, val panelId: Int) : Message
    data class Unsupported(val raw: String) : Message
}

object Protocol {

    /// Слово, по которому компьютер узнаёт запрос телефона.
    const val DISCOVERY_REQUEST = "POOLSAFETY-DISCOVER"

    const val DEFAULT_PORT = 8765
    const val DISCOVERY_PORT = 8766

    /// Собрать команду для отправки на компьютер.
    fun command(action: AlarmCommand): ByteArray =
        ("{\"type\":\"command\",\"action\":\"" + action.wire + "\"}\n").toByteArray()

    fun parse(line: String): Message {
        val text = line.trim()
        if (text.isEmpty()) return Message.Unsupported(line)

        val json = runCatching { JSONObject(text) }.getOrNull()
            ?: return Message.Unsupported(line)

        return when (json.optString("type")) {
            "panels" -> Message.Panels(parsePanels(json))
            "event" -> Message.Event(parseEvent(json))
            "hello" -> Message.Hello(json.optString("version", "?"))
            "alarm_state" -> Message.Alarm(
                phase = AlarmPhase.of(json.optString("state")),
                panelId = json.optInt("panel", -1)
            )
            else -> Message.Unsupported(line)
        }
    }

    private fun parsePanels(json: JSONObject): List<Panel> {
        val array = json.optJSONArray("items") ?: return emptyList()
        val result = ArrayList<Panel>(array.length())
        for (i in 0 until array.length()) {
            val item = array.optJSONObject(i) ?: continue
            result += Panel(
                id = item.optInt("id", -1),
                zone = item.optString("zone"),
                stream = item.optString("stream"),
                people = item.optInt("people", 0),
                level = Level.of(item.optInt("level", 0))
            )
        }
        return result
    }

    private fun parseEvent(json: JSONObject): WatchEvent = WatchEvent(
        kind = EventKind.of(json.optString("kind")),
        panelId = json.optInt("panel", -1),
        zone = json.optString("zone"),
        title = json.optString("title"),
        details = json.optString("details"),
        level = Level.of(json.optInt("level", 0)),
        time = json.optString("time")
    )
}
