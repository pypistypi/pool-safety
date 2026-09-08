#include "ui/MainWindow.h"
#include "ui/CameraPanel.h"
#include "ui/AlarmCard.h"
#include "ui/AlarmDialog.h"
#include "ui/Chime.h"
#include "ui/EventLogDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"

#include "core/SourceHub.h"
#include "core/DeviceRegistry.h"
#include "core/AlarmController.h"
#include "core/AnalysisWorker.h"
#include "core/EventLog.h"
#include "core/PersonDetector.h"
#include "core/PoseEstimator.h"
#include "core/EventServer.h"
#include "core/AppPaths.h"
#include "core/CameraDiscovery.h"
#include "core/SingleInstance.h"

#include <QApplication>
#include <QUrl>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QStatusBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QCloseEvent>
#include <QScreen>
#include <QMessageBox>
#include <QFile>
#include <QDir>
#include <QCameraDevice>
#include <QFileInfo>
#include <QSystemTrayIcon>
#include <QMenu>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ui {

namespace {

// Названия зон. Порядок соответствует расположению панелей в сетке.
const QStringList &zoneNames()
{
    static const QStringList names = {
        QStringLiteral("Северо-западный угол"),
        QStringLiteral("Северо-восточный угол"),
        QStringLiteral("Юго-западный угол"),
        QStringLiteral("Юго-восточный угол")
    };
    return names;
}

// Сколько секунд висит уведомление о появлении людей. Достаточно, чтобы
// заметить, и мало, чтобы не мозолить глаза.
constexpr int kNoticeSeconds = 8;

/// Во что превращается найденное системой положение при объявлении тревоги.
///
/// Обстоятельство — это то, что оператор скажет диспетчеру. Поэтому
/// сопоставление здесь не косметическое: от него зависит и текст для звонка, и
/// подсказка о первых действиях.
core::AlarmCircumstance circumstanceFor(const core::DangerReport &report)
{
    const bool inWater = report.reasons.join(QLatin1Char(' '))
                             .contains(QStringLiteral("в воде"));

    switch (report.situation) {
    case core::Situation::Drowning:
        return core::AlarmCircumstance::Drowning;
    case core::Situation::Unconscious:
        return inWater ? core::AlarmCircumstance::UnconsciousInWater
                       : core::AlarmCircumstance::UnconsciousPoolside;
    case core::Situation::Fall:
        return core::AlarmCircumstance::Injury;
    case core::Situation::ChildAlone:
        return core::AlarmCircumstance::ChildUnattended;
    case core::Situation::Unsteady:
        break;
    }
    return core::AlarmCircumstance::Other;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Наблюдение за бассейном — пост оператора"));

    m_hub = new core::SourceHub(this);
    m_devices = new core::DeviceRegistry(this);
    m_alarm = new core::AlarmController(this);
    m_chime = new Chime(this);
    m_log = new core::EventLog(this);
    m_server = new core::EventServer(this);

    // Сведения об объекте и настройки читаются один раз при запуске. Если
    // файлов нет, они создаются с заготовками — администратору останется
    // вписать адрес и телефоны.
    m_site = core::SiteInfo::load(core::SiteInfo::defaultPath());
    m_settings = core::Settings::load(core::Settings::defaultPath());

    buildUi();
    buildMenu();
    wireAlarm();
    startAnalysisThread();
    applyStartupGeometry();
    applyCommandLine();

    connect(m_devices, &core::DeviceRegistry::devicesChanged, this, [this] {
        statusBar()->showMessage(
            QStringLiteral("Состав видеоустройств изменился"), 4000);
    });

    buildTray();

    if (m_settings.serverEnabled) {
        if (m_server->start(quint16(m_settings.serverPort))) {
            m_log->write(core::EventLog::Kind::Info,
                         QStringLiteral("Телефоны могут подключаться: %1")
                             .arg(m_server->localAddress()));
            // Телефон присылает те же решения, что и оператор на посту.
            connect(m_server, &core::EventServer::commandReceived,
                    this, &MainWindow::onPhoneCommand);

            connect(m_server, &core::EventServer::clientsChanged, this,
                    [this](int count) {
                        statusBar()->showMessage(
                            count > 0
                                ? QStringLiteral("Телефонов подключено: %1").arg(count)
                                : QStringLiteral("Телефоны отключились"), 5000);
                    });
        } else {
            showNotice(QStringLiteral(
                           "Не удалось открыть порт %1 для телефонов: %2. "
                           "Уведомления на телефон работать не будут.")
                           .arg(m_settings.serverPort, 0, 10)
                           .arg(m_server->lastError()), true);
        }
    }
    publishPanels();

    m_log->write(core::EventLog::Kind::Info,
                 QStringLiteral("Пост наблюдения запущен"));

    if (!m_site.isAddressReady()) {
        showNotice(QStringLiteral(
            "Адрес объекта не задан. Откройте «Настройки → Объект» и заполните его — "
            "иначе при тревоге оператору нечего будет продиктовать скорой."), true);
        m_log->write(core::EventLog::Kind::Info,
                     QStringLiteral("Адрес объекта не заполнен"));
    }
}

MainWindow::~MainWindow()
{
    // Поток останавливается явно и до разрушения остальных объектов: иначе он
    // мог бы обратиться к уже уничтоженному воркеру.
    if (m_analysisThread.isRunning()) {
        m_analysisThread.quit();
        m_analysisThread.wait(3000);
    }
}

// ------------------------------------------------------------------ сборка

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(12, 8, 12, 8);
    root->setSpacing(8);

    // --- шапка -----------------------------------------------------------
    auto *header = new QHBoxLayout;
    header->setSpacing(14);

    auto *title = new QLabel(QStringLiteral("Наблюдение за бассейном"), central);
    title->setObjectName(QStringLiteral("Title"));
    header->addWidget(title);

    // СИГНАЛ 1 живёт здесь — в спокойной части окна, отдельно от тревоги.
    m_presenceLabel = new QLabel(central);
    m_presenceLabel->setMinimumWidth(200);
    header->addWidget(m_presenceLabel);

    header->addStretch(1);

    m_detectorLabel = new QLabel(central);
    m_detectorLabel->setObjectName(QStringLiteral("Subtitle"));
    header->addWidget(m_detectorLabel);

