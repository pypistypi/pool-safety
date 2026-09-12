#pragma once

// ---------------------------------------------------------------------------
//  Вычислительный поток: распознавание человека и опасных положений.
//
//  ЗДЕСЬ И ТОЛЬКО ЗДЕСЬ идёт разбор кадров. Поток отдельный, и это не
//  формальность: поиск людей занимает около 90 мс, оценка позы — ещё около
//  100, а окно перерисовывается каждые 40. Выполняйся распознавание в потоке
//  интерфейса — оператор видел бы рывки вместо видео.
//
//  ЧЕТЫРЕ ПРАВИЛА, УДЕРЖИВАЮЩИЕ НАГРУЗКУ:
//
//  1. ОБРАБАТЫВАЕТСЯ ТОЛЬКО ПОСЛЕДНИЙ КАДР. У каждой панели одна ячейка:
//     пришёл новый кадр — прежний, необработанный, выбрасывается. Иначе
//     очередь растёт без конца, память кончается, а оператор видит всё более
//     старую картинку. Потеря промежуточных кадров для наблюдения безвредна,
//     отставание от реальности — нет.
//  2. РАЗБОР НЕ ЧАЩЕ ЗАДАННОГО ТЕМПА. Камера отдаёт 30 кадров в секунду;
//     разбирать их все незачем — беда длится секунды, а не тридцатые доли.
//  3. ПАНЕЛИ РАЗБИРАЮТСЯ ПО ОЧЕРЕДИ, одним потоком. Четыре параллельных
//     прогона заняли бы все ядра и отняли бы их у отрисовки — ровно та
//     ошибка, что была в прежней версии программы.
//  4. ПОЗА СЧИТАЕТСЯ ТОЛЬКО ТАМ, ГДЕ НАШЁЛСЯ ЧЕЛОВЕК. Пустой бассейн не
//     стоит ничего.
//
//  СГЛАЖИВАНИЕ. Модель изредка пропускает человека на отдельном кадре. Без
//  сглаживания счётчик прыгал бы, рамка мигала, а звуковое уведомление
//  тарахтело. Поэтому появление подтверждается двумя разборами подряд, а
//  пропажа выдерживается полутора секундами.
// ---------------------------------------------------------------------------

#include "core/AnalysisTypes.h"
#include "core/BackgroundModel.h"
#include "core/PersonDetector.h"
#include "core/PersonTracker.h"
#include "core/PoseEstimator.h"
#include "core/SituationRules.h"

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QVideoFrame>
#include <QAtomicInt>
#include <QElapsedTimer>

namespace core {

class AnalysisWorker : public QObject
{
    Q_OBJECT

public:
    explicit AnalysisWorker(QObject *parent = nullptr);
    ~AnalysisWorker() override;

    /// Принять кадр на разбор. Вызывается из потока интерфейса и возвращает
    /// управление немедленно.
    void submitFrame(int panelId, const QVideoFrame &frame);

    bool isEnabled() const { return m_enabled.loadAcquire() != 0; }
    bool isPoseReady() const { return m_poseReady.loadAcquire() != 0; }

    /// Сколько кадров отброшено как устаревшие — показатель для строки
    /// состояния: большое число означает, что разбор не поспевает за потоком.
    int droppedFrames() const { return m_dropped.loadAcquire(); }

    /// Среднее время одного разбора, миллисекунды.
    int averageDetectMs() const { return m_averageMs.loadAcquire(); }

    /// Сколько в среднем миллисекунд проходит между разборами кадра.
    ///
    /// Это НЕ то же самое, что время разбора: между разборами есть ещё пауза
    /// заданного темпа, а панелей может быть несколько. Оператору важно
    /// именно это число — от него зависит, насколько рамка отстаёт от
    /// человека.
    int averagePeriodMs() const { return m_periodMs.loadAcquire(); }

    /// Название загруженной модели поиска людей — для строки состояния.
    /// Читается из потока интерфейса после сигнала detectorReady, когда
    /// загрузка уже завершена.
    QString detectorName() const;

public slots:
    /// Загрузить обе модели и включить распознавание. Выполняется в своём
    /// потоке: открытие моделей занимает сотни миллисекунд, и делать это в
    /// потоке интерфейса значило бы придержать показ окна.
    void start(const QString &detectorPath, const QString &posePath);

    /// Забыть накопленное по панели: сменили источник, и прежние сведения о
    /// людях к новой картинке отношения не имеют.
    void forgetPanel(int panelId);

    /// Подстроить пороги правил, не пересобирая программу.
    void applyThresholds(const core::Thresholds &thresholds);

    /// Темп разбора, миллисекунды между разборами ОДНОЙ панели.
    void setDetectIntervalMs(int milliseconds);

    /// Включить или выключить обучение постоянному окружению.
    Q_INVOKABLE void setSelfLearning(bool enabled, const QString &modelPath);

    /// Состояние обучения понятными словами — для окна настроек.
    QString learningStatus() const;

    /// Забыть накопленное и начать заново. Нужно после переноса камер.
    Q_INVOKABLE void resetLearning();

    /// Порог уверенности поиска людей. Применяется при загрузке модели,
    /// поэтому задавать его нужно ДО start(): менять порог на ходу означало бы
    /// пересоздавать сессию — сотни миллисекунд посреди наблюдения.
    ///
    /// По умолчанию 0,35 против 0,45 в первой версии: при 0,45 человек в
    /// глубине помещения, частично закрытый мебелью, не находился вовсе.
    /// Пропущенный человек — пропущенная беда, а лишняя найденная рамка всего
    /// лишь получит скелет и спокойный разбор.
    void setSearchConfidence(double confidence);

