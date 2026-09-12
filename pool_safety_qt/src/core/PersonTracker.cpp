#include "core/PersonTracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace core {

namespace {

double vectorLength(const QPointF &vector)
{
    return std::sqrt(vector.x() * vector.x() + vector.y() * vector.y());
}

double median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if (values.size() % 2 == 1)
        return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

double intersectionOverUnion(const QRect &a, const QRect &b)
{
    const QRect common = a.intersected(b);
    if (common.isEmpty())
        return 0.0;
    const double overlap = double(common.width()) * common.height();
    const double total = double(a.width()) * a.height()
                         + double(b.width()) * b.height() - overlap;
    return total > 0.0 ? overlap / total : 0.0;
}

/// Среднее смещение точек, видимых в обоих замерах, в точках экрана.
/// Пусто — сравнивать нечего: общих видимых точек нет.
std::optional<double> sharedShift(const Sample &previous, const Sample &current)
{
    double sum = 0.0;
    int shared = 0;
    for (int index = 0; index < kp::Count; ++index) {
        if (!previous.pose.visible(index) || !current.pose.visible(index))
            continue;
        sum += vectorLength(current.pose.point(index) - previous.pose.point(index));
        ++shared;
    }
    if (shared == 0)
        return std::nullopt;
    return sum / shared;
}

/// Среднее смещение видимых в обоих замерах кистей, в точках экрана. Пусто —
/// ни одна кисть не видна в обоих замерах сразу. Та же мера, что уже
/// используется в Track::metrics() для armActivity, но для одной пары
/// замеров — нужна отдельно, чтобы отследить, с какого момента человек
/// начал энергично двигать руками (см. Track::splashingSeconds()).
std::optional<double> wristShift(const Sample &previous, const Sample &current)
{
    double sum = 0.0;
    int shared = 0;
    for (int index : {kp::LeftWrist, kp::RightWrist}) {
        if (!previous.pose.visible(index) || !current.pose.visible(index))
            continue;
        sum += vectorLength(current.pose.point(index) - previous.pose.point(index));
        ++shared;
    }
    if (shared == 0)
        return std::nullopt;
    return sum / shared;
}

/// Наибольшая скорость изменения наклона туловища по всей переданной
/// истории, °/с. Сглажено медианой по трём соседним замерам: без этого
/// одиночный сбой скелета (модель на кадр путает плечи с бёдрами) выглядит
/// как мгновенный поворот тела на полсотни градусов, и правило падения
/// срабатывало на спокойно идущем человеке.
///
/// Вынесена отдельной функцией, а не оставлена внутри Track::metrics(),
/// потому что нужна из двух разных мест: metrics() считает её по КАЖДОМУ
/// разбору для строки состояния, а Track::add() — один раз, в момент
/// перехода в горизонталь, чтобы ЗАПОМНИТЬ и не потерять свидетельство
/// падения, когда сам всплеск позже уйдёт из хранимой истории (см.
/// Track::fallTiltRate()).
double maxTiltChange(const std::deque<Sample> &samples)
{
    std::vector<std::pair<double, double>> tilts;
    for (const Sample &sample : samples) {
        if (sample.features.torsoTilt)
            tilts.emplace_back(sample.timestamp, *sample.features.torsoTilt);
    }
    if (tilts.size() < 4)
        return 0.0;

    std::vector<std::pair<double, double>> smoothed;
    smoothed.reserve(tilts.size());
    for (size_t i = 1; i + 1 < tilts.size(); ++i) {
        smoothed.emplace_back(tilts[i].first,
                              median({tilts[i - 1].second, tilts[i].second,
                                      tilts[i + 1].second}));
    }

    double change = 0.0;
    for (size_t i = 1; i < smoothed.size(); ++i) {
        const double dt = std::max(1e-3, smoothed[i].first - smoothed[i - 1].first);
        change = std::max(change, std::abs(smoothed[i].second - smoothed[i - 1].second) / dt);
    }
    return change;
}

} // namespace

// ------------------------------------------------------------------ дорожка