    m_alarmButton = new QPushButton(QStringLiteral("ОБЪЯВИТЬ ТРЕВОГУ"), central);
    m_alarmButton->setObjectName(QStringLiteral("AlarmButton"));
    m_alarmButton->setCursor(Qt::PointingHandCursor);
    m_alarmButton->setToolTip(QStringLiteral("Объявить тревогу по всей зоне бассейна"));
    connect(m_alarmButton, &QPushButton::clicked, this, [this] { onAlarmRequested(-1); });
    header->addWidget(m_alarmButton);

    root->addLayout(header);

    // --- строка уведомления (сигнал 1 текстом) ---------------------------
    //
    // Отдельная строка, а не всплывающее окно: окно пришлось бы закрывать, а
    // появление людей в зоне — обычное событие, случающееся десятки раз за
    // смену. Оно должно сообщаться заметно, но не требовать действий.
    m_notice = new QLabel(central);
    m_notice->setWordWrap(true);
    m_notice->hide();
    root->addWidget(m_notice);

    // --- карточка тревоги (скрыта, пока тревоги нет) ---------------------
    m_card = new AlarmCard(central);
    m_card->hide();
    root->addWidget(m_card);

    // --- сетка панелей ---------------------------------------------------
    auto *gridHost = new QWidget(central);
    buildPanels(gridHost);
    root->addWidget(gridHost, 1);

    setCentralWidget(central);

    // --- строка состояния ------------------------------------------------
    m_stateLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_stateLabel);
    statusBar()->showMessage(QStringLiteral("Запуск распознавания…"));

    updatePresenceIndicator();

    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    m_tick->setTimerType(Qt::CoarseTimer);
    connect(m_tick, &QTimer::timeout, this, &MainWindow::refreshReadouts);
    m_tick->start();

    // Мигание запускается только на время непринятой тревоги: гонять
    // перерисовку четырёх панелей четыре раза в секунду просто так — это
    // впустую отданные проценты процессора.
    m_blink = new QTimer(this);
    m_blink->setInterval(280);
    connect(m_blink, &QTimer::timeout, this, &MainWindow::onBlinkTick);
}

void MainWindow::buildMenu()
{
    auto *settingsMenu = menuBar()->addMenu(QStringLiteral("Настройки"));

    QAction *settingsAction = settingsMenu->addAction(
        QStringLiteral("Объект, телефоны, тревога…"));
    settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    settingsMenu->addSeparator();

    m_skeletonAction = settingsMenu->addAction(QStringLiteral("Показывать скелеты"));
    m_skeletonAction->setCheckable(true);
    // Выключено по умолчанию: оператору нужна рамка, а не семнадцать точек.
    // Пункт остаётся — по нему проверяют биомеханику при настройке правил.
    m_skeletonAction->setChecked(false);
    connect(m_skeletonAction, &QAction::toggled, this, [this](bool on) {
        for (CameraPanel *panel : m_panels)
            panel->setSkeletonsVisible(on);
    });

    auto *journalMenu = menuBar()->addMenu(QStringLiteral("Журнал"));
    QAction *journalAction = journalMenu->addAction(QStringLiteral("Журнал событий…"));
    journalAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(journalAction, &QAction::triggered, this, &MainWindow::openEventLog);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("Справка"));
    connect(helpMenu->addAction(QStringLiteral("Что программа умеет и чего не умеет")),
            &QAction::triggered, this, [this] {
        QMessageBox::information(this, QStringLiteral("О системе наблюдения"),
            QStringLiteral(
                "Система решает одну задачу: НЕ ДАТЬ ЧЕЛОВЕКУ ОСТАТЬСЯ НЕЗАМЕЧЕННЫМ.\n\n"
                "Надёжно распознаётся:\n"
                "  • человек лежит и не двигается;\n"
                "  • человек упал и не поднялся;\n"
                "  • в зоне есть люди и сколько их.\n\n"
                "С оговорками:\n"
                "  • человек завис в воде вертикально и не продвигается — признак, "
                "похожий на утопление, но не доказательство;\n"
                "  • ребёнок без взрослых рядом — по пропорциям тела.\n\n"
                "НЕ распознаётся и не будет: утопление как диагноз, опьянение, "
                "личность человека. Лица не распознаются вовсе — система работает "
                "только с положением тела и не хранит персональных данных.\n\n"
                "Всё считается на этом компьютере. Ни один кадр не уходит в "
                "интернет."));
    });
}

void MainWindow::buildPanels(QWidget *gridHost)
{
    auto *grid = new QGridLayout(gridHost);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(10);

    const QStringList &names = zoneNames();
    for (int i = 0; i < names.size(); ++i) {
        auto *panel = new CameraPanel(i, names.at(i), m_hub, m_devices, gridHost);
        connect(panel, &CameraPanel::alarmRequested,
                this, &MainWindow::onAlarmRequested);
        connect(panel, &CameraPanel::sourceChanged, this,
                [this](int panelId, const core::SourceDescriptor &descriptor) {
                    // Источник сменили — прежние сведения о людях к новой
                    // картинке отношения не имеют и должны исчезнуть сразу,
                    // включая накопленное в потоке разбора.
                    m_alarm->forgetPanel(panelId);
                    if (m_analysis) {
                        QMetaObject::invokeMethod(m_analysis, "forgetPanel",
                                                  Qt::QueuedConnection,
                                                  Q_ARG(int, panelId));
                    }
                    if (panelId >= 0 && panelId < m_panels.size())
                        m_panels.at(panelId)->clearAnalysis();

                    const QString text =
                        descriptor.isNone()
                            ? QStringLiteral("Источник отключён")
                            : QStringLiteral("Источник: %1").arg(descriptor.displayName());
                    statusBar()->showMessage(
                        QStringLiteral("Панель %1: %2").arg(panelId + 1).arg(text), 4000);
                    m_log->write(core::EventLog::Kind::Info, text, panelId);
                    publishPanels();
                });

        grid->addWidget(panel, i / 2, i % 2);
        m_panels.append(panel);
    }

    // Все клетки равны: ни одна зона не важнее другой.
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
}

