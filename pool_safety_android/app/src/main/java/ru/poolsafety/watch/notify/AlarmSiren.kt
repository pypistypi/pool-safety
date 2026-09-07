package ru.poolsafety.watch.notify

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import androidx.core.content.getSystemService

// ---------------------------------------------------------------------------
//  Сирена тревоги на телефоне.
//
//  ПОЧЕМУ СВОЙ ЗВУК, А НЕ ЗВУК УВЕДОМЛЕНИЯ. Уведомление звучит один раз, при
//  показе. Оператор жаловался ровно на это: звук обрывался от первого касания
//  экрана — достаточно было потянуть шторку, и сигнал пропадал, а тревога
//  оставалась. Так сирена не работает: на посту она звучит до тех пор, пока
//  человек не нажмёт «принял», и на телефоне должна вести себя так же.
//
//  ЗВУК СИСТЕМНЫЙ, КАК ПРОСИЛ ЗАКАЗЧИК: берётся сигнал будильника устройства.
//  Своих файлов нет — системный оператор узнаёт, он одинаково слышен на любом
//  телефоне и не пропадёт при обновлении приложения.
//
//  ИГРАЕТ ПО КАНАЛУ БУДИЛЬНИКА. Не «уведомления»: их громкость убавляют, чтобы
//  не отвлекали, и вместе с ними убавили бы тревогу. Будильник для того и
//  существует, чтобы его услышали.
// ---------------------------------------------------------------------------

object AlarmSiren {

    private const val TAG = "PoolSafety"

    private var player: MediaPlayer? = null
    private var vibrator: Vibrator? = null

    /// Звучит ли сирена прямо сейчас.
    @Volatile
    var isSounding: Boolean = false
        private set

    @Synchronized
    fun start(context: Context) {
        if (isSounding) return

        val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM)
            ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
            ?: return

        runCatching {
            player = MediaPlayer().apply {
                setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_ALARM)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build()
                )
                setDataSource(context, uri)
                isLooping = true
                prepare()
                start()
            }
            isSounding = true
            startVibration(context)
        }.onFailure {
            Log.w(TAG, "сирена не запустилась: ${it.message}")
            stop()
        }
    }

    /// Заглушить сирену. Тревога при этом НЕ снимается: на посту она идёт
    /// дальше, и решение по ней принимается отдельно.
    @Synchronized
    fun stop() {
        runCatching { player?.stop() }
        runCatching { player?.release() }
        player = null

        runCatching { vibrator?.cancel() }
        vibrator = null

        isSounding = false
    }

    private fun startVibration(context: Context) {
        val manager = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            context.getSystemService<VibratorManager>()?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService<Vibrator>()
        }
        vibrator = manager ?: return

        // Тревогу надо чувствовать и в шумном помещении, и с телефоном в
        // кармане. Рисунок повторяется, пока сирену не заглушат.
        val pattern = longArrayOf(0, 600, 400, 600, 400)
        runCatching {
            vibrator?.vibrate(VibrationEffect.createWaveform(pattern, 0))
        }
    }
}