void Track::updateDurations(double timestamp, const Sample &previous,
                            const PoseFeatures &features, const Pose &pose)
{
    // Неподвижность: сравниваем с предыдущим замером. Условие нарушено —
    // отсчёт начинается заново.
    const double dt = timestamp - previous.timestamp;
    bool moving = true;
    if (dt > 1e-3) {
        const double scale = features.torsoLength.value_or(
            previous.features.torsoLength.value_or(0.0));
        const std::optional<double> shift = sharedShift(previous, {timestamp, features, pose});
        if (scale > 1e-6 && shift)
            moving = (*shift / scale / dt) > kStillThreshold;
    }
    if (moving)
        m_stillSince = timestamp;

    // Лежит.
    const bool horizontal = features.torsoTilt && *features.torsoTilt >= 55.0;
    if (!horizontal)
        m_horizontalSince = timestamp;

    // Вертикально и без видимых ног — «завис в воде».
    const bool submerged = features.torsoTilt && *features.torsoTilt <= 35.0
                           && features.lowerBodyRatio <= 0.25;
    if (!submerged)
        m_submergedSince = timestamp;

    // Брызги на месте: кисти двигаются энергично, а центр тела остаётся у
    // точки, где отсчёт начался. Якорь переносится в текущую точку, а отсчёт
    // начинается заново, когда условие рвётся — либо руки успокоились, либо
    // человек в самом деле сместился (значит, плывёт, а не барахтается). См.
    // Track::splashingSeconds() и Situation::Splashing в SituationRules.h.
    bool energetic = false;
    if (dt > 1e-3) {
        const double scale = features.torsoLength.value_or(
            previous.features.torsoLength.value_or(0.0));
        const std::optional<double> wrists = wristShift(previous, {timestamp, features, pose});
        if (scale > 1e-6 && wrists)
            energetic = (*wrists / scale / dt) >= kSplashActivitySpeed;
    }

    if (!features.center) {
        // Центр не измерен — снести отсчёт нельзя проверить, поэтому не
        // рискуем и гасим его: считать «на месте» без доказательства нечестно.
        m_splashSince = timestamp;
    } else if (!energetic) {
        m_splashAnchor = *features.center;
        m_splashSince = timestamp;
    } else {
        const double scale = features.torsoLength.value_or(1.0);
        const double drift = scale > 1e-6
            ? vectorLength(*features.center - m_splashAnchor) / scale
            : 0.0;
        if (drift > kSplashDriftLimit) {
            m_splashAnchor = *features.center;
            m_splashSince = timestamp;
        }
        // Иначе — энергично и всё ещё у якоря: отсчёт не трогаем, он идёт
        // с момента, когда условие стало верным в последний раз подряд.
    }
}

void Track::add(double timestamp, const Pose &pose)
{
    // Скорость обновляется скользящим средним: одиночный рывок рамки не
    // должен уводить предсказание в сторону, но за настоящим движением оно
    // обязано поспевать.
    if (const Sample *previous = latest()) {
        const double dt = timestamp - previous->timestamp;
        if (dt > 1e-3) {
            const QPointF shift = QRectF(pose.box).center()
                                  - QRectF(previous->pose.box).center();
            const QPointF measured = shift / dt;
            m_velocity = m_samples.size() < 2 ? measured
                                              : m_velocity * 0.5 + measured * 0.5;
        }
    }

    Sample sample;
    sample.timestamp = timestamp;
    sample.pose = pose;
    sample.features = extractFeatures(pose);

    const Sample *previous = latest();
    const bool horizontalNow = sample.features.torsoTilt
                               && *sample.features.torsoTilt >= 55.0;

    if (previous) {
        updateDurations(timestamp, *previous, sample.features, pose);
    } else {
        // Первый замер: отсчёт всех состояний начинается сейчас.
        m_stillSince = m_horizontalSince = m_submergedSince = m_splashSince = timestamp;
        if (sample.features.center)
            m_splashAnchor = *sample.features.center;
    }

    m_samples.push_back(std::move(sample));
    m_lastSeen = timestamp;

    // Падение: резкий переход в горизонталь. Пересчитываем резкость на
    // каждом кадре, пока переход совсем свежий (kFallSettleSeconds) — за
    // одну сглаживающая медиана в maxTiltChange ещё не отличит настоящий
    // скачок наклона от единичного сбоя скелета, ей нужно два-три замера
    // ПОСЛЕ перехода. Как только распознавание устоялось — ЗАМОРАЖИВАЕМ
    // значение и дальше его не трогаем, пока человек не встанет. См.
    // Track::fallTiltRate(): без заморозки то же самое свидетельство падения
    // потерялось бы, стоило самому всплеску уйти из хранимой истории — то
    // самое исправление пропущенной тревоги.
    if (!horizontalNow)
        m_fallTiltRate = 0.0;
    else if (m_lastSeen - m_horizontalSince <= kFallSettleSeconds)
        m_fallTiltRate = maxTiltChange(m_samples);

    while (!m_samples.empty()
           && timestamp - m_samples.front().timestamp > PersonTracker::kHistorySeconds) {
        m_samples.pop_front();
    }
}

