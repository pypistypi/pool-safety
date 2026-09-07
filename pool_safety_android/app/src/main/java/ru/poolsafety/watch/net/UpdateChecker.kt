package ru.poolsafety.watch.net

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

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
    data object Failed : UpdateResult
}

object UpdateChecker {

    private const val TAG = "PoolSafety"
    private const val API_URL =
        "https://api.github.com/repos/pypistypi/pool-safety/releases/latest"
    private const val TIMEOUT_MS = 8_000

    suspend fun check(currentVersion: String): UpdateResult = withContext(Dispatchers.IO) {
        runCatching {
            val body = fetch(API_URL) ?: return@withContext UpdateResult.Failed
            val json = JSONObject(body)

            // Тег вида «v1.2.1» — отбрасываем букву v для сравнения.
            val tag = json.optString("tag_name").removePrefix("v").trim()
            if (tag.isEmpty()) return@withContext UpdateResult.Failed
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
            val download = apkUrl ?: return@withContext UpdateResult.Failed

            UpdateResult.Available(
                UpdateInfo(
                    version = tag,
                    downloadUrl = download,
                    releaseUrl = json.optString("html_url")
                )
            )
        }.onFailure {
            Log.i(TAG, "проверка обновлений не удалась: ${it.message}")
        }.getOrDefault(UpdateResult.Failed)
    }

    private fun fetch(url: String): String? {
        val connection = URL(url).openConnection() as HttpURLConnection
        return try {
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            // GitHub отвечает 403 без опознаваемого User-Agent.
            connection.setRequestProperty("User-Agent", "PoolSafetyWatch")

            if (connection.responseCode != HttpURLConnection.HTTP_OK) return null
            connection.inputStream.bufferedReader().use { it.readText() }
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