    /// Считать ли позу и опасные положения. Выключение оставляет только
    /// присутствие — полезно на слабой машине.
    void setPoseEnabled(bool enabled);

    /// Отметить, что в эту панель попадает вода. Пока не отмечено, правила,
    /// завязанные на воду, не применяются — см. SituationAnalyzer::analyse().
    void setPanelHasWater(int panelId, bool hasWater);

signals:
    /// Итог одного разбора: рамки, счётчик и уровень опасности — одним
    /// состоянием кадра.
    void panelAnalysed(const core::PanelAnalysis &result);

    /// СИГНАЛ 1 — число людей на панели изменилось.
    void presenceDetected(int panelId, int peopleCount);

    /// СИГНАЛ 2 — найдено опасное положение уровня «ТРЕВОГА».
    void dangerDetected(const core::DangerReport &report);

    /// Признаки есть, но до тревоги не дотягивают. Оператору показывается
    /// строкой, звуком не сопровождается.
    void attentionDetected(const core::DangerReport &report);

    /// Распознавание готово к работе либо не смогло запуститься.
    void detectorReady(bool ready, const QString &message);

    /// Оценка позы готова или недоступна. Отдельно от detectorReady: без позы
    /// программа продолжает считать людей, и об этом надо сказать честно.
    void poseReady(bool ready, const QString &message);

private slots:
    void processPending();

private:
    struct PanelState {
        qint64 lastRunMs = -100000;   ///< когда панель разбиралась в последний раз
        qint64 lastSeenMs = -100000;  ///< когда на ней последний раз видели людей
        int consecutiveHits = 0;      ///< сколько разборов подряд нашли людей
        int reportedCount = 0;        ///< что уже сообщено наружу
        QSize frameSize;
        QVector<QRect> boxes;
        PersonTracker tracker;

        /// Когда последний раз считались позы, и что тогда получилось.
        /// Между разборами поз этим заполняются рамки, чтобы вердикт и цвет
        /// держались, а не мигали.
        qint64 lastPoseMs = -100000;
        QVector<PersonView> lastPeople;
        Level lastWorstLevel = Level::Normal;
        QString lastWorstLabel;
        QStringList lastWorstReasons;
        double lastTrackSeconds = 0.0;

        /// Когда по паре «дорожка + положение» последний раз поднималась
        /// тревога. Без этого одна и та же беда сообщалась бы каждые полсекунды
        /// и превратилась бы в шум.
        QHash<QString, qint64> announced;
    };

    void analysePanel(int panelId, const QVideoFrame &frame, qint64 now);
    void runSituations(int panelId, PanelState &state, PanelAnalysis &result,
                       const QImage &image, const QVector<QRect> &boxes, qint64 now);

    /// Карта постоянного окружения. Одна на все панели: камеры смотрят на
    /// один и тот же бассейн, и разделять карты по панелям значило бы учить
    /// четыре раза то же самое вчетверо дольше.
    ///
    /// НЕ ПОДАВЛЯЕТ ЛЮДЕЙ. Только повышает требовательность к слабым откликам
    /// там, где неделями не менялось ничего.
    BackgroundModel m_background;
    QString m_backgroundPath;
    QAtomicInt m_learning = 0;
    int m_sinceSave = 0;

    PersonDetector m_detector;
    PoseEstimator m_pose;
    SituationAnalyzer m_analyzer;

    QMutex m_mutex;
    QHash<int, QVideoFrame> m_pending;   ///< по одной ячейке на панель
    QHash<int, PanelState> m_panels;
    QHash<int, bool> m_water;            ///< какие панели смотрят на воду

    QAtomicInt m_wakeScheduled = 0;
    QAtomicInt m_enabled = 0;
    QAtomicInt m_poseReady = 0;
    QAtomicInt m_poseWanted = 1;
    QAtomicInt m_dropped = 0;
    QAtomicInt m_averageMs = 0;
    QAtomicInt m_periodMs = 0;
    qint64 m_lastAnalysisMs = -1;

    QElapsedTimer m_clock;

    // Настройки темпа и сглаживания. Поля, а не константы по месту, чтобы их
    // можно было подобрать под объект, не переписывая логику.
    qint64 m_detectIntervalMs = 120;

    /// Как часто считаются позы.
    ///
    /// РЕЖЕ, ЧЕМ ИЩУТСЯ ЛЮДИ, И ЭТО НЕ ЭКОНОМИЯ НА БЕЗОПАСНОСТИ. На кадре с
    /// шестью людьми поиск занимает 87 мс, а позы — 117 мс: позы дороже всего
    /// остального вместе взятого. При этом ни одно правило не смотрит на
    /// отдельный кадр: самый короткий порог — две секунды. Трижды в секунду
    /// разбирать положение тела достаточно с большим запасом, а рамка при
    /// этом успевает за человеком.
    qint64 m_poseIntervalMs = 300;
    double m_searchConfidence = 0.25;
    qint64 m_presenceHoldMs = 1500;
    int m_minHits = 2;

    /// Как часто одно и то же опасное положение сообщается повторно.
    qint64 m_repeatDangerMs = 60000;
};

} // namespace core
