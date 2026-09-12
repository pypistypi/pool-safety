#include "core/AnalysisWorker.h"

#include <QMutexLocker>
#include <QMetaObject>
#include <QImage>

namespace core {

AnalysisWorker::AnalysisWorker(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
}

AnalysisWorker::~AnalysisWorker() = default;

void AnalysisWorker::start(const QString &detectorPath, const QString &posePath)
{
    // Метод вызывается и повторно — когда оператор сменил модель в настройках.
    // Накопленное по панелям к новой модели отношения не имеет: рамки у неё
    // другие, и дорожки надо начинать заново.
    {
        QMutexLocker locker(&m_mutex);
        m_pending.clear();
    }
    m_panels.clear();
    m_enabled.storeRelease(0);
    m_poseReady.storeRelease(0);

    PersonDetector::Config config;
    config.modelPath = detectorPath;
    // Порог ниже, чем был в первой версии (0,45): пропущенный человек — это
    // пропущенная беда, а лишняя рамка всего лишь получит спокойный разбор.
    config.confidenceThreshold = float(m_searchConfidence);

    if (!m_detector.load(config)) {
        m_enabled.storeRelease(0);
        // Программа продолжает показывать видео: наблюдать оператор сможет и
        // без распознавания. Но молчать об этом нельзя — иначе он будет
        // считать, что алгоритм работает и никого не находит.
        emit detectorReady(false, m_detector.lastError());
        return;
    }

    m_enabled.storeRelease(1);
    emit detectorReady(true, QString());

    PoseEstimator::Config poseConfig;
    poseConfig.modelPath = posePath;

    if (!m_pose.load(poseConfig)) {
        m_poseReady.storeRelease(0);
        emit poseReady(false, m_pose.lastError());
        return;
    }

    m_poseReady.storeRelease(1);
    emit poseReady(true, QString());
}

QString AnalysisWorker::detectorName() const
{
    return m_detector.modelName();
}

void AnalysisWorker::submitFrame(int panelId, const QVideoFrame &frame)
{
    if (!isEnabled() || !frame.isValid())
        return;

    {
        QMutexLocker locker(&m_mutex);
        if (m_pending.contains(panelId))
            m_dropped.fetchAndAddRelaxed(1);
        m_pending.insert(panelId, frame);
    }

    // Будим поток не на каждый кадр, а только если прошлое пробуждение уже
    // отработано. Иначе очередь событий копила бы вызовы ровно так же, как
    // копила бы кадры — беда та же, просто переехавшая этажом ниже.
    if (m_wakeScheduled.testAndSetOrdered(0, 1)) {
        QMetaObject::invokeMethod(this, &AnalysisWorker::processPending,
                                  Qt::QueuedConnection);
    }
}

void AnalysisWorker::forgetPanel(int panelId)
{
    // Слот вызывается из потока интерфейса, но Qt доставляет его через очередь
    // событий, поэтому тело выполняется в потоке разбора — там же, где живёт
    // m_panels. Отдельная защита ей поэтому не нужна; мьютексом закрыта только
    // очередь кадров, к которой обращаются оба потока.
    {
        QMutexLocker locker(&m_mutex);
        m_pending.remove(panelId);
    }
    m_panels.remove(panelId);
}

void AnalysisWorker::applyThresholds(const Thresholds &thresholds)
{
    m_analyzer.setThresholds(thresholds);
}

void AnalysisWorker::setSelfLearning(bool enabled, const QString &modelPath)
{
    m_backgroundPath = modelPath;
    if (enabled && !m_backgroundPath.isEmpty())
        m_background.load(m_backgroundPath);
    if (!enabled && !m_backgroundPath.isEmpty())
        m_background.save(m_backgroundPath);
    m_learning.storeRelease(enabled ? 1 : 0);
}

QString AnalysisWorker::learningStatus() const
{
    if (m_learning.loadAcquire() == 0)
        return QStringLiteral("выключено");
    return m_background.status();
}

void AnalysisWorker::resetLearning()
{
    m_background.reset();
    if (!m_backgroundPath.isEmpty())
        m_background.save(m_backgroundPath);
}

void AnalysisWorker::setDetectIntervalMs(int milliseconds)
{
    m_detectIntervalMs = qBound(100, milliseconds, 5000);
}

void AnalysisWorker::setSearchConfidence(double confidence)
{
    m_searchConfidence = qBound(0.15, confidence, 0.90);
}

void AnalysisWorker::setPoseEnabled(bool enabled)
{
    m_poseWanted.storeRelease(enabled ? 1 : 0);
}

void AnalysisWorker::setPanelHasWater(int panelId, bool hasWater)
{
    m_water.insert(panelId, hasWater);
}

void AnalysisWorker::processPending()
{
    QHash<int, QVideoFrame> batch;
    {
        QMutexLocker locker(&m_mutex);
        batch.swap(m_pending);
    }
    m_wakeScheduled.storeRelease(0);

    if (!isEnabled())
        return;

    const qint64 now = m_clock.elapsed();

    for (auto it = batch.constBegin(); it != batch.constEnd(); ++it) {
        const int panelId = it.key();

        // Темп разбора. Проверяется до всякой работы с кадром: самая дешёвая
        // обработка — та, которой не было.
        const qint64 lastRun = m_panels.value(panelId).lastRunMs;
        if (now - lastRun < m_detectIntervalMs)
            continue;

        analysePanel(panelId, it.value(), now);
    }
}

void AnalysisWorker::analysePanel(int panelId, const QVideoFrame &frame, qint64 now)
{
    // Кадр разворачивается в картинку здесь, в своём потоке. Панель делает то
    // же самое для показа, и на первый взгляд это лишняя работа — но перенос
    // преобразования в поток интерфейса вернул бы туда десятки миллисекунд на
    // каждый разбор, а именно этого мы и избегаем.
    const QImage image = frame.toImage();
    if (image.isNull())
        return;

    // Насколько часто разбор случается на самом деле. Считается по факту, а не
    // выводится из настроек: панелей может быть несколько, и каждая отнимает
    // время у остальных.
    if (m_lastAnalysisMs >= 0) {
        const qint64 gap = now - m_lastAnalysisMs;
        const int previous = m_periodMs.loadAcquire();
        m_periodMs.storeRelease(previous == 0 ? int(gap)
                                              : int((previous * 3 + gap) / 4));
    }
    m_lastAnalysisMs = now;

    QElapsedTimer timer;
    timer.start();
    QVector<Detection> found = m_detector.detect(image);

    // --- карта постоянного окружения ---------------------------------------
    if (m_learning.loadAcquire() != 0) {
        m_background.observe(image);

        // Сохраняем изредка: карта меняется медленно, а запись на диск в
        // вычислительном потоке — это отнятые у разбора миллисекунды.
        if (++m_sinceSave >= 2000 && !m_backgroundPath.isEmpty()) {
            m_sinceSave = 0;
            m_background.save(m_backgroundPath);
        }

        if (m_background.isReady() && !found.isEmpty()) {
            // СТРОЖЕ ТАМ, ГДЕ НИКОГДА НИЧЕГО НЕ МЕНЯЕТСЯ. Слабый отклик на
            // стене или на узоре дна — почти наверняка не человек. Но порог
            // растёт ограниченно и уверенный отклик проходит всегда: человек
            // способен стоять неподвижно, и отказать ему в существовании
            // из-за того, что он встал в тихом углу, — та самая ошибка,
            // которой вся система и должна не допустить.
            QVector<Detection> kept;
            kept.reserve(found.size());
            for (const Detection &detection : std::as_const(found)) {
                const double still = m_background.stillnessAt(detection.box,
                                                             image.size());
                const float required = float(m_searchConfidence)
                                       + float(still) * 0.20f;
                if (detection.confidence >= required)
                    kept.append(detection);
            }
            found = kept;
        }
    }

    PanelState &state = m_panels[panelId];
    state.lastRunMs = now;
    state.frameSize = image.size();

    QVector<QRect> boxes;
    boxes.reserve(found.size());
    for (const Detection &detection : found)
        boxes.append(detection.box);

    int newCount = state.reportedCount;

    if (!found.isEmpty()) {
        ++state.consecutiveHits;
        state.lastSeenMs = now;
        state.boxes = boxes;

        // Появление подтверждается двумя разборами подряд: одиночный ложный
        // отклик не должен поднимать оператора звуком.
        if (state.consecutiveHits >= m_minHits)
            newCount = int(found.size());
    } else {
        state.consecutiveHits = 0;

        // Пропажа выдерживается: модель изредка теряет человека на отдельном
        // кадре, и без выдержки рамка мигала бы, а счётчик прыгал.
        if (now - state.lastSeenMs > m_presenceHoldMs) {
            newCount = 0;
            state.boxes.clear();
        }
    }

    PanelAnalysis result;
    result.panelId = panelId;
    result.frameSize = state.frameSize;
    result.peopleCount = newCount;

    // Разбор положений — только если человек найден. Пустой бассейн не должен
    // стоить ничего. И не на каждом кадре: позы дороже поиска людей, а правила
    // работают на порогах в секунды.
    const bool posesDue = (now - state.lastPoseMs) >= m_poseIntervalMs;
    const bool posesWanted = isPoseReady() && m_poseWanted.loadAcquire() != 0
                             && !state.boxes.isEmpty();

    if (posesWanted && posesDue) {
        state.lastPoseMs = now;
        runSituations(panelId, state, result, image, state.boxes, now);

        // Запоминаем итог: им заполнятся кадры до следующего разбора поз.
        if (!result.people.isEmpty()) {
            state.lastPeople = result.people;
            state.lastWorstLevel = result.worstLevel;
            state.lastWorstLabel = result.worstLabel;
            state.lastWorstReasons = result.worstReasons;
            state.lastTrackSeconds = result.averageTrackSeconds;
        }
    }

    if (result.people.isEmpty()) {
        // Позы в этот раз не считались (или не смогли) — показываем рамки, а
        // вердикт по каждому берём из последнего разбора поз. Иначе цвет
        // рамки и подпись мигали бы через кадр.
        for (const QRect &box : std::as_const(state.boxes)) {
            PersonView person;
            person.box = box;

            if (const PersonView *previous = matchPrevious(state.lastPeople, box)) {
                person.trackId = previous->trackId;
                person.level = previous->level;
                person.label = previous->label;
                person.reasons = previous->reasons;
                person.heldSeconds = previous->heldSeconds;
            }
            result.people.append(person);
        }

        if (state.lastWorstLevel != Level::Normal && !state.boxes.isEmpty()) {
            result.worstLevel = state.lastWorstLevel;
            result.worstLabel = state.lastWorstLabel;
            result.worstReasons = state.lastWorstReasons;
        }
        result.averageTrackSeconds = state.lastTrackSeconds;
    }

    // Людей не стало — прошлые вердикты больше ни к чему.
    if (state.boxes.isEmpty()) {
        state.lastPeople.clear();
        state.lastWorstLevel = Level::Normal;
        state.lastWorstLabel.clear();
        state.lastWorstReasons.clear();
    }

    const qint64 elapsed = timer.elapsed();

    // Скользящее среднее: одиночный всплеск не должен пугать оператора цифрой
    // в строке состояния.
    const int previous = m_averageMs.loadAcquire();
    m_averageMs.storeRelease(previous == 0 ? int(elapsed)
                                           : int((previous * 3 + elapsed) / 4));

    // Сперва панель — всё состояние кадра разом.
    emit panelAnalysed(result);

    // Затем — только при изменении — сообщение о людях в зоне.
    if (newCount != state.reportedCount) {
        state.reportedCount = newCount;
        emit presenceDetected(panelId, newCount);
    }
}

void AnalysisWorker::runSituations(int panelId, PanelState &state, PanelAnalysis &result,
                                   const QImage &image, const QVector<QRect> &boxes,
                                   qint64 now)
{
    const QVector<Pose> poses = m_pose.estimate(image, boxes);
    if (poses.isEmpty())
        return;

    const double seconds = double(now) / 1000.0;
    const QVector<int> updated = state.tracker.update(poses, seconds);
    const QVector<const Track *> tracks = state.tracker.tracks();

    double ageSum = 0.0;
    for (const Track *track : tracks)
        ageSum += track->age();
    result.averageTrackSeconds = tracks.isEmpty() ? 0.0 : ageSum / tracks.size();

    for (int trackId : updated) {
        const Track *track = state.tracker.track(trackId);
        if (!track)
            continue;
        const Sample *latest = track->latest();
        if (!latest)
            continue;

        PersonView person;
        person.trackId = trackId;
        person.box = latest->pose.box;

        const QVector<Verdict> verdicts =
            m_analyzer.analyse(*track, tracks, m_water.value(panelId, false));
        const Verdict worst = SituationAnalyzer::worst(verdicts);

        if (worst.level != Level::Normal) {
            person.level = worst.level;
            person.label = situationText(worst.situation);
            person.reasons = worst.reasons;
            person.heldSeconds = worst.heldSeconds;

            if (worst.level > result.worstLevel) {
                result.worstLevel = worst.level;
                result.worstLabel = person.label;
                result.worstReasons = worst.reasons;
            }

            DangerReport report;
            report.panelId = panelId;
            report.trackId = trackId;
            report.situation = worst.situation;
            report.level = worst.level;
            report.reasons = worst.reasons;
            report.heldSeconds = worst.heldSeconds;
            report.at = QDateTime::currentDateTime();

            // Одна и та же беда не должна сообщаться каждые полсекунды: она
            // держится десятками секунд, и повтор превратил бы её в шум,
            // который оператор перестанет замечать.
            //
            // УРОВЕНЬ — ЧАСТЬ КЛЮЧА, А НЕ ТОЛЬКО ДОРОЖКА И ПОЛОЖЕНИЕ. Раньше
            // ключ не различал «ВНИМАНИЕ» и «ТРЕВОГУ» по одному и тому же
            // положению: объявленное «внимание» ставило отметку времени,
            // которая следующие шестьдесят секунд не пускала уже саму
            // ТРЕВОГУ — та требовала того же интервала от ТОЙ ЖЕ отметки.
            // Человек падал, «внимание» успевало прозвучать и уйти в журнал
            // за секунду-другую до того, как положение дотягивало до
            // тревожного порога, — и настоящая тревога после этого молча
            // проглатывалась до конца минуты. Раздельные ключи на каждый
            // уровень не мешают друг другу: усиление всегда сообщается сразу,
            // а от шума бережёт тот же интервал, но уже внутри своего уровня.
            const QString levelKey = QStringLiteral("%1/%2/%3")
                                         .arg(trackId).arg(int(worst.situation))
                                         .arg(int(worst.level));
            const qint64 lastTime = state.announced.value(levelKey, -1000000);

            if (now - lastTime > m_repeatDangerMs) {
                state.announced.insert(levelKey, now);
                if (worst.level == Level::Alarm)
                    emit dangerDetected(report);
                else
                    emit attentionDetected(report);
            }
        }

        result.people.append(person);
    }
}

} // namespace core
