package ru.poolsafety.watch.net

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.appcompat.app.AlertDialog
import ru.poolsafety.watch.BuildConfig
import ru.poolsafety.watch.R

// ---------------------------------------------------------------------------
//  Окно «доступно обновление».
//
//  СКАЧИВАНИЕ ИДЁТ ЧЕРЕЗ БРАУЗЕР, А НЕ ВНУТРИ ПРИЛОЖЕНИЯ. Загрузить APK и
//  запустить установку самим — значит просить у Android отдельное разрешение
//  «устанавливать неизвестные приложения» и городить обработку файлового
//  провайдера. Браузер и системный загрузчик умеют это надёжно и без нашего
//  участия: нажал «Скачать» — файл лёг в «Загрузки», Android сам предложил
//  его открыть.
// ---------------------------------------------------------------------------

fun showUpdateDialog(context: Context, info: UpdateInfo) {
    AlertDialog.Builder(context)
        .setTitle(R.string.updates_available_title)
        .setMessage(
            context.getString(
                R.string.updates_available_message,
                info.version,
                BuildConfig.VERSION_NAME
            )
        )
        .setPositiveButton(R.string.updates_download) { _, _ ->
            context.startActivity(
                Intent(Intent.ACTION_VIEW, Uri.parse(info.downloadUrl))
            )
        }
        .setNegativeButton(R.string.updates_later, null)
        .show()
}