void MainWindow::wireAlarm()
{
    connect(m_alarm, &core::AlarmController::stateChanged,
            this, &MainWindow::onAlarmStateChanged);
    connect(m_alarm, &core::AlarmController::alarmRaised,
            this, &MainWindow::onAlarmRaised);
    // Тревога поднялась или закрылась — обстановка по панелям изменилась.
    connect(m_alarm, &core::AlarmController::stateChanged,
            this, [this](core::AlarmState) { schedulePanelsPublish(); });
    connect(m_alarm, &core::AlarmController::presenceChanged,
            this, &MainWindow::onPresenceChanged);
    // Телефон должен видеть не только «в зоне кто-то есть», но и на какой
    // именно панели. Без этого рамка нужной камеры не загорится.
    connect(m_alarm, &core::AlarmController::presenceChanged,
            this, [this](int, int) { schedulePanelsPublish(); });
    connect(m_alarm, &core::AlarmController::zoneOccupancyChanged,
            this, &MainWindow::onZoneOccupancyChanged);

    connect(m_card, &AlarmCard::acknowledgeRequested,
            m_alarm, &core::AlarmController::acknowledge);
    connect(m_card, &AlarmCard::falseAlarmRequested,
            m_alarm, &core::AlarmController::dismissAsFalse);
    connect(m_card, &AlarmCard::confirmRequested,
            m_alarm, &core::AlarmController::confirm);
    connect(m_card, &AlarmCard::closeIncidentRequested,
            m_alarm, &core::AlarmController::closeIncident);

    connect(m_card, &AlarmCard::detailsEdited, this, [this](const QString &details) {
        m_alarm->updateDetails(details);
        // Текст для звонка пересобирается сразу: оператор дописывает
        // подробности уже во время разговора.
        m_card->applyState(m_alarm->state(), m_alarm->currentEvent(), m_site);
    });

    connect(m_alarm, &core::AlarmController::alarmClosed, this,
            [this](const core::AlarmEvent &event, bool wasFalse) {
                const QString text =
                    wasFalse
                        ? QStringLiteral("Тревога признана ложной (%1)").arg(event.zoneLabel)
                        : QStringLiteral("Инцидент закрыт (%1): %2")
                              .arg(event.zoneLabel,
                                   core::circumstanceText(event.circumstance));
                showNotice(text + QStringLiteral(". Наблюдение продолжается."), false);
                m_log->write(core::EventLog::Kind::Operator, text, event.panelId);
            });

    connect(m_alarm, &core::AlarmController::alarmAcknowledged, this,
            [this](const core::AlarmEvent &event) {
                m_log->write(core::EventLog::Kind::Operator,
                             QStringLiteral("Оператор принял тревогу"), event.panelId);
            });

    connect(m_alarm, &core::AlarmController::alarmConfirmed, this,
            [this](const core::AlarmEvent &event) {
                m_log->write(core::EventLog::Kind::Operator,
                             QStringLiteral("Оператор подтвердил тревогу — помощь вызвана"),
                             event.panelId);
            });
}

void MainWindow::startAnalysisThread()
{
    m_analysis = new core::AnalysisWorker;   // без родителя: живёт в своём потоке
    m_analysis->moveToThread(&m_analysisThread);

    connect(&m_analysisThread, &QThread::finished,
            m_analysis, &QObject::deleteLater);

    // Сигналы из вычислительного потока Qt доставляет через очередь событий
    // автоматически, поэтому ничего блокировать не нужно.
    connect(m_analysis, &core::AnalysisWorker::presenceDetected,
            m_alarm, &core::AlarmController::reportPresence);
    connect(m_analysis, &core::AnalysisWorker::panelAnalysed,
            this, &MainWindow::onPanelAnalysed);
    connect(m_analysis, &core::AnalysisWorker::detectorReady,
            this, &MainWindow::onDetectorReady);
    connect(m_analysis, &core::AnalysisWorker::poseReady,
            this, &MainWindow::onPoseReady);
    connect(m_analysis, &core::AnalysisWorker::dangerDetected,
            this, &MainWindow::onDangerDetected);
    connect(m_analysis, &core::AnalysisWorker::attentionDetected,
            this, &MainWindow::onAttentionDetected);

    for (CameraPanel *panel : m_panels)
        panel->setAnalysisWorker(m_analysis);

    m_analysisThread.setObjectName(QStringLiteral("analysis"));
    m_analysisThread.start();

    // Настройки применяются ДО загрузки моделей: порог поиска людей учитывается
    // при открытии модели, а не на ходу.
    applySettingsToParts();

    // Модели открываются уже в своём потоке: это сотни миллисекунд, и в главном
    // потоке они задержали бы появление окна.
    QMetaObject::invokeMethod(
        m_analysis, "start", Qt::QueuedConnection,
        Q_ARG(QString, core::PersonDetector::modelPath(m_settings.detectorModel)),
        Q_ARG(QString, core::PoseEstimator::defaultModelPath()));
}

void MainWindow::applySettingsToParts()
{
    m_chime->setEnabled(m_settings.soundEnabled);
    m_chime->setVolumes(m_settings.alarmVolume, m_settings.presenceVolume);

    for (CameraPanel *panel : m_panels)
        panel->setFpsLimit(m_settings.displayFpsCap);

    if (!m_analysis)
        return;

    QMetaObject::invokeMethod(m_analysis, "setDetectIntervalMs", Qt::QueuedConnection,
                              Q_ARG(int, m_settings.detectIntervalMs));
    QMetaObject::invokeMethod(m_analysis, "setSearchConfidence", Qt::QueuedConnection,
                              Q_ARG(double, m_settings.searchConfidence));
    QMetaObject::invokeMethod(m_analysis, "setPoseEnabled", Qt::QueuedConnection,
                              Q_ARG(bool, m_settings.poseEnabled));

    for (int panelId = 0; panelId < m_settings.panelHasWater.size(); ++panelId) {
        QMetaObject::invokeMethod(m_analysis, "setPanelHasWater", Qt::QueuedConnection,
                                  Q_ARG(int, panelId),
                                  Q_ARG(bool, m_settings.panelHasWater.at(panelId)));
    }
    QMetaObject::invokeMethod(m_analysis, "applyThresholds", Qt::QueuedConnection,
                              Q_ARG(core::Thresholds, m_settings.thresholds));

    // Карта постоянного окружения принадлежит этой установке: своя расстановка
    // камер — своя карта. Поэтому файл лежит рядом с настройками программы, а
    // не поставляется с ней.
    QMetaObject::invokeMethod(m_analysis, "setSelfLearning", Qt::QueuedConnection,
                              Q_ARG(bool, m_settings.selfLearning),
                              Q_ARG(QString, backgroundModelPath()));
}

