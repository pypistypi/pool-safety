package ru.poolsafety.watch

import android.Manifest
import android.app.PictureInPictureParams
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.util.Rational
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.launch
import ru.poolsafety.watch.databinding.ActivityMainBinding
import ru.poolsafety.watch.net.AlarmCommand
import ru.poolsafety.watch.net.AlarmPhase
import ru.poolsafety.watch.net.Connection
import ru.poolsafety.watch.notify.AlarmSiren
import ru.poolsafety.watch.net.EventKind
import ru.poolsafety.watch.net.Panel
import ru.poolsafety.watch.net.UpdateChecker
import ru.poolsafety.watch.net.UpdateResult
import ru.poolsafety.watch.net.showUpdateDialog
import ru.poolsafety.watch.service.WatchService
import ru.poolsafety.watch.ui.PanelHolder

// ---------------------------------------------------------------------------
//  Экран оператора на телефоне.
//
//  ДВА РЕЖИМА ПРОСМОТРА, КАК И ПРОСИЛ ЗАКАЗЧИК: все четыре камеры сразу или
//  одна выбранная. Переключение — одним нажатием по клетке, без меню: на
//  телефоне, который держат одной рукой, меню лишнее.
//
//  ЭКРАН НЕ ВЕДЁТ СВЯЗЬ. Связь ведёт служба, а экран только смотрит на её
//  состояние. Иначе всё, что видит оператор, пропадало бы вместе с окном.
// ---------------------------------------------------------------------------

class MainActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_PANEL = "panel"

        /// Экран открыли кнопкой «Принять» в уведомлении — сирену пора
        /// заглушить: человек уже смотрит на камеру.
        const val EXTRA_MUTE = "mute"
    }

    private lateinit var binding: ActivityMainBinding
    private lateinit var prefs: Prefs
    private lateinit var holders: List<PanelHolder>

    /// Какая панель показана крупно. −1 — показаны все.
    private var single = -1

    /// Последняя камера, которую разворачивали на весь экран.
    ///
    /// НУЖНА ДЛЯ КАРТИНКИ В КАРТИНКЕ. В окошко размером со спичечный коробок
    /// сетка из четырёх клеток не помещается по смыслу: разглядеть там нечего.
    /// Уходя в него из режима «все четыре», показываем ту камеру, которую
    /// оператор смотрел последней, — почти наверняка именно она ему и нужна.
    private var lastSingle = 0

    private val askNotifications = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (!granted) {
            Toast.makeText(this, R.string.notifications_denied, Toast.LENGTH_LONG).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        prefs = Prefs(this)

        holders = listOf(
            PanelHolder(this, binding.panel0),
            PanelHolder(this, binding.panel1),
            PanelHolder(this, binding.panel2),
            PanelHolder(this, binding.panel3)
        )
        holders.forEachIndexed { index, holder ->
            holder.attachGestures { toggleSingle(index) }
        }

        binding.ackButton.setOnClickListener { send(AlarmCommand.Acknowledge) }
        binding.falseButton.setOnClickListener { send(AlarmCommand.FalseAlarm) }
        binding.confirmButton.setOnClickListener { send(AlarmCommand.Confirm) }
        binding.closeButton.setOnClickListener { send(AlarmCommand.Close) }

        binding.modeButton.setOnClickListener { toggleSingle(if (single >= 0) -1 else 0) }
        binding.settingsButton.setOnClickListener { openSettings() }
        binding.setupButton.setOnClickListener { openSettings() }
        binding.connectionToggle.setOnClickListener { toggleConnection() }

        handleIntent(intent)
        requestNotificationPermission()
        observeService()
    }

    override fun onStart() {
        super.onStart()
        applyConfigured()
        // Отключённый вручную телефон не пытается связаться сам: скорее всего
        // оператор просто открыл приложение вне домашней сети посмотреть
        // настройки, а не вернулся к посту наблюдения.
        if (prefs.isConfigured && !prefs.manuallyDisconnected) WatchService.start(this)
        showConnectionToggle()
        applyMode()
        maybeCheckForUpdate()
    }

    /// Раз в сутки, и только если оператор не выключил проверку в настройках.
    ///
    /// НЕ ЧАЩЕ: приложение открывают по многу раз за смену, а спрашивать
    /// GitHub на каждое открытие — значит дёргать сеть ради вопроса, ответ на
    /// который за минуты не меняется.
    private fun maybeCheckForUpdate() {
        if (!prefs.checkUpdates) return
        val dayMs = 24L * 60 * 60 * 1000
        if (System.currentTimeMillis() - prefs.lastUpdateCheckMs < dayMs) return

        lifecycleScope.launch {
            val result = UpdateChecker.check(BuildConfig.VERSION_NAME)
            prefs.lastUpdateCheckMs = System.currentTimeMillis()
            // Тихая проверка: молчим и про «нет обновлений», и про неудачу.
            // Диалогом беспокоим только когда есть что предложить.
            if (result is UpdateResult.Available && !isFinishing) {
                showUpdateDialog(this@MainActivity, result.info)
            }
        }
    }

    override fun onStop() {
        // Уходя с экрана, гасим потоки — но только если не ушли в картинку в
        // картинке: там окно живёт дальше и видео обязано идти.
        if (!isInPictureInPictureMode) {
            holders.forEach { it.setVisible(false) }
        }
        super.onStop()
    }

    override fun onDestroy() {
        holders.forEach { it.release() }
        super.onDestroy()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent) {
        if (intent.getBooleanExtra(EXTRA_MUTE, false)) AlarmSiren.stop()
        val panel = intent.getIntExtra(EXTRA_PANEL, -1)
        if (panel in holders.indices) toggleSingle(panel)
    }

    // --------------------------------------------------------- состояние

    private fun applyConfigured() {
        val ready = prefs.isConfigured
        binding.setupHint.visibility = if (ready) View.GONE else View.VISIBLE
        binding.grid.visibility = if (ready) View.VISIBLE else View.GONE
        binding.modeButton.visibility = if (ready) View.VISIBLE else View.GONE
    }

    private fun observeService() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    WatchService.connection.collect { showConnection(it) }
                }
                launch {
                    WatchService.panels.collect { showPanels(it) }
                }
                launch {
                    WatchService.alarmPhase.collect { showAlarmBar(it) }
                }
                launch {
                    WatchService.lastEvent.collect { event ->
                        event ?: return@collect
                        holders.getOrNull(event.panelId)
                            ?.applyEvent(event.kind, event.level)
                        // Тревога сама разворачивает нужную камеру: оператор,
                        // открывший приложение по уведомлению, должен увидеть
                        // место происшествия, а не сетку из четырёх клеток.
                        if (event.kind == EventKind.Alarm && event.panelId in holders.indices) {
                            toggleSingle(event.panelId)
                        }
                    }
                }
            }
        }
    }

    /// Отправить решение по тревоге.
    private fun send(action: AlarmCommand) {
        android.util.Log.i("PoolSafety", "нажата кнопка: ${action.wire}")
        val send = WatchService.sendCommand
        if (send == null) {
            android.util.Log.w("PoolSafety", "служба не отдала способ отправки")
            Toast.makeText(this, R.string.no_connection_for_command, Toast.LENGTH_SHORT)
                .show()
            return
        }
        send(action)
    }

    /// Полоса тревоги: видна ровно тогда, когда тревога идёт.
    private fun showAlarmBar(phase: AlarmPhase) {
        val visible = phase.isActive && !isInPictureInPictureMode
        binding.alarmBar.visibility = if (visible) View.VISIBLE else View.GONE
        if (!visible) return

        val event = WatchService.lastEvent.value
        binding.alarmTitle.text = when (phase) {
            AlarmPhase.Raised -> getString(R.string.alarm_raised)
            AlarmPhase.Acknowledged -> getString(R.string.alarm_acknowledged)
            AlarmPhase.Confirmed -> getString(R.string.alarm_confirmed)
            AlarmPhase.Idle -> ""
        }
        binding.alarmDetails.text = listOfNotNull(
            event?.zone?.takeIf { it.isNotBlank() },
            event?.title?.takeIf { it.isNotBlank() }
        ).joinToString(" · ")

        // Набор кнопок зависит от того, что уже решено. Лишняя кнопка в такой
        // момент — лишняя мысль.
        //
        //   объявлена  → принял / ложная / помощь вызвана
        //   принята    → ложная / помощь вызвана
        //   помощь     → происшествие закрыто
        val helpCalled = phase == AlarmPhase.Confirmed
        binding.ackButton.visibility =
            if (phase == AlarmPhase.Raised) View.VISIBLE else View.GONE
        binding.falseButton.visibility = if (helpCalled) View.GONE else View.VISIBLE
        binding.confirmButton.visibility = if (helpCalled) View.GONE else View.VISIBLE
        binding.closeButton.visibility = if (helpCalled) View.VISIBLE else View.GONE

        // Открыли экран по тревоге — звук больше не нужен: человек уже здесь.
        AlarmSiren.stop()
    }

    private fun showConnection(state: Connection) {
        binding.connectionState.text = when {
            prefs.manuallyDisconnected -> getString(R.string.connection_disconnected)
            state is Connection.Online -> "на связи с ${state.host}"
            state is Connection.Connecting ->
                if (state.attempt <= 1) "подключение…"
                else "переподключение (попытка ${state.attempt})…"
            state is Connection.Failed -> "нет связи: ${state.reason}"
            else -> getString(R.string.connection_offline)
        }
    }

    /// Переключатель «Отключиться» / «Подключиться» рядом со строкой связи.
    ///
    /// СКРЫТ, ПОКА КОМПЬЮТЕР НЕ ВЫБРАН: отключаться не от чего, а кнопка на
    /// пустом месте только сбивала бы с толку человека, впервые открывшего
    /// приложение.
    private fun showConnectionToggle() {
        binding.connectionToggle.visibility =
            if (prefs.isConfigured) View.VISIBLE else View.GONE
        binding.connectionToggle.setText(
            if (prefs.manuallyDisconnected) R.string.reconnect else R.string.disconnect
        )
    }

    private fun toggleConnection() {
        if (prefs.manuallyDisconnected) {
            prefs.manuallyDisconnected = false
            WatchService.start(this)
        } else {
            prefs.manuallyDisconnected = true
            WatchService.stop(this)
        }
        showConnectionToggle()
        showConnection(WatchService.connection.value)
    }

    private fun showPanels(panels: List<Panel>) {
        holders.forEachIndexed { index, holder ->
            holder.bind(panels.firstOrNull { it.id == index })
        }
        applyMode()
    }

    // ------------------------------------------------------------ режимы

    private fun toggleSingle(index: Int) {
        single = if (single == index) -1 else index
        if (single >= 0) lastSingle = single
        // Приближение принадлежит одной камере в одном режиме: перенося его
        // на сетку, мы показали бы обрезки кадров.
        holders.forEach { it.resetZoom() }
        applyMode()
    }

    private fun applyMode() {
        val showingAll = single < 0

        binding.modeButton.setText(if (showingAll) R.string.show_one else R.string.show_all)

        holders.forEachIndexed { index, holder ->
            holder.setVisible(showingAll || index == single)
        }

        // Пустые строки убираем целиком, иначе видимая панель займёт лишь
        // четверть экрана, а остальное останется чёрным.
        binding.rowTop.visibility =
            if (showingAll || single <= 1) View.VISIBLE else View.GONE
        binding.rowBottom.visibility =
            if (showingAll || single >= 2) View.VISIBLE else View.GONE
    }

    // --------------------------------------------- картинка в картинке

    override fun onUserLeaveHint() {
        super.onUserLeaveHint()
        enterPipIfPossible()
    }

    private fun enterPipIfPossible() {
        if (!prefs.isConfigured) return
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        if (!packageManager.hasSystemFeature(PackageManager.FEATURE_PICTURE_IN_PICTURE)) return

        // В картинке в картинке помещается только одна камера. Если оператор
        // смотрел сетку, берём ту, которую он разворачивал последней.
        if (single < 0) {
            val wanted = lastSingle.takeIf { it in holders.indices } ?: 0
            toggleSingle(wanted)
        }

        runCatching {
            enterPictureInPictureMode(
                PictureInPictureParams.Builder()
                    .setAspectRatio(Rational(16, 9))
                    .build()
            )
        }
    }

    override fun onPictureInPictureModeChanged(
        inPip: Boolean,
        config: Configuration
    ) {
        super.onPictureInPictureModeChanged(inPip, config)
        // В окошке нет места ни на шапку, ни на подсказку.
        binding.header.visibility = if (inPip) View.GONE else View.VISIBLE
        showAlarmBar(WatchService.alarmPhase.value)
    }

    // ------------------------------------------------------------ прочее

    private fun openSettings() {
        startActivity(Intent(this, SettingsActivity::class.java))
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        val granted = ContextCompat.checkSelfPermission(
            this, Manifest.permission.POST_NOTIFICATIONS
        ) == PackageManager.PERMISSION_GRANTED
        if (!granted) askNotifications.launch(Manifest.permission.POST_NOTIFICATIONS)
    }
}
