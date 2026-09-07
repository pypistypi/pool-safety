package ru.poolsafety.watch

import android.os.Bundle
import android.view.MenuItem
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import ru.poolsafety.watch.databinding.ActivitySettingsBinding
import ru.poolsafety.watch.net.DiscoveryClient
import ru.poolsafety.watch.net.Protocol
import ru.poolsafety.watch.net.UpdateChecker
import ru.poolsafety.watch.net.UpdateResult
import ru.poolsafety.watch.net.showUpdateDialog
import ru.poolsafety.watch.service.WatchService

// ---------------------------------------------------------------------------
//  Настройки.
//
//  ПОИСК ЕСТЬ, НО РУЧНОЙ ВВОД ОСТАЁТСЯ. Широковещательный запрос доходит не в
//  каждой сети: гостевые сети с разделением клиентов режут его. Поиск —
//  удобство; поле адреса — то, что работает всегда.
// ---------------------------------------------------------------------------

class SettingsActivity : AppCompatActivity() {

    private lateinit var binding: ActivitySettingsBinding
    private lateinit var prefs: Prefs

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)

        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        title = getString(R.string.settings_title)

        prefs = Prefs(this)
        load()

        binding.searchButton.setOnClickListener { search() }
        binding.saveButton.setOnClickListener { save() }
        binding.checkUpdateButton.setOnClickListener { checkForUpdate() }
    }

    private fun load() {
        binding.hostField.setText(prefs.host)
        binding.portField.setText(prefs.port.toString())
        binding.soundSwitch.isChecked = prefs.soundEnabled
        binding.volumeSlider.value = prefs.volume
        binding.presenceSwitch.isChecked = prefs.notifyPresence
        binding.attentionSwitch.isChecked = prefs.notifyAttention
        binding.clearSwitch.isChecked = prefs.notifyClear
        binding.updateCheckSwitch.isChecked = prefs.checkUpdates
        binding.versionLabel.text = getString(R.string.updates_title) +
            ": установлена " + BuildConfig.VERSION_NAME
    }

    /// Проверка по нажатию кнопки — в обход суточного ограничения: раз
    /// оператор попросил явно, ждать сутки незачем.
    private fun checkForUpdate() {
        binding.checkUpdateButton.isEnabled = false
        binding.updateResult.text = getString(R.string.updates_checking)

        lifecycleScope.launch {
            val result = UpdateChecker.check(BuildConfig.VERSION_NAME)
            prefs.lastUpdateCheckMs = System.currentTimeMillis()
            binding.checkUpdateButton.isEnabled = true

            when (result) {
                is UpdateResult.Available -> {
                    binding.updateResult.text = ""
                    showUpdateDialog(this@SettingsActivity, result.info)
                }
                UpdateResult.UpToDate ->
                    binding.updateResult.text = getString(R.string.updates_none)
                UpdateResult.Failed ->
                    binding.updateResult.text = getString(R.string.updates_failed)
            }
        }
    }

    private fun search() {
        binding.searchButton.isEnabled = false
        binding.searchResult.text = getString(R.string.searching)

        lifecycleScope.launch {
            val found = DiscoveryClient.search()
            binding.searchButton.isEnabled = true

            if (found.isEmpty()) {
                binding.searchResult.text = getString(R.string.search_none)
                return@launch
            }

            val first = found.first()
            binding.hostField.setText(first.host)
            binding.portField.setText(first.port.toString())
            binding.searchResult.text = if (found.size == 1) {
                "Найден компьютер ${first.host}, панелей: ${first.panels}"
            } else {
                "Найдено компьютеров: ${found.size}. Взят первый — ${first.host}."
            }
        }
    }

    private fun save() {
        val host = binding.hostField.text?.toString()?.trim().orEmpty()
        if (host.isEmpty()) {
            binding.hostField.error = "Укажите адрес компьютера"
            return
        }

        val addressChanged = prefs.host != host ||
            prefs.port != (binding.portField.text?.toString()?.trim()?.toIntOrNull()
                ?: Protocol.DEFAULT_PORT)

        prefs.host = host
        prefs.port = binding.portField.text?.toString()?.trim()?.toIntOrNull()
            ?: Protocol.DEFAULT_PORT
        prefs.soundEnabled = binding.soundSwitch.isChecked
        prefs.volume = binding.volumeSlider.value
        prefs.notifyPresence = binding.presenceSwitch.isChecked
        prefs.notifyAttention = binding.attentionSwitch.isChecked
        prefs.notifyClear = binding.clearSwitch.isChecked
        prefs.checkUpdates = binding.updateCheckSwitch.isChecked

        // Служба подхватывает настройки сразу: те, что вступают в силу
        // «когда-нибудь потом», оператор считает несохранёнными.
        //
        // ОДНА КОМАНДА, А НЕ «ОСТАНОВИТЬ И ЗАПУСТИТЬ». Пара команд подряд
        // роняла приложение: остановка срабатывала уже после запуска и уносила
        // с собой только что поднятую службу. Служба сама переподключается с
        // новым адресом, получив запуск повторно.
        // Адрес не менялся — рвать живую связь незачем: остальные настройки
        // служба читает при каждом событии.
        if (addressChanged) WatchService.stop(this)
        WatchService.start(this)

        Toast.makeText(this, R.string.saved, Toast.LENGTH_SHORT).show()
        finish()
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == android.R.id.home) {
            finish()
            return true
        }
        return super.onOptionsItemSelected(item)
    }
}