void MainWindow::applyCommandLine()
{
    // Запуск с готовой раскладкой источников:
    //
    //   PoolSafety.exe --panel1 "D:/видео/бассейн.mp4" --panel2 camera:Logitech
    //   PoolSafety.exe --panel1 rtsp://192.168.1.126:8080/h264_ulaw.sdp
    //
    // Полезно и оператору (не выбирать четыре источника вручную при каждом
    // запуске), и при проверках: программу можно запустить сразу в рабочем
    // состоянии и замерить нагрузку.
    const QStringList arguments = QApplication::arguments();

    for (int i = 1; i < arguments.size() - 1; ++i) {
        const QString key = arguments.at(i);
        if (!key.startsWith(QStringLiteral("--panel")))
            continue;

        bool ok = false;
        const int number = key.mid(7).toInt(&ok);
        if (!ok || number < 1 || number > m_panels.size())
            continue;

        const QString value = arguments.at(i + 1);
        CameraPanel *panel = m_panels.at(number - 1);

        if (value.startsWith(QStringLiteral("camera"), Qt::CaseInsensitive)) {
            // «camera» — первая доступная, «camera:часть имени» — нужная.
            const QString wanted = value.section(QLatin1Char(':'), 1).trimmed();
            const auto cameras = m_devices->cameras();
            for (const QCameraDevice &device : cameras) {
                if (!wanted.isEmpty()
                    && !device.description().contains(wanted, Qt::CaseInsensitive))
                    continue;
                panel->setSource(core::SourceDescriptor::camera(
                    QString::fromUtf8(device.id()), device.description()));
                break;
            }
        } else if (core::CameraDiscovery::parseUrl(value, nullptr)) {
            // Адрес потока IP-камеры. Проверяется тем же разбором, что и в
            // окне подключения: одно место — один ответ на вопрос «что
            // считается адресом камеры».
            panel->setSource(core::SourceDescriptor::network(
                value, QStringLiteral("Камера %1").arg(QUrl(value).host())));
        } else if (QFileInfo::exists(value)) {
            panel->setSource(core::SourceDescriptor::file(value));
        }
    }
}

void MainWindow::applyStartupGeometry()
{
    // Окно должно целиком помещаться на экране сразу при запуске — без
    // прокрутки и без ручного растягивания.
    const QScreen *screen = QApplication::primaryScreen();
    if (!screen) {
        resize(1280, 760);
        return;
    }

    const QRect available = screen->availableGeometry();
    const int width  = qMin(1440, int(available.width() * 0.92));
    const int height = qMin(880,  int(available.height() * 0.92));

    resize(width, height);
    move(available.center() - QPoint(width / 2, height / 2));
    setMinimumSize(900, 600);
}

QString MainWindow::zoneName(int panelId) const
{
    if (panelId >= 0 && panelId < m_panels.size())
        return m_panels.at(panelId)->zoneLabel();
    return QStringLiteral("зона бассейна");
}

// ------------------------------------------------------------- распознавание

void MainWindow::onDetectorReady(bool ready, const QString &message)
{
    if (ready) {
        m_detectorLabel->setText(QStringLiteral("Распознавание: работает"));
        m_detectorLabel->setStyleSheet(QStringLiteral("color: #939fb0;"));
        statusBar()->showMessage(
            QStringLiteral("Распознавание людей включено. "
                           "Выберите источники в правом верхнем углу панелей."), 8000);
        m_log->write(core::EventLog::Kind::Info,
                     QStringLiteral("Поиск людей включён, модель: %1")
                         .arg(m_analysis ? m_analysis->detectorName() : QString()));
        return;
    }

    // Видео продолжает показываться — наблюдать оператор сможет и так. Но
    // молчать нельзя: иначе он решит, что алгоритм работает и никого не видит.
    m_detectorLabel->setText(QStringLiteral("Распознавание НЕ РАБОТАЕТ"));
    m_detectorLabel->setStyleSheet(QStringLiteral("color: #ffd23f; font-weight: 600;"));
    showNotice(QStringLiteral("Распознавание людей не запустилось: %1. "
                              "Видео показывается, но людей система не ищет.")
                   .arg(message), true);
    m_log->write(core::EventLog::Kind::Info,
                 QStringLiteral("Поиск людей НЕ запустился: %1").arg(message));
}

void MainWindow::onPoseReady(bool ready, const QString &message)
{
    if (ready) {
        m_log->write(core::EventLog::Kind::Info,
                     QStringLiteral("Разбор поз и опасных положений включён"));
        return;
    }

    // Без позы система остаётся счётчиком людей. Это заметно меньше, чем
    // обещано, и умолчать об этом значило бы оставить оператора в уверенности,
    // что за неподвижным человеком следят.
    showNotice(QStringLiteral(
                   "Разбор поз не запустился: %1. Люди считаются, но опасные положения "
                   "(лежит без движения, упал, завис в воде) НЕ отслеживаются.")
                   .arg(message), true);
    m_log->write(core::EventLog::Kind::Info,
                 QStringLiteral("Разбор поз НЕ запустился: %1").arg(message));
}

QString MainWindow::backgroundModelPath()
{
    return core::paths::dataDir() + QStringLiteral("/config/окружение.json");
}

void MainWindow::publishPanels()
{
    if (!m_server)
        return;

    QVector<core::PanelInfo> info;
    for (CameraPanel *panel : m_panels) {
        const int id = panel->panelId();

        core::PanelInfo item;
        item.panelId = id;
        item.zone = panel->zoneLabel();
        item.streamUrl = panel->streamUrl();
        item.peopleCount = m_alarm->peopleOnPanel(id);
        // Уровень шлётся вместе с остальным: телефон, подключившийся посреди
        // тревоги, обязан увидеть её сразу, а не ждать следующего события.
        item.level = id >= 0 && id < m_panelLevels.size()
                         ? core::Level(m_panelLevels.at(id))
                         : core::Level::Normal;

        // ОБЪЯВЛЕННАЯ ТРЕВОГА ГЛАВНЕЕ РАЗБОРА КАДРА. Разбор говорит о том, что
        // видно сейчас, а тревога держится до решения оператора. Пока сюда
        // ехал только разбор, рамка на телефоне краснела на миг и тут же
        // зеленела обратно: следом приходил спокойный уровень и затирал её.
        if (m_alarm->state() != core::AlarmState::Idle
            && m_alarm->currentEvent().panelId == id) {
            item.level = core::Level::Alarm;
        }
        info.append(item);
    }
    m_server->setPanels(info);
}

