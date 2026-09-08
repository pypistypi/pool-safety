#pragma once

// ---------------------------------------------------------------------------
//  Главное окно поста наблюдения.
//
//  Связывает сигналами независимые части, не давая им знать друг о друге:
//
//      реестр источников    →  четыре панели сетки
//      панели               →  вычислительный поток (кадры на разбор)
//      вычислительный поток →  обработка сигналов (люди, опасные положения)
//      оператор / алгоритм  →  тревога, звонок, разбор происшествия
//      всё перечисленное    →  журнал событий
//
//  РАЗДЕЛЕНИЕ РАБОТЫ ПО ПОТОКАМ. В главном потоке остаётся только рисование
//  окна — так требует Qt: обращаться к виджетам из других потоков запрещено, и
//  нарушение этого правила даёт именно те неповторимые зависания, от которых
//  мы уходим. Всё тяжёлое вынесено за его пределы: декодирование видео ведут
//  рабочие потоки Qt, распознавание человека и разбор поз — отдельный поток
//  (AnalysisWorker). Один разбор занимает около 190 мс — выполняйся он здесь,
//  окно замирало бы каждые несколько кадров.
//
//  ДВА ТАЙМЕРА НА ВСЁ ОКНО, И БОЛЬШЕ НИ ОДНОГО. Секундный обновляет часы
//  панелей, счётчик времени тревоги и снимает уведомления. Четвертьсекундный
//  работает ТОЛЬКО во время непринятой тревоги и гоняет мигание. Свой таймер в
//  каждой панели означал бы четыре пробуждения вместо одного при той же пользе.
//
//  ТРЕВОГА ОБЯЗАНА БЫТЬ ЗАМЕЧЕННОЙ. Поэтому она бьёт по всем каналам сразу:
//  непрерывная сирена, мигающая рамка, окно поверх остальных, мигание значка в
//  панели задач. Замолкает всё это только по нажатию «ПРИНЯЛ» — не по времени.
// ---------------------------------------------------------------------------

#include "core/AlarmTypes.h"
#include "core/EventServer.h"
#include "core/AnalysisTypes.h"
#include "core/Settings.h"
#include "core/SiteInfo.h"
#include "core/SourceDescriptor.h"

#include <QMainWindow>
#include <QVector>
#include <QThread>
#include <QDateTime>
#include <QPointer>

class QLabel;
class QPushButton;
class QTimer;
class QAction;
class QSystemTrayIcon;

namespace core {
class SourceHub;
class DeviceRegistry;
class AlarmController;
class AnalysisWorker;
class EventLog;
class EventServer;
}

namespace ui {

class CameraPanel;
class AlarmCard;
class Chime;
class EventLogDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

#ifdef Q_OS_WIN
    /// Ловит сообщение от второй, только что запущенной копии программы:
    /// «разбуди своё окно». См. core/SingleInstance.h — там же объяснено,
    /// какую тупиковую ситуацию это устраняет.
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private slots:
    void onAlarmRequested(int panelId);
    void onAlarmStateChanged(core::AlarmState state);

    /// Разослать объявленную тревогу телефонам.
    ///
    /// ОДНО МЕСТО НА ВСЕ ТРЕВОГИ. Раньше телефонам сообщал только разбор
    /// кадра, и тревога, объявленная оператором вручную, до них не доходила
    /// вовсе — то есть ровно тот случай, когда человек уже увидел беду.
    void onAlarmRaised(const core::AlarmEvent &event);

    /// Решение по тревоге, принятое с телефона.
    void onPhoneCommand(core::EventServer::Command command);
    void onPresenceChanged(int panelId, int peopleCount);
    void onZoneOccupancyChanged(bool occupied, int peopleTotal);
    void onPanelAnalysed(const core::PanelAnalysis &result);
    void onDangerDetected(const core::DangerReport &report);
    void onAttentionDetected(const core::DangerReport &report);
    void onDetectorReady(bool ready, const QString &message);
    void onPoseReady(bool ready, const QString &message);
    void refreshReadouts();
    void onBlinkTick();
    void openSettings();
    void openEventLog();
    void showFromTray();
    void onTrayActivated(int reason);

private:
    void buildUi();
    void buildMenu();
    void applyCommandLine();
    void buildPanels(QWidget *gridHost);
    void wireAlarm();
    void startAnalysisThread();
    void applyStartupGeometry();
    void applySettingsToParts();
    void updatePresenceIndicator();
    void showNotice(const QString &text, bool important);
    void hideNotice();
    void demandAttention();
    void buildTray();
    /// Где лежит карта постоянного окружения этой установки.
    static QString backgroundModelPath();

    void publishPanels();

    /// Заказать рассылку сведений о панелях телефонам.
    ///
    /// НЕ НАПРЯМУЮ, А ЧЕРЕЗ ЗАДЕРЖКУ. Число людей на панели меняется по
    /// нескольку раз в секунду, и слать при каждом изменении полный список
    /// панелей значит забивать канал ради данных, которые устареют раньше,
    /// чем их прочтут.
    void schedulePanelsPublish();
    void notifyPhones(const QString &kind, int panelId, const QString &zone,
                      const QString &title, const QString &details, int level);
    QString zoneName(int panelId) const;

    core::SourceHub *m_hub = nullptr;
    core::DeviceRegistry *m_devices = nullptr;
    core::AlarmController *m_alarm = nullptr;
    core::EventLog *m_log = nullptr;
    core::EventServer *m_server = nullptr;
    core::SiteInfo m_site;
    core::Settings m_settings;

    QThread m_analysisThread;
    core::AnalysisWorker *m_analysis = nullptr;

    QVector<CameraPanel *> m_panels;
    AlarmCard *m_card = nullptr;
    Chime *m_chime = nullptr;
    QPointer<EventLogDialog> m_logDialog;

    QLabel *m_presenceLabel = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_detectorLabel = nullptr;
    QLabel *m_notice = nullptr;
    QPushButton *m_alarmButton = nullptr;
    QAction *m_skeletonAction = nullptr;
    QTimer *m_tick = nullptr;
    QTimer *m_blink = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    bool m_quitting = false;

    QDateTime m_noticeUntil;

    /// Средняя длительность сопровождения человека, секунд — по последней
    /// разобранной панели. Показывается в строке состояния.
    double m_trackSeconds = 0.0;

    /// Последнее, что телефоны знают о панелях. Хранится, чтобы отличать
    /// настоящее изменение от повторной присылки того же самого.
    QVector<int> m_panelPeople;
    QVector<int> m_panelLevels;
    QTimer *m_panelsPublish = nullptr;
    bool m_blinkBright = true;

    /// Когда сирена включалась в последний раз. По ней же проверяется, что
    /// звук не оборвался: молчащая тревога — не тревога.
    QDateTime m_alarmSoundSince;
};

} // namespace ui
