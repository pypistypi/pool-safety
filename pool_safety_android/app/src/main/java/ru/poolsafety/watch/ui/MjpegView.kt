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
import java.io.ByteArrayOutputStream
import java.io.InputStream
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
            var lastShown = 0L

            while (isActive) {
                val frame = readJpeg(stream) ?: throw IllegalStateException("поток кончился")

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

    /// Вытащить из потока один кадр JPEG.
    private fun readJpeg(stream: InputStream): ByteArray? {
        val buffer = ByteArrayOutputStream(64 * 1024)

        // Ищем начало кадра: FF D8.
        var previous = -1
        while (true) {
            val byte = stream.read()
            if (byte < 0) return null
            if (previous == 0xFF && byte == 0xD8) {
                buffer.write(0xFF)
                buffer.write(0xD8)
                break
            }
            previous = byte
        }

        // Читаем до конца кадра: FF D9.
        previous = -1
        while (true) {
            val byte = stream.read()
            if (byte < 0) return null
            buffer.write(byte)
            if (previous == 0xFF && byte == 0xD9) break
            previous = byte
        }

        return buffer.toByteArray()
    }

    override fun onDetachedFromWindow() {
        stop()
        scope.cancel()
        super.onDetachedFromWindow()
    }
}