QRect Track::predictedBox(double timestamp) const
{
    const Sample *last = latest();
    if (!last)
        return QRect();

    const double dt = qBound(0.0, timestamp - last->timestamp, 2.0);
    const QPointF shift = m_velocity * dt;
    return last->pose.box.translated(int(shift.x()), int(shift.y()));
}

WindowMetrics Track::metrics(double window) const
{
    WindowMetrics result;
    if (m_samples.empty())
        return result;

    const double now = m_samples.back().timestamp;

    std::vector<const Sample *> chosen;
    for (const Sample &sample : m_samples) {
        if (now - sample.timestamp <= window)
            chosen.push_back(&sample);
    }
    if (chosen.size() < 2)
        return result;

    result.span = chosen.back()->timestamp - chosen.front()->timestamp;
    if (result.span < 1e-3)
        return result;

    // Масштаб: всё меряем в длинах туловища, чтобы дальний человек и ближний
    // оценивались одинаково.
    std::vector<double> scales;
    for (const Sample *sample : chosen) {
        if (sample->features.torsoLength && *sample->features.torsoLength > 1e-6)
            scales.push_back(*sample->features.torsoLength);
    }
    const double scale = median(scales);

    if (scale > 1e-6) {
        // --- подвижность --------------------------------------------------
        double speedSum = 0.0;
        int speedCount = 0;
        double armSum = 0.0;
        int armCount = 0;

        for (size_t i = 1; i < chosen.size(); ++i) {
            const Sample &previous = *chosen[i - 1];
            const Sample &current = *chosen[i];
            const double dt = current.timestamp - previous.timestamp;
            if (dt < 1e-3)
                continue;

            if (const std::optional<double> shift = sharedShift(previous, current)) {
                speedSum += *shift / scale / dt;
                ++speedCount;
            }

            double wristSum = 0.0;
            int wristCount = 0;
            for (int wrist : {kp::LeftWrist, kp::RightWrist}) {
                if (!previous.pose.visible(wrist) || !current.pose.visible(wrist))
                    continue;
                wristSum += vectorLength(current.pose.point(wrist)
                                         - previous.pose.point(wrist));
                ++wristCount;
            }
            if (wristCount > 0) {
                armSum += wristSum / wristCount / scale / dt;
                ++armCount;
            }
        }

        if (speedCount > 0)
            result.motion = speedSum / speedCount;
        if (armCount > 0)
            result.armActivity = armSum / armCount;

        // --- перемещение --------------------------------------------------
        std::vector<QPointF> centers;
        for (const Sample *sample : chosen) {
            if (sample->features.center)
                centers.push_back(*sample->features.center);
        }
        if (centers.size() >= 2) {
            result.displacement = vectorLength(centers.back() - centers.front()) / scale;

            // Боковые колебания: отклонение от прямой, соединяющей начало и
            // конец пути. У трезвого человека путь почти прямой, у пьяного
            // центр тела уходит вбок и возвращается. Это подозрение, а не
            // диагноз.
            const QPointF start = centers.front();
            const QPointF direction = centers.back() - start;
            const double pathLength = vectorLength(direction);
            if (pathLength > 0.5 * scale) {
                const QPointF normal(-direction.y() / pathLength,
                                     direction.x() / pathLength);
                double sum = 0.0;
                double squares = 0.0;
                for (const QPointF &point : centers) {
                    const QPointF relative = point - start;
                    const double offset = relative.x() * normal.x()
                                          + relative.y() * normal.y();
                    sum += offset;
                    squares += offset * offset;
                }
                const double count = double(centers.size());
                const double mean = sum / count;
                const double variance = std::max(0.0, squares / count - mean * mean);
                result.sway = std::sqrt(variance) / scale;
            }
        }
    }

    // --- как долго держится состояние ------------------------------------
    // Берутся готовыми: они накапливаются при каждом замере и не ограничены
    // глубиной хранимой истории (см. пояснение в PersonTracker.h).
    result.stillSeconds = stillnessSeconds();
    result.horizontalSeconds = horizontalSeconds();
    result.uprightSubmergedSeconds = uprightSubmergedSeconds();

    // --- резкость изменения наклона --------------------------------------
    //
    // Наклон сглаживается медианой по трём соседним замерам. Без этого
    // одиночный сбой скелета (модель на кадр перепутала плечи с бёдрами)
    // выглядит как мгновенный поворот тела на полсотни градусов, и правило
    // падения срабатывает на спокойно идущем человеке. Проверено: именно так
    // и было до сглаживания.
    //
    // Тоже по всей истории: падение могло случиться до начала окна, а человек
    // всё ещё лежит — и это именно та тревога, которую нельзя потерять.
    //
    // Это значение — снимок «сейчас», для строки состояния и для отладки.
    // Правило падения (fall() в SituationRules.cpp) им больше НЕ пользуется —
    // оно смотрит на Track::fallTiltRate(), запомненную один раз в момент
    // перехода в горизонталь: иначе через 15 секунд (предел хранимой истории)
    // сам всплеск уходит из m_samples, это поле обнуляется, и правило гаснет,
    // хотя человек так и лежит.
    result.tiltChange = maxTiltChange(m_samples);

    // --- видимость --------------------------------------------------------
    int headVisible = 0;
    int submerged = 0;
    for (const Sample *sample : chosen) {
        if (sample->features.headVisible)
            ++headVisible;
        if (sample->features.lowerBodyRatio <= 0.25)
            ++submerged;
    }
    result.headVisibleRatio = double(headVisible) / double(chosen.size());
    result.submergedRatio = double(submerged) / double(chosen.size());

    return result;
}

