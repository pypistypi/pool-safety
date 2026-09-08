package ru.poolsafety.watch.net

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URL
import java.net.UnknownHostException
import javax.net.ssl.SSLException

// ---------------------------------------------------------------------------
//  Проверка выпусков на GitHub.
//
//  ЕДИНСТВЕННОЕ МЕСТО В ПРИЛОЖЕНИИ, ГДЕ ЕСТЬ ОБРАЩЕНИЕ В ИНТЕРНЕТ. Связь с
//  постом наблюдения — только локальная сеть, без интернета и облаков; это
//  несущий принцип всей системы, и он не тронут. Проверка обновлений — вещь
//  другого рода: не наблюдение, а забота о том, чтобы у оператора стояла
//  рабочая версия. Она включается отдельным переключателем в настройках,
//  ошибка сети здесь тихая (нет связи — как будто обновлений не нашли, а не
//  повод для тревожного сообщения), и она никогда не запускается сама по
//  себе — только когда оператор открыл приложение или нажал «Проверить».
// ---------------------------------------------------------------------------

data class UpdateInfo(
    val version: String,
    val downloadUrl: String,
    val releaseUrl: String
)

/// Итог проверки. Три разных исхода — и различать их для оператора важно:
/// «обновлений нет» и «не спросить не удалось» выглядят на экране одинаково
/// пусто, но означают разное — одно можно спокойно закрыть, другое стоит
/// повторить, когда появится интернет.
sealed interface UpdateResult {
    data object UpToDate : UpdateResult
    data class Available(val info: UpdateInfo) : UpdateResult

    /// ПРИЧИНА — НЕ УКРАШЕНИЕ, А СУТЬ ИСПРАВЛЕНИЯ. Раньше любой отказ —
    /// не нашёлся адрес, истёк восьмисекундный таймаут, GitHub ответил 403
    /// из-за лимита на анонимные запросы (60 в час НА ОБЩИЙ IP-АДРЕС —
    /// у мобильного оператора его делят тысячи телефонов) — превращался в
    /// одно и то же «нет связи с интернетом». Сообщение было ложным ровно в
    /// том случае, из-за которого его и завели: интернет у оператора был, а
    /// сообщение убеждало в обратном. Теперь причина видна и укладывается в
    /// одну короткую фразу — без стека вызовов, но и без неверных догадок.
    data class Failed(val reason: String) : UpdateResult
}

object UpdateChecker {

    private const val TAG = "PoolSafety"
    private const val API_URL =
        "https://api.github.com/repos/pypistypi/pool-safety/releases/latest"
    private const val TIMEOUT_MS = 8_000

    /// Отказ с уже понятной, короткой причиной — чтобы не гадать по типу
    /// исключения на каждом месте, где он может случиться.
    private class CheckFailed(reason: String) : Exception(reason)

    suspend fun check(currentVersion: String): UpdateResult = withContext(Dispatchers.IO) {
        runCatching {
            val body = fetch(API_URL)
            val json = JSONObject(body)

            // Тег вида «v1.2.1» — отбрасываем букву v для сравнения.
            val tag = json.optString("tag_name").removePrefix("v").trim()
            if (tag.isEmpty())
                throw CheckFailed("сервер обновлений ответил непонятно")
            if (!isNewer(tag, currentVersion)) return@withContext UpdateResult.UpToDate

            // Среди файлов выпуска ищем APK — единственный, что нужен телефону.
            val assets = json.optJSONArray("assets")
            var apkUrl: String? = null
            if (assets != null) {
                for (i in 0 until assets.length()) {
                    val asset = assets.getJSONObject(i)
                    if (asset.optString("name").endsWith(".apk", ignoreCase = true)) {
                        apkUrl = asset.optString("browser_download_url")
                        break
                    }
                }
            }
            val download = apkUrl
                ?: throw CheckFailed("в выпуске на GitHub нет файла .apk")

            UpdateResult.Available(
                UpdateInfo(
                    version = tag,
                    downloadUrl = download,
                    releaseUrl = json.optString("html_url")
                )
            )
        }.getOrElse { error ->
            // ПРИЧИНА — НЕ УГАДЫВАЕТСЯ ЗАДНИМ ЧИСЛОМ, А ОПРЕДЕЛЯЕТСЯ ПО ТИПУ
            // ИСКЛЮЧЕНИЯ. Раньше любой сбой здесь превращался в один и тот же
            // текст «нет связи с интернетом» — и был ложным ровно тогда,
            // когда интернет на самом деле был: например, при ограничении
            // GitHub на анонимные запросы (60 в час на общий IP-адрес
            // мобильного оператора — его делят тысячи телефонов разом) или
            // при VPN, который резолвит имена иначе или режет TLS.
            val reason = when (error) {
                is CheckFailed -> error.message ?: "не удалось проверить обновления"
                is UnknownHostException ->
                    "не удалось найти сервер обновлений — проверьте подключение или VPN"
                is SocketTimeoutException ->
                    "сервер обновлений не ответил за ${TIMEOUT_MS / 1000} с — слабая сеть или VPN"
                is SSLException ->
                    "не удалось установить защищённое соединение — возможно, мешает VPN"
                else -> "не удалось проверить обновления: ${error.message ?: error.javaClass.simpleName}"
            }
            Log.i(TAG, "проверка обновлений не удалась: $reason", error)
            UpdateResult.Failed(reason)
        }
    }

    private fun fetch(url: String): String {
        val connection = URL(url).openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            // GitHub отвечает 403 без опознаваемого User-Agent.
            connection.setRequestProperty("User-Agent", "PoolSafetyWatch")

            val code = connection.responseCode
            if (code == 403 || code == 429) {
                // ИМЕННО ТОТ СЛУЧАЙ, КОТОРЫЙ И ЗАВЁЛ ЭТО ИСПРАВЛЕНИЕ. Анонимные
                // запросы к API GitHub ограничены 60 в час НА IP-АДРЕС, а не
                // на устройство — на мобильном интернете этот адрес общий на
                // всех абонентов оператора разом, и лимит может быть уже
                // исчерпан чужим трафиком. Оператор в этот момент видел бы
                // «нет связи с интернетом», хотя связь у него была.
                val remaining = connection.getHeaderField("X-RateLimit-Remaining")
                throw CheckFailed(
                    if (remaining == "0")
                        "GitHub временно ограничил проверки с этой сети — попробуйте позже"
                    else "сервер обновлений ответил отказом (код $code)"
                )
            }
            if (code != HttpURLConnection.HTTP_OK)
                throw CheckFailed("сервер обновлений ответил ошибкой (код $code)")

            return connection.inputStream.bufferedReader().use { it.readText() }
        } finally {
            connection.disconnect()
        }
    }

    /// true, если `remote` новее `local`. Обе строки вида «1.2.10».
    ///
    /// СРАВНИВАЕМ ЧИСЛАМИ, А НЕ СТРОКАМИ: «1.9.0» строкой больше «1.10.0», а
    /// по смыслу — меньше. Один неправильный релиз с двузначным номером — и
    /// строковое сравнение начало бы врать.
    fun isNewer(remote: String, local: String): Boolean {
        val r = remote.split('.').mapNotNull { it.toIntOrNull() }
        val l = local.split('.').mapNotNull { it.toIntOrNull() }
        if (r.isEmpty() || l.isEmpty()) return false

        for (i in 0 until maxOf(r.size, l.size)) {
            val rv = r.getOrElse(i) { 0 }
            val lv = l.getOrElse(i) { 0 }
            if (rv != lv) return rv > lv
        }
        return false
    }
}