void MainWindow::schedulePanelsPublish()
{
    if (!m_server)
        return;

    if (!m_panelsPublish) {
        m_panelsPublish = new QTimer(this);
        m_panelsPublish->setSingleShot(true);
        m_panelsPublish->setInterval(800);
        connect(m_panelsPublish, &QTimer::timeout, this, &MainWindow::publishPanels);
    }
    if (!m_panelsPublish->isActive())
        m_panelsPublish->start();
}

void MainWindow::onAlarmRaised(const core::AlarmEvent &event)
{
    // Сюда приходит и тревога от разбора кадра, и объявленная оператором.
    // Телефон обязан узнать об обеих и одинаково быстро.
    //
    // ДЛЯ ТЕЛЕФОНА — КОРОТКАЯ ФРАЗА, НЕ event.details. У тревоги от алгоритма
    // details несёт измерения («резкий переход в горизонталь (62°/с)») —
    // это то, что оператор читает диспетчеру («Уточнение: …» в
    // buildCallBrief), а не то, что нужно спасателю на бегу. phoneSummary
    // заполняется только для автоматической тревоги (см. onDangerDetected);
    // для тревоги, объявленной оператором вручную, details и так короткое.
    QString details = event.phoneSummary.isEmpty() ? event.details : event.phoneSummary;
    if (details.isEmpty())
        details = core::circumstanceAction(event.circumstance);

    notifyPhones(QStringLiteral("alarm"), event.panelId, event.zoneLabel,
                 core::circumstanceText(event.circumstance), details, 2);
}

void MainWindow::onPhoneCommand(core::EventServer::Command command)
{
    // РЕШЕНИЕ С ТЕЛЕФОНА РАВНОСИЛЬНО РЕШЕНИЮ НА ПОСТУ. Спасатель у воды видит
    // происходящее лучше того, кто смотрит в монитор, и заставлять его бежать
    // к компьютеру, чтобы выключить сирену, — значит отнимать секунды у
    // помощи. Никаких особых прав у телефона при этом нет: те же три кнопки.
    switch (command) {
    case core::EventServer::Command::Acknowledge:
        m_log->write(core::EventLog::Kind::Operator,
                     QStringLiteral("Тревогу принял оператор с телефона"),
                     m_alarm->currentEvent().panelId);
        m_alarm->acknowledge();
        break;
    case core::EventServer::Command::FalseAlarm:
        m_log->write(core::EventLog::Kind::Operator,
                     QStringLiteral("Тревога признана ложной с телефона"),
                     m_alarm->currentEvent().panelId);
        m_alarm->dismissAsFalse();
        break;
    case core::EventServer::Command::Confirm:
        m_log->write(core::EventLog::Kind::Operator,
                     QStringLiteral("Тревога подтверждена с телефона: помощь вызвана"),
                     m_alarm->currentEvent().panelId);
        m_alarm->confirm();
        break;
    case core::EventServer::Command::Close:
        // Происшествие закончено. Отдельным шагом, а не вместе с «помощь
        // вызвана»: между вызовом скорой и её отъездом проходит время, и всё
        // это время тревога должна оставаться на экране.
        m_log->write(core::EventLog::Kind::Operator,
                     QStringLiteral("Происшествие закрыто с телефона"),
                     m_alarm->currentEvent().panelId);
        m_alarm->closeIncident();
        break;
    }
}

void MainWindow::notifyPhones(const QString &kind, int panelId, const QString &zone,
                              const QString &title, const QString &details, int level)
{
    if (m_server)
        m_server->publishEvent(kind, panelId, zone, title, details, level);
}

void MainWindow::onPanelAnalysed(const core::PanelAnalysis &result)
{
    if (result.panelId >= 0 && result.panelId < m_panels.size())
        m_panels.at(result.panelId)->setAnalysis(result);

    if (result.averageTrackSeconds > 0.0)
        m_trackSeconds = result.averageTrackSeconds;

    // Запоминаем обстановку по панели и, если она изменилась, сообщаем
    // телефонам. Сравнение обязательно: разбор идёт по нескольку раз в
    // секунду, а меняется картина куда реже.
    const int id = result.panelId;
    if (id < 0)
        return;

    if (m_panelPeople.size() <= id)
        m_panelPeople.resize(id + 1);
    if (m_panelLevels.size() <= id)
        m_panelLevels.resize(id + 1);

    const int level = int(result.worstLevel);
    if (m_panelPeople.at(id) == result.peopleCount && m_panelLevels.at(id) == level)
        return;

    m_panelPeople[id] = result.peopleCount;
    m_panelLevels[id] = level;
    schedulePanelsPublish();
}

void MainWindow::onDangerDetected(const core::DangerReport &report)
{
    const QString zone = zoneName(report.panelId);
    const QString text = report.describe();

    m_log->write(core::EventLog::Kind::Alarm,
                 QStringLiteral("%1 — %2").arg(zone, text), report.panelId);

    if (!m_settings.reactionsEnabled || !m_settings.autoAlarm) {
        // Автоматическая тревога выключена — это режим настройки на новом
        // объекте. Панель всё равно подсвечена, событие записано в журнал, и
        // оператор о нём знает.
        //
        // ТЕЛЕФОНУ ЭТО ПРИХОДИТ КАК «ВНИМАНИЕ», А НЕ КАК ТРЕВОГА. Пока сюда
        // слалась тревога, телефон выл сиреной там, где на посту её нет: на
        // объекте, где автотревога ещё не настроена, это первый способ
        // приучить спасателя не верить сигналу.
        //
        // situationAlert(), А НЕ report.reasons — на телефоне короткий факт
        // без измерений, цифры и углы наклона там только мешают.
        notifyPhones(QStringLiteral("attention"), report.panelId, zone,
                     core::situationText(report.situation),
                     core::situationAlert(report.situation), 1);

        if (m_settings.reactionsEnabled) {
            showNotice(QStringLiteral("%1: %2. Автотревога выключена в настройках.")
                           .arg(zone, text), true);
        }
        return;
    }

    if (m_alarm->state() != core::AlarmState::Idle) {
        // Тревога уже идёт. Вторая поверх первой сбила бы оператора; поэтому
        // новая только дописывается в журнал и в строку состояния.
        statusBar()->showMessage(
            QStringLiteral("Ещё одно опасное положение: %1 (%2)").arg(text, zone), 8000);
        return;
    }

    core::AlarmEvent event;
    event.panelId = report.panelId;
    event.zoneLabel = zone;
    event.circumstance = circumstanceFor(report);
    event.details = report.reasons.join(QStringLiteral("; "))
                    + QStringLiteral(". ") + report.advice();
    // Короткая фраза для телефона — details несёт измерения для звонка
    // диспетчеру (buildCallBrief), а спасателю с телефоном в руке нужен
    // голый факт. См. AlarmEvent::phoneSummary.
    event.phoneSummary = core::situationAlert(report.situation);
    event.origin = core::AlarmOrigin::Automatic;
    event.raisedAt = QDateTime::currentDateTime();
    event.peopleInZone = report.panelId >= 0 ? m_alarm->peopleOnPanel(report.panelId)
                                             : m_alarm->peopleInZone();
    m_alarm->raiseAlarm(event);
}

