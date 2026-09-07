package ru.poolsafety.watch.ui

import android.content.Context
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import androidx.annotation.OptIn
import androidx.core.content.ContextCompat
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.exoplayer.DefaultLoadControl
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.rtsp.RtspMediaSource
import androidx.media3.common.util.UnstableApi
import ru.poolsafety.watch.R
import ru.poolsafety.watch.databinding.ViewPanelBinding
import ru.poolsafety.watch.net.EventKind
import ru.poolsafety.watch.net.Level
import ru.poolsafety.watch.net.Panel

// ---------------------------------------------------------------------------
//  Одна клетка сетки на телефоне.
//
//  ДВА СПОСОБА ПОКАЗА, И ВЫБИРАЕТСЯ ОН ПО АДРЕСУ. rtsp:// отдаётся ExoPlayer,
//  http:// — своему приёмнику MJPEG, потому что ExoPlayer этот формат не
//  понимает. Оператору разница не видна и видна быть не должна.
//
//  ПОТОК ВКЛЮЧАЕТСЯ ТОЛЬКО У ВИДИМОЙ ПАНЕЛИ. Держать четыре видеопотока, из
//  которых смотрят на один, — значит сажать батарею и забивать сеть впустую.
// ---------------------------------------------------------------------------

@OptIn(UnstableApi::class)
class PanelHolder(
    private val context: Context,
    val binding: ViewPanelBinding
) {

    private var player: ExoPlayer? = null
    private var panel: Panel? = null
    private var visible = false

    /// Адрес, который проигрывается прямо сейчас. Пусто — не проигрывается.
    ///
    /// ЗАЧЕМ ЗАПОМИНАТЬ. Сведения о панелях приходят с компьютера при каждом
    /// изменении обстановки — то есть в людной смене по нескольку раз в
    /// секунду. Пока каждое такое сообщение перезапускало поток, видео просто
    /// не успевало начаться: в журнале шла бесконечная череда «создан —
    /// освобождён». Перезапускать поток нужно тогда и только тогда, когда
    /// сменился сам адрес.
    private var playingUrl: String? = null

    /// Уровень, показанный рамкой. Приходит и от сведений о панели, и от
    /// событий: событие свежее, чем список панелей, и рамка обязана
    /// откликаться немедленно.
    private var level: Level = Level.Normal
    private var people = 0

    val root: View get() = binding.root

    // ------------------------------------------------------------- жесты
    //
    //  ОДИН ПАЛЕЦ ПЕРЕКЛЮЧАЕТ, ДВА ПРИБЛИЖАЮТ. Нажатие по клетке разворачивает
    //  камеру на весь экран и обратно — так было и раньше. Щипок двумя
    //  пальцами приближает картинку: у бортика бывает нужно разглядеть, лежит
    //  человек или сидит, а клетка сетки для этого мала.
    //
    //  ПРИБЛИЖЕНИЕ НЕ ТРОГАЕТ РАСПОЗНАВАНИЕ. Оно происходит целиком на
    //  телефоне и только для глаз: компьютер по-прежнему разбирает полный
    //  кадр. Приблизив угол, оператор не «сузит» наблюдение.

    private var scale = 1f
    private var offsetX = 0f
    private var offsetY = 0f

    private companion object {
        const val MIN_SCALE = 1f
        const val MAX_SCALE = 4f
    }

    /// Навесить жесты. [onSingleTap] — обычное нажатие по клетке.
    fun attachGestures(onSingleTap: () -> Unit) {
        val tap = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                onSingleTap()
                return true
            }

            /// Двойное нажатие возвращает картинку к исходному размеру —
            /// быстрее, чем сводить пальцы обратно.
            override fun onDoubleTap(e: MotionEvent): Boolean {
                resetZoom()
                return true
            }

            override fun onScroll(
                down: MotionEvent?, event: MotionEvent,
                dx: Float, dy: Float
            ): Boolean {
                // Двигать картинку можно только когда она приближена: иначе
                // жест ничего не значит и мешает пролистыванию.
                if (scale <= MIN_SCALE) return false
                offsetX -= dx
                offsetY -= dy
                applyZoom()
                return true
            }
        })

        val pinch = ScaleGestureDetector(context,
            object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
                override fun onScale(detector: ScaleGestureDetector): Boolean {
                    scale = (scale * detector.scaleFactor)
                        .coerceIn(MIN_SCALE, MAX_SCALE)
                    applyZoom()
                    return true
                }
            })

        root.setOnTouchListener { view, event ->
            pinch.onTouchEvent(event)
            // Пока идёт щипок, одиночное нажатие не считаем: палец, снятый
            // после приближения, не должен переключать режим.
            if (!pinch.isInProgress) tap.onTouchEvent(event)
            view.performClick()
            true
        }
    }

    /// Вернуть исходный размер. Вызывается при смене камеры и режима: иначе
    /// приближение осталось бы от прошлой картинки.
    fun resetZoom() {
        scale = 1f
        offsetX = 0f
        offsetY = 0f
        applyZoom()
    }

    private fun applyZoom() {
        // Не даём утащить картинку за край: пустоты по бокам быть не должно.
        val limitX = (binding.player.width * (scale - 1f)) / 2f
        val limitY = (binding.player.height * (scale - 1f)) / 2f
        offsetX = offsetX.coerceIn(-limitX, limitX)
        offsetY = offsetY.coerceIn(-limitY, limitY)

        for (view in listOf<View>(binding.player, binding.mjpeg)) {
            view.scaleX = scale
            view.scaleY = scale
            view.translationX = offsetX
            view.translationY = offsetY
        }
    }

    fun bind(panel: Panel?) {
        // Сменилась камера — приближение от прежней больше не имеет смысла.
        if (this.panel?.stream != panel?.stream) resetZoom()
        this.panel = panel
        if (panel != null) {
            level = panel.level
            people = panel.people
        }
        render()
        syncStream()
    }

    /// Отметить обстановку по событию.
    fun applyEvent(kind: EventKind, newLevel: Level) {
        level = when (kind) {
            EventKind.Clear -> Level.Normal
            else -> newLevel
        }
        if (kind == EventKind.Clear) people = 0
        render()
    }

    fun setPeople(count: Int) {
        people = count
        render()
    }

    /// Показывать ли эту панель. От этого зависит, идёт ли поток.
    fun setVisible(value: Boolean) {
        if (visible == value) return
        visible = value
        root.visibility = if (value) View.VISIBLE else View.GONE
        syncStream()
    }

    fun release() {
        stopStream()
    }

    /// Перезапустить поток принудительно — например, после потери связи.
    fun restartStream() {
        stopStream()
        syncStream()
    }

    // ------------------------------------------------------------- показ

    private fun render() {
        val current = panel

        binding.zone.text = current?.zone.orEmpty().ifBlank { "—" }
        binding.zone.visibility = if (current == null) View.GONE else View.VISIBLE

        binding.badge.text = when {
            current == null -> ""
            people > 0 -> "в зоне: $people"
            else -> ""
        }
        binding.badge.visibility = if (binding.badge.text.isNullOrEmpty()) View.GONE
                                   else View.VISIBLE

        // ЦВЕТ НЕ ЗАВИСИТ ОТ ТОГО, ЕСТЬ ЛИ ВИДЕО. Панель может смотреть на
        // запись или на камеру самого компьютера — телефону её картинка
        // недоступна, но знать, что там люди или тревога, он обязан. Пока
        // серым красилось всё без видео, тревога на такой панели выглядела
        // как пустая клетка.
        val colour = ContextCompat.getColor(
            context,
            when {
                current == null -> R.color.level_none
                level == Level.Alarm -> R.color.level_alarm
                level == Level.Attention -> R.color.level_attention
                people > 0 -> R.color.level_presence
                else -> R.color.level_none
            }
        )
        binding.card.strokeColor = colour

        // Толщина рамки тоже говорит: тревогу надо замечать боковым зрением.
        val width = context.resources.displayMetrics.density *
            if (level == Level.Alarm) 4f else 2f
        binding.card.strokeWidth = width.toInt()
    }

    private fun setStatus(text: String?) {
        binding.status.text = text.orEmpty()
        binding.status.visibility = if (text.isNullOrBlank()) View.GONE else View.VISIBLE
    }

    // ------------------------------------------------------------- поток

    /// Привести поток в соответствие с тем, что панель должна показывать.
    private fun syncStream() {
        val wanted = panel?.stream
            ?.takeIf { visible && it.isNotBlank() }

        // Поток трогаем только при смене адреса — иначе он не успеет начаться.
        if (wanted != playingUrl) {
            stopStream()
            playingUrl = wanted

            if (wanted != null) {
                if (wanted.startsWith("rtsp", ignoreCase = true)) {
                    startRtsp(wanted)
                } else {
                    startMjpeg(wanted)
                }
            } else {
                showNothing()
            }
        }

        // А подпись обновляем всегда.
        //
        // ЗДЕСЬ УЖЕ БЫЛА ОШИБКА: при самом первом вызове и нужный адрес, и
        // проигрываемый пусты, проверка «ничего не изменилось» срабатывала — и
        // панель без камеры оставалась просто чёрным прямоугольником, без
        // объяснения, почему на ней ничего нет.
        if (wanted == null) {
            val current = panel
            setStatus(
                when {
                    !visible -> null
                    current == null -> context.getString(R.string.panel_empty)
                    else -> context.getString(R.string.panel_no_video)
                }
            )
        }
    }

    private fun showNothing() {
        binding.player.visibility = View.GONE
        binding.mjpeg.visibility = View.GONE
    }

    private fun startRtsp(url: String) {
        binding.player.visibility = View.VISIBLE
        binding.mjpeg.visibility = View.GONE
        setStatus(context.getString(R.string.panel_connecting))

        // БУФЕР УРЕЗАН ДО ПРЕДЕЛА. По умолчанию ExoPlayer копит до пятидесяти
        // секунд видео: для фильма это правильно, для наблюдения — катастрофа.
        // Спасатель смотрит на то, что происходит сейчас, а не полминуты
        // назад. Здесь буфер измеряется десятыми долями секунды: картинка
        // может дрогнуть при заминке сети, и это несравнимо меньшая беда, чем
        // отставание.
        val loadControl = DefaultLoadControl.Builder()
            .setBufferDurationsMs(
                /* minBufferMs = */ 200,
                /* maxBufferMs = */ 800,
                /* bufferForPlaybackMs = */ 100,
                /* bufferForPlaybackAfterRebufferMs = */ 150
            )
            .setPrioritizeTimeOverSizeThresholds(true)
            .build()

        val exo = ExoPlayer.Builder(context)
            .setLoadControl(loadControl)
            .build()
        player = exo
        binding.player.player = exo

        exo.addListener(object : Player.Listener {
            override fun onPlaybackStateChanged(state: Int) {
                if (state == Player.STATE_READY) setStatus(null)
            }

            /// Надпись снимается по первому показанному кадру.
            ///
            /// Состояния READY для этого мало: у живого потока оно приходит не
            /// всегда, и «подключение к камере…» оставалось висеть поверх уже
            /// идущего видео — оператор читал бы, что связи нет, глядя на
            /// картинку с камеры.
            override fun onRenderedFirstFrame() {
                setStatus(null)
            }

            override fun onPlayerError(error: PlaybackException) {
                // Камера могла просто моргнуть питанием. Молча остаться с
                // чёрным прямоугольником нельзя — оператор должен отличать
                // «в кадре пусто» от «камера отвалилась».
                setStatus("камера не отвечает")
                exo.seekToDefaultPosition()
                exo.prepare()
            }
        })

        // Транспорт TCP, а не UDP: в сети с точками доступа UDP теряет пакеты
        // и картинка сыплется квадратами.
        val source = RtspMediaSource.Factory()
            .setForceUseRtpTcp(true)
            .setTimeoutMs(5_000)
            .createMediaSource(MediaItem.fromUri(url))

        exo.setMediaSource(source)
        exo.prepare()
        exo.playWhenReady = true
    }

    private fun startMjpeg(url: String) {
        binding.player.visibility = View.GONE
        binding.mjpeg.visibility = View.VISIBLE
        binding.mjpeg.onStatus = { setStatus(it) }
        binding.mjpeg.start(url)
    }

    private fun stopStream() {
        playingUrl = null
        player?.let {
            it.stop()
            it.release()
        }
        player = null
        binding.player.player = null
        binding.mjpeg.stop()
        binding.mjpeg.onStatus = null
    }
}
