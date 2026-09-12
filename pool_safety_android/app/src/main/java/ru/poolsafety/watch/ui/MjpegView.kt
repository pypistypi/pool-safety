package ru.poolsafety.watch.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.AttributeSet
import android.util.Log
import androidx.appcompat.widget.AppCompatImageView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedInputStream
import java.net.HttpURLConnection
import java.net.URL
import kotlin.coroutines.coroutineContext

// ---------------------------------------------------------------------------
//  Просмотр потока MJPEG.
//
//  ЗАЧЕМ ЭТО НУЖНО, ЕСЛИ ЕСТЬ ExoPlayer. Потому что ExoPlayer MJPEG не умеет —
//  ни в каком виде. А смартфон в роли камеры и часть недорогих камер отдают
//  именно его. Показать «поток не поддерживается» там, где картинка на самом
//  деле есть, — худший исход: оператор решит, что сломана камера.
//
//  ФОРМАТ ПРОСТ ДО НЕПРИЛИЧИЯ. Это HTTP-ответ multipart/x-mixed-replace: кадры
//  JPEG подряд, между ними строка-разделитель. Разбирается поиском маркеров
//  начала (FF D8) и конца (FF D9) кадра — так надёжнее, чем доверять
//  заголовку Content-Length, который некоторые камеры пишут неверно.
//
//  ЭТО ЗАПАСНОЙ ПУТЬ. Основной — RTSP: он втрое легче для сети. Здесь каждый
//  кадр — целая картинка, сжатая сама по себе, поэтому по сети идёт заметно
//  больше данных, сколько бы кадров мы ни отбросили при показе.
//
//  ЧТЕНИЕ ПАКЕТАМИ, А НЕ ПО БАЙТУ — то же исправление, что уже сделано для
//  программы поста (core::MjpegWorker на стороне ПК), перенесённое сюда.
//  Раньше кадр вычитывался из потока по одному байту (stream.read() в цикле):
//  для кадра 720p в сто с лишним килобайт это сотня тысяч вызовов чтения на
//  каждый кадр, тридцать раз в секунду — и именно это, а не разжатие
//  картинки, было главной тратой на слабом телефоне. Хуже того: побайтовое
//  чтение шло для КАЖДОГО кадра из сети, даже для тех, что тут же
//  отбрасывались по частоте показа — отбор кадра происходил уже ПОСЛЕ самой
//  дорогой части.
//
//  Теперь сеть читается большими кусками (stream.read в массив, а не по
//  байту), кадры ищутся в уже накопленном буфере по тем же маркерам
//  SOI/EOI (см. FrameBuffer ниже), а если сеть успела
//  прислать больше одного целого кадра за раз — разобраны будут все, но до
//  разжатия дойдёт только самый свежий. Устаревший кадр в этой схеме не
//  стоит почти ничего: несколько сравнений байт в уже лежащем в памяти
//  массиве, а не поток системных вызовов.
// ---------------------------------------------------------------------------