void MainWindow::onAttentionDetected(const core::DangerReport &report)
{
    const QString zone = zoneName(report.panelId);
    const QString text = report.describe();

    m_log->write(core::EventLog::Kind::Attention,
                 QStringLiteral("%1 — %2").arg(zone, text), report.panelId);

    notifyPhones(QStringLiteral("attention"), report.panelId, zone,
                 core::situationText(report.situation),
                 core::situationAlert(report.situation), 1);

    if (!m_settings.reactionsEnabled)
        return;   // отклики выключены оператором — панель подсвечена, и хватит

    if (m_alarm->state() != core::AlarmState::Idle)
        return;   // во время тревоги мелкие замечания только мешают

    showNotice(QStringLiteral("Панель «%1»: %2. %3")
                   .arg(zone, text, report.advice()), false);
    if (m_settings.soundEnabled)
        m_chime->playAttention();
}

// ------------------------------------------------------------------ тревога

void MainWindow::onAlarmRequested(int panelId)
{
    if (m_alarm->state() != core::AlarmState::Idle) {
        statusBar()->showMessage(
            QStringLiteral("Тревога уже объявлена — сначала обработайте её"), 4000);
        return;
    }

    const QString zone = panelId >= 0 ? zoneName(panelId)
                                      : QStringLiteral("вся зона бассейна");

    // Спрашиваем, что случилось. Это не задержка: без ответа на этот вопрос
    // звонок в скорую всё равно не начнётся, а так он готов заранее.
    AlarmDialog dialog(zone, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    core::AlarmEvent event;
    event.panelId = panelId;
    event.zoneLabel = zone;
    event.circumstance = dialog.circumstance();
    event.origin = core::AlarmOrigin::Manual;
    event.raisedAt = QDateTime::currentDateTime();
    event.peopleInZone = panelId >= 0 ? m_alarm->peopleOnPanel(panelId)
                                      : m_alarm->peopleInZone();

    m_alarm->raiseAlarm(event);
}

void MainWindow::onAlarmStateChanged(core::AlarmState state)
{
    const core::AlarmEvent &event = m_alarm->currentEvent();
    m_card->applyState(state, event, m_site);

    const bool active = state != core::AlarmState::Idle;

    // Телефон должен узнать о снятии тревоги немедленно, а не при следующем
    // событии. Пока этого не было, уведомление на телефоне висело красным уже
    // после того, как на посту всё погасили.
    if (m_server) {
        const QString name = state == core::AlarmState::Idle ? QStringLiteral("idle")
                           : state == core::AlarmState::Raised ? QStringLiteral("raised")
                           : state == core::AlarmState::Acknowledged
                                 ? QStringLiteral("acknowledged")
                                 : QStringLiteral("confirmed");
        m_server->publishAlarmState(name, event.panelId);
    }

    // Красной рамкой обводится та панель, с которой объявлена тревога. Если
    // тревога по всей зоне — все панели сразу.
    for (CameraPanel *panel : m_panels) {
        const bool involved = active
                              && (event.panelId < 0 || event.panelId == panel->panelId());
        panel->setAlarmActive(involved);
    }

    m_alarmButton->setEnabled(!active);

    if (state == core::AlarmState::Raised) {
        // ВСЕ КАНАЛЫ СРАЗУ. Тревога, которую оператор не заметил, ничем не
        // лучше её отсутствия — а заметить один короткий звук, отвернувшись или
        // разговаривая, невозможно.
        m_chime->startAlarm();
        m_alarmSoundSince = QDateTime::currentDateTime();
        if (isHidden() && m_settings.popupLevel > 0)
            showFromTray();
        demandAttention();
        if (m_settings.blinkOnAlarm) {
            m_blinkBright = true;
            m_blink->start();
        }
        hideNotice();   // уведомление о людях сейчас только мешает

        m_log->write(core::EventLog::Kind::Alarm,
                     QStringLiteral("ТРЕВОГА объявлена (%1): %2%3")
                         .arg(event.origin == core::AlarmOrigin::Automatic
                                  ? QStringLiteral("системой")
                                  : QStringLiteral("оператором"),
                              core::circumstanceText(event.circumstance),
                              event.details.isEmpty()
                                  ? QString()
                                  : QStringLiteral(" — ") + event.details),
                     event.panelId);
    } else {
        // Сирена смолкает по нажатию «ПРИНЯЛ», а не по времени: оператор
        // обязан подтвердить, что увидел.
        m_chime->stopAlarm();
        m_alarmSoundSince = QDateTime();
        m_blink->stop();
        m_blinkBright = true;
        for (CameraPanel *panel : m_panels)
            panel->setBlinkPhase(true);
        m_card->setBlinkPhase(true);
    }

    m_stateLabel->setText(QStringLiteral("Состояние: %1").arg(core::alarmStateText(state)));
    m_stateLabel->setStyleSheet(
        active ? QStringLiteral("color: #ff6b6b; font-weight: 600;")
               : QStringLiteral("color: #939fb0;"));
}

void MainWindow::demandAttention()
{
    if (m_settings.popupLevel <= 0)
        return;

    if (m_settings.raiseWindowOnAlarm) {
        // Окно могло быть свёрнуто или закрыто другим: разворачиваем и
        // поднимаем. Фокус не отбираем силой — Windows этого и не позволит, —
        // но окно становится видимым.
        if (isMinimized())
            showNormal();
        raise();
        activateWindow();
    }

    if (m_settings.flashTaskbarOnAlarm) {
        // Ноль означает «мигать, пока окно не станет активным». Если оператор
        // ушёл в другую программу, значок в панели задач будет мигать всё это
        // время.
        QApplication::alert(this, 0);
    }
}

void MainWindow::onBlinkTick()
{
    m_blinkBright = !m_blinkBright;
    for (CameraPanel *panel : m_panels)
        panel->setBlinkPhase(m_blinkBright);
    m_card->setBlinkPhase(m_blinkBright);
}

// ----------------------------------------------------------------- сигнал 1

void MainWindow::onPresenceChanged(int panelId, int peopleCount)
{
    // Саму панель здесь трогать не нужно: рамки и счётчик она получила разом,
    // из panelAnalysed. Обновляем только общий показатель в шапке.
    Q_UNUSED(panelId)
    Q_UNUSED(peopleCount)
    updatePresenceIndicator();
}

void MainWindow::onZoneOccupancyChanged(bool occupied, int peopleTotal)
{
    // ЭТО И ЕСТЬ СИГНАЛ 1. Он подаётся только при переходе «пусто → есть
    // люди»: пока люди в зоне, повторять нечего, а звук на каждое изменение
    // счётчика превратился бы в трескотню, которую оператор отключит первой.
    //
    // Обратный переход отмечается тихо: то, что зона опустела, — не новость,
    // требующая внимания.
    // Журнал ведётся всегда, даже когда отклики выключены: разбирать смену
    // задним числом надо в любом случае, а журнал никому не мешает.
    m_log->write(core::EventLog::Kind::Presence,
                 occupied ? QStringLiteral("В зоне появились люди: %1").arg(peopleTotal)
                          : QStringLiteral("Зона опустела"));

    notifyPhones(occupied ? QStringLiteral("presence") : QStringLiteral("clear"),
                 -1, QStringLiteral("зона бассейна"),
                 occupied ? QStringLiteral("В зоне люди: %1").arg(peopleTotal)
                          : QStringLiteral("Зона пуста"),
                 QString(), occupied ? 0 : 0);

    const bool react = m_settings.reactionsEnabled && m_settings.presenceReaction;
    if (react && occupied) {
        if (m_settings.presenceSound)
            m_chime->playPresence();
        showNotice(QStringLiteral(
                       "В зоне бассейна люди: %1. Взяты на сопровождение. "
                       "Это не тревога.").arg(peopleTotal),
                   false);
    } else if (react) {
        showNotice(QStringLiteral("Зона бассейна пуста."), false);
    }
    updatePresenceIndicator();
}

void MainWindow::updatePresenceIndicator()
{
    // Выключенные отклики обязаны быть видны постоянно. Иначе оператор решит,
    // что система следит, а она молчит по его же вчерашнему нажатию.
    if (!m_settings.reactionsEnabled) {
        m_presenceLabel->setText(QStringLiteral("⚠ ОТКЛИКИ ВЫКЛЮЧЕНЫ"));
        m_presenceLabel->setStyleSheet(
            QStringLiteral("color: #ffd23f; font-weight: 700;"));
        return;
    }

    const int total = m_alarm->peopleInZone();
    if (total > 0) {
        m_presenceLabel->setText(QStringLiteral("● В зоне людей: %1").arg(total));
        m_presenceLabel->setStyleSheet(QStringLiteral("color: #35c759; font-weight: 600;"));
    } else {
        m_presenceLabel->setText(QStringLiteral("○ Зона пуста"));
        m_presenceLabel->setStyleSheet(QStringLiteral("color: #939fb0;"));
    }
}

// --------------------------------------------------------------- уведомления

void MainWindow::showNotice(const QString &text, bool important)
{
    m_notice->setText(text);
    m_notice->setStyleSheet(
        important
            ? QStringLiteral("background-color: #3d3216; border: 1px solid #ffd23f;"
                             "border-radius: 6px; padding: 8px 12px; color: #ffe9a8;")
            : QStringLiteral("background-color: #16301c; border: 1px solid #35c759;"
                             "border-radius: 6px; padding: 8px 12px; color: #b8f0c6;"));
    m_notice->show();

    // Важное сообщение (не задан адрес, не работает распознавание) висит, пока
    // его не сменит другое: это неисправность, а не событие.
    m_noticeUntil = important ? QDateTime()
                              : QDateTime::currentDateTime().addSecs(kNoticeSeconds);
}

void MainWindow::hideNotice()
{
    m_notice->hide();
    m_noticeUntil = QDateTime();
}

// ------------------------------------------------------------------ окна

void MainWindow::openSettings()
{
    SettingsDialog dialog(m_site, m_settings, this);

    // Состояние обучения знает вычислительный поток — окно только показывает.
    if (m_analysis)
        dialog.setLearningState(m_analysis->learningStatus());

    connect(&dialog, &SettingsDialog::learningResetRequested, this, [this] {
        if (!m_analysis)
            return;
        QMetaObject::invokeMethod(m_analysis, "resetLearning", Qt::QueuedConnection);
        m_log->write(core::EventLog::Kind::Operator,
                     QStringLiteral("Обучение постоянному окружению начато заново"));
    });

    connect(&dialog, &SettingsDialog::learningImportRequested, this,
            [this](const QString &path) {
                // Готовый файл кладём на место своего и просим поток его
                // перечитать: так не нужно ни отдельного пути, ни особого
                // режима работы.
                const QString target = backgroundModelPath();
                QDir().mkpath(QFileInfo(target).absolutePath());
                QFile::remove(target);
                const bool copied = QFile::copy(path, target);

                m_log->write(
                    core::EventLog::Kind::Operator,
                    copied ? QStringLiteral("Загружено готовое обучение: %1").arg(path)
                           : QStringLiteral("Не удалось прочитать файл обучения: %1")
                                 .arg(path));

                if (copied && m_analysis) {
                    QMetaObject::invokeMethod(
                        m_analysis, "setSelfLearning", Qt::QueuedConnection,
                        Q_ARG(bool, m_settings.selfLearning),
                        Q_ARG(QString, target));
                }
            });
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_site = dialog.site();
    m_settings = dialog.settings();

    const bool siteSaved = m_site.save(core::SiteInfo::defaultPath());
    const bool settingsSaved = m_settings.save(core::Settings::defaultPath());

    applySettingsToParts();

    // Карточка тревоги может быть открыта прямо сейчас — она должна показать
    // новые телефоны немедленно, а не после перезапуска.
    if (m_alarm->state() != core::AlarmState::Idle)
        m_card->applyState(m_alarm->state(), m_alarm->currentEvent(), m_site);

    if (!siteSaved || !settingsSaved) {
        QMessageBox::warning(this, QStringLiteral("Настройки не сохранены"),
            QStringLiteral("Не удалось записать файлы настроек рядом с программой.\n"
                           "Проверьте права на папку — иначе после перезапуска "
                           "правки пропадут."));
    }

    m_log->write(core::EventLog::Kind::Operator,
                 QStringLiteral("Настройки изменены"));

    if (m_site.isAddressReady())
        showNotice(QStringLiteral("Настройки сохранены. Адрес объекта задан."), false);
    else
        showNotice(QStringLiteral("Настройки сохранены, но адрес объекта всё ещё "
                                  "не заполнен."), true);

    // Модель и порог поиска применяются перезагрузкой модели прямо сейчас —
    // без перезапуска программы. Пока модель грузится (сотни миллисекунд),
    // разбор молчит, но видео продолжает идти.
    if (m_analysis) {
        statusBar()->showMessage(QStringLiteral("Перезагрузка модели распознавания…"),
                                 5000);
        QMetaObject::invokeMethod(
            m_analysis, "start", Qt::QueuedConnection,
            Q_ARG(QString, core::PersonDetector::modelPath(m_settings.detectorModel)),
            Q_ARG(QString, core::PoseEstimator::defaultModelPath()));
        for (CameraPanel *panel : m_panels)
            panel->clearAnalysis();
    }
}

void MainWindow::buildTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(windowIcon(), this);
    m_tray->setToolTip(QStringLiteral("Наблюдение за бассейном"));

    auto *menu = new QMenu(this);
    connect(menu->addAction(QStringLiteral("Показать окно")), &QAction::triggered,
            this, &MainWindow::showFromTray);
    connect(menu->addAction(QStringLiteral("Настройки…")), &QAction::triggered,
            this, &MainWindow::openSettings);
    connect(menu->addAction(QStringLiteral("Журнал событий…")), &QAction::triggered,
            this, &MainWindow::openEventLog);
    menu->addSeparator();
    connect(menu->addAction(QStringLiteral("Выйти из программы")), &QAction::triggered,
            this, [this] {
                m_quitting = true;
                close();
            });

    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                onTrayActivated(int(reason));
            });
    m_tray->show();
}