// ------------------------------------------------------------ сопровождение

QVector<int> PersonTracker::update(const QVector<Pose> &poses, double timestamp)
{
    QVector<int> updated;
    updated.reserve(poses.size());

    // Жадное сопоставление в две ступени. Венгерский алгоритм здесь был бы
    // сложностью без выигрыша: людей в кадре бассейна единицы.
    for (const Pose &pose : poses) {
        int bestId = -1;
        double bestScore = kIouThreshold;

        // Ступень 1 — пересечение с ПРЕДСКАЗАННОЙ рамкой. Предсказание уже
        // учло, куда человек успел уйти с прошлого разбора.
        for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it) {
            if (updated.contains(it.key()))
                continue;
            const QRect predicted = it.value().predictedBox(timestamp);
            if (predicted.isEmpty())
                continue;
            const double score = intersectionOverUnion(pose.box, predicted);
            if (score > bestScore) {
                bestScore = score;
                bestId = it.key();
            }
        }

        // Ступень 2 — человек прошёл больше собственной ширины, перекрытия
        // нет вовсе. Тогда смотрим, не стоит ли он вплотную к тому месту, где
        // его ждали, и похож ли по размеру. Без этой ступени дорожки идущих
        // людей рвутся, и все временные пороги становятся недостижимы.
        if (bestId < 0) {
            double bestDistance = std::numeric_limits<double>::max();
            for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it) {
                if (updated.contains(it.key()))
                    continue;
                const QRect predicted = it.value().predictedBox(timestamp);
                if (predicted.isEmpty())
                    continue;

                const double height = std::max(1.0, double(predicted.height()));
                const double ratio = pose.box.height() / height;
                if (ratio < kSizeRatioLow || ratio > kSizeRatioHigh)
                    continue;

                const QPointF offset = QRectF(pose.box).center()
                                       - QRectF(predicted).center();
                const double distance = std::sqrt(offset.x() * offset.x()
                                                  + offset.y() * offset.y());
                if (distance < height * kCentreDistance && distance < bestDistance) {
                    bestDistance = distance;
                    bestId = it.key();
                }
            }
        }

        if (bestId < 0) {
            bestId = m_nextId++;
            m_tracks.insert(bestId, Track(bestId, timestamp));
        }

        m_tracks[bestId].add(timestamp, pose);
        updated.append(bestId);
    }

    // Забываем тех, кого давно не видели.
    for (auto it = m_tracks.begin(); it != m_tracks.end(); ) {
        if (timestamp - it.value().lastSeen() > kHoldSeconds)
            it = m_tracks.erase(it);
        else
            ++it;
    }

    return updated;
}

QVector<const Track *> PersonTracker::tracks() const
{
    QVector<const Track *> result;
    result.reserve(m_tracks.size());
    for (auto it = m_tracks.constBegin(); it != m_tracks.constEnd(); ++it)
        result.append(&it.value());
    return result;
}

const Track *PersonTracker::track(int id) const
{
    auto it = m_tracks.constFind(id);
    return it == m_tracks.constEnd() ? nullptr : &it.value();
}

void PersonTracker::reset()
{
    m_tracks.clear();
    m_nextId = 1;
}

} // namespace core