class MjpegView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyle: Int = 0
) : AppCompatImageView(context, attrs, defStyle) {

    companion object {
        private const val TAG = "PoolSafety"
        private const val CONNECT_TIMEOUT_MS = 5_000
        private const val READ_TIMEOUT_MS = 10_000

        /// Сколько кадров в секунду показываем.
        ///
        /// Камера присылает 25–30, но телефону столько не нужно: экран мелкий,
        /// а каждый лишний кадр — это разжатие целой картинки и расход
        /// батареи. Лишние кадры отбрасываются сразу после чтения.
        private const val TARGET_FPS = 12
        private const val MIN_FRAME_GAP_MS = 1000L / TARGET_FPS
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var job: Job? = null

    /// Состояние для подписи на панели: пусто — всё хорошо.
    var onStatus: ((String?) -> Unit)? = null

    fun start(url: String) {
        stop()
        job = scope.launch { loop(url) }
    }

    fun stop() {
        job?.cancel()
        job = null
        setImageDrawable(null)
    }

    private suspend fun loop(url: String) {
        var attempt = 0
        while (coroutineContext.isActive) {
            attempt++
            onStatus?.invoke(
                if (attempt == 1) "подключение к камере…"
                else "переподключение (попытка $attempt)…"
            )
            try {
                read(url)
                attempt = 0
            } catch (stop: kotlinx.coroutines.CancellationException) {
                throw stop
            } catch (failure: Exception) {
                Log.w(TAG, "MJPEG $url: ${failure.message}")
                onStatus?.invoke("камера не отвечает")
            }
            delay(minOf(1_000L * attempt, 15_000L))
        }
    }

    private suspend fun read(url: String) = withContext(Dispatchers.IO) {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout = READ_TIMEOUT_MS
            doInput = true
            connect()
        }

        try {
            val stream = BufferedInputStream(connection.inputStream, 64 * 1024)
            val buffer = FrameBuffer()
            val chunk = ByteArray(32 * 1024)
            var lastShown = 0L

            while (isActive) {
                val read = stream.read(chunk)
                if (read < 0) throw IllegalStateException("поток кончился")
                buffer.append(chunk, read)

                // Разбирает всё, что накопилось, и возвращает только самый
                // свежий целый кадр — устаревшие уходят из буфера, так и не
                // будучи разжатыми. Пустой результат — ни одного целого кадра
                // ещё нет, читаем дальше.
                val frame = buffer.extractLatest() ?: continue

                val now = System.currentTimeMillis()
                if (now - lastShown < MIN_FRAME_GAP_MS) continue
                lastShown = now

                val bitmap = decode(frame) ?: continue
                withContext(Dispatchers.Main) {
                    onStatus?.invoke(null)
                    setImageBitmap(bitmap)
                }
            }
        } finally {
            runCatching { connection.disconnect() }
        }
    }

    /// Размер, в котором кадр реально показывается. Обновляется разметкой.
    @Volatile
    private var viewWidth = 0

    @Volatile
    private var viewHeight = 0

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        viewWidth = w
        viewHeight = h
    }

    /// Разжать кадр сразу до нужного размера.
    ///
    /// ПОЧЕМУ НЕ ЦЕЛИКОМ. Кадр 1920x1080 занимает восемь мегабайт, а на клетке
    /// сетки телефона показывается в размере, вчетверо меньшем по стороне.
    /// Разжимать полностью — значит двенадцать раз в секунду просить у системы
    /// по восемь мегабайт и тут же выбрасывать. Прежняя мысль освобождать кадр
    /// вручную оказалась хуже: картинку в этот момент ещё может рисовать сам
    /// вид, и освобождённый кадр роняет отрисовку.
    private fun decode(frame: ByteArray): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(frame, 0, frame.size, bounds)

        var sample = 1
        val targetW = viewWidth
        val targetH = viewHeight
        if (targetW > 0 && targetH > 0 && bounds.outWidth > 0) {
            while (bounds.outWidth / (sample * 2) >= targetW &&
                bounds.outHeight / (sample * 2) >= targetH
            ) {
                sample *= 2
            }
        }

        val options = BitmapFactory.Options().apply {
            inSampleSize = sample
            // Полупрозрачности в видеокадре не бывает, а RGB_565 занимает вдвое
            // меньше памяти и на глаз от полного цвета здесь неотличим.
            inPreferredConfig = Bitmap.Config.RGB_565
        }
        return BitmapFactory.decodeByteArray(frame, 0, frame.size, options)
    }

    override fun onDetachedFromWindow() {
        stop()
        scope.cancel()
        super.onDetachedFromWindow()
    }
}

/// Растущий буфер сырых байт потока с разбором по маркерам JPEG.
///
/// АНАЛОГ core::MjpegWorker::extractLatestFrame НА СТОРОНЕ ПК, тем же
/// приёмом: копится байтами, разбирается сразу ВСЁ, что успело накопиться,
/// а возвращается только самый свежий целиком собранный кадр — предыдущие
/// уходят из буфера, так и не будучи разжатыми. Именно так лишний кадр
/// перестаёт стоить почти ничего: до JPEG-декодера, самой дорогой части,
/// он просто не доходит.
///
/// Разбор — только по маркерам SOI (FF D8) и EOI (FF D9), без опоры на
/// заголовки multipart: у смартфона в роли камеры и у самодельных камер
/// разметка между кадрами отличается, а сам JPEG — нет.
private class FrameBuffer {

    companion object {
        /// Не копим байты бесконечно, если поток окажется не тем, что мы
        /// ждём, — тот же предел, что и на стороне ПК.
        private const val MAX_BYTES = 32 * 1024 * 1024

        private val SOI = byteArrayOf(0xFF.toByte(), 0xD8.toByte())
        private val EOI = byteArrayOf(0xFF.toByte(), 0xD9.toByte())
    }

    private var data = ByteArray(0)

    fun append(chunk: ByteArray, length: Int) {
        val merged = ByteArray(data.size + length)
        System.arraycopy(data, 0, merged, 0, data.size)
        System.arraycopy(chunk, 0, merged, data.size, length)
        data = merged

        if (data.size > MAX_BYTES) {
            // Похоже, это не MJPEG вовсе — маркеры никогда не находятся, и
            // буфер растёт без конца. Начинаем заново, а не копим мегабайты
            // впустую.
            data = ByteArray(0)
        }
    }

    fun extractLatest(): ByteArray? {
        var last: ByteArray? = null
        var consumedUpTo = 0

        while (true) {
            val soi = indexOf(data, SOI, consumedUpTo)
            if (soi < 0) break
            val eoi = indexOf(data, EOI, soi + SOI.size)
            if (eoi < 0) break   // кадр начался, но не закончился — ждём остаток

            val frameEnd = eoi + EOI.size
            last = data.copyOfRange(soi, frameEnd)
            consumedUpTo = frameEnd
        }

        if (consumedUpTo > 0)
            data = data.copyOfRange(consumedUpTo, data.size)

        return last
    }

    private fun indexOf(haystack: ByteArray, needle: ByteArray, from: Int): Int {
        val limit = haystack.size - needle.size
        var i = from
        while (i <= limit) {
            if (haystack[i] == needle[0] && haystack[i + 1] == needle[1])
                return i
            ++i
        }
        return -1
    }
}