void MainWindow::showFromTray()
{
    showNormal();
    raise();
    activateWindow();
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType == "windows_generic_MSG") {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message == core::singleInstance::restoreMessage()) {
            // Оператор нажал ярлык второй раз — программа уже работает, но
            // была свёрнута в трей (или просто за другими окнами). Ровно то
            // же самое, что пункт «Показать окно» в меню значка у часов.
            showFromTray();
            if (result)
                *result = 0;
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::onTrayActivated(int reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
        showFromTray();
}

void MainWindow::openEventLog()
{
    if (m_logDialog) {
        m_logDialog->raise();
        m_logDialog->activateWindow();
        return;
    }

    // Окно не модальное: наблюдение при открытом журнале не прерывается.
    m_logDialog = new EventLogDialog(m_log, this);
    m_logDialog->setAttribute(Qt::WA_DeleteOnClose);
    m_logDialog->show();
}

// ---------------------------------------------------------------- показания

void MainWindow::refreshReadouts()
{
    for (CameraPanel *panel : m_panels)
        panel->refreshReadouts();

    m_card->tick();

    if (m_noticeUntil.isValid() && QDateTime::currentDateTime() > m_noticeUntil)
        hideNotice();

    if (m_alarm->state() == core::AlarmState::Raised) {
        const qint64 elapsed = m_alarmSoundSince.isValid()
                                   ? m_alarmSoundSince.secsTo(QDateTime::currentDateTime())
                                   : 0;

        // Сирена должна звучать. Если звук почему-то оборвался (устройство
        // перехватила другая программа, сменилось устройство вывода) —
        // запускаем заново: молчащая тревога это не тревога.
        if (!m_chime->isAlarmSounding() && m_settings.soundEnabled
            && elapsed >= m_settings.alarmRepeatSeconds) {
            m_chime->startAlarm();
        }

        // Реакции нет — усиливаем: сирена резче и громче, окно снова наверх,
        // значок снова мигает.
        if (m_settings.escalateAfterSeconds > 0
            && elapsed >= m_settings.escalateAfterSeconds) {
            m_chime->escalate();
            demandAttention();
        }
    }

    if (m_analysis && m_analysis->isEnabled()) {
        const int detectMs = m_analysis->averageDetectMs();
        if (detectMs > 0) {
            QString text = m_analysis->isPoseReady()
                               ? QStringLiteral("Распознавание и позы: %1 мс").arg(detectMs)
                               : QStringLiteral("Распознавание: %1 мс").arg(detectMs);

            // Сколько раз в секунду обновляется рамка. Именно это число
            // оператор видит глазами как «рамка успевает» или «отстаёт».
            const int periodMs = m_analysis->averagePeriodMs();
            if (periodMs > 0) {
                text += QStringLiteral("  ·  рамка %1 раз/с")
                            .arg(1000.0 / periodMs, 0, 'f', 1);
            }

            if (m_trackSeconds > 0.0) {
                text += QStringLiteral("  ·  сопровождение %1 с")
                            .arg(m_trackSeconds, 0, 'f', 1);
            }
            m_detectorLabel->setText(text);
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Крестик сворачивает программу в значок у часов, а не выключает её.
    //
    // ЗАЧЕМ ТАК. Наблюдение должно идти всю смену. Оператор, закрывший окно
    // «чтобы не мешало», выключил бы вместе с ним и распознавание, и тревогу —
    // и никто бы этого не заметил до происшествия. Выйти совсем можно из меню
    // значка: это осознанное действие.
    if (!m_quitting && m_settings.minimizeToTray && m_tray && m_tray->isVisible()) {
        hide();
        m_tray->showMessage(
            QStringLiteral("Наблюдение продолжается"),
            QStringLiteral("Программа свёрнута в значок у часов. Чтобы выйти "
                           "совсем, нажмите на значок правой кнопкой."),
            QSystemTrayIcon::Information, 5000);
        event->ignore();
        return;
    }

    if (m_alarm->state() != core::AlarmState::Idle) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Тревога не обработана"),
            QStringLiteral("Сейчас идёт необработанная тревога. Всё равно закрыть программу?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    m_chime->stopAlarm();
    m_log->write(core::EventLog::Kind::Info, QStringLiteral("Пост наблюдения закрыт"));

    // Источники освобождаются до закрытия окна, чтобы камеры не остались
    // занятыми: следующий запуск программы должен их увидеть.
    for (CameraPanel *panel : m_panels)
        panel->setSource(core::SourceDescriptor::none());

    event->accept();
}

} // namespace ui
