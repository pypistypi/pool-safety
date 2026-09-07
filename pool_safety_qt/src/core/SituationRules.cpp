#include "core/SituationRules.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace core {

namespace {

double medianOf(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if (values.size() % 2 == 1)
        return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

double distance(const QPointF &a, const QPointF &b)
{
    const QPointF d = a - b;
    return std::sqrt(d.x() * d.x() + d.y() * d.y());
}

QString seconds(double value)
{
    return QString::number(value, 'f', value < 10.0 ? 1 : 0);
}

// ---------------------------------------------------------------------------
//  Зависание в воде — то, что похоже на утопление
// ---------------------------------------------------------------------------
//
//  Опознаётся НЕ по «махам руками»: настоящее утопление тихое, на крик и
//  размахивание не остаётся дыхания. Опознаётся по сочетанию:
//
//    * тело вертикально (а не горизонтально, как у плывущего);
//    * ног не видно — человек в воде;
//    * продвижения нет: пловец за шесть секунд уходит на корпус-другой,
//      зависший остаётся на месте;
//    * голова у самой воды или периодически пропадает.
//
//  Ни один признак сам по себе ничего не значит. Вертикально в воде стоит и
//  тот, кто просто отдыхает, — его выдаёт то, что голова уверенно над водой и
//  что он рано или поздно двинется.
Verdict drowning(const Track &track, const WindowMetrics &metrics, const Thresholds &t,
                 bool waterInView)
{
    Verdict verdict;
    verdict.situation = Situation::Drowning;

    // Нет воды в кадре — нечему и тонуть. Признак задаёт оператор: догадка по
    // невидимым ногам годится, чтобы уточнить обстоятельство, но не чтобы
    // объявить утопление там, где воды нет.
    if (!waterInView)
        return verdict;

    const double held = metrics.uprightSubmergedSeconds;
    if (held < 1.0)
        return verdict;

    verdict.heldSeconds = held;
    verdict.reasons << QStringLiteral("тело вертикально, ног не видно — %1 с")
                           .arg(seconds(held));

    const bool noProgress = metrics.displacement
                            && *metrics.displacement < t.drowningMaxDisplacement;
    if (noProgress) {
        verdict.reasons << QStringLiteral("нет продвижения (%1 длины тела)")
                               .arg(*metrics.displacement, 0, 'f', 1);
    }

    const bool headTrouble = metrics.headVisibleRatio < t.drowningHeadVisibleMin;
    if (headTrouble) {
        verdict.reasons << QStringLiteral("голова видна лишь %1% времени")
                               .arg(int(metrics.headVisibleRatio * 100));
    }

    const Sample *latest = track.latest();

    // У стоящего в воде голова заметно выше бёдер. Если она опустилась к линии
    // бёдер, человек уходит под воду или запрокинулся.
    bool headLow = false;
    if (latest && latest->features.headAboveHips) {
        headLow = *latest->features.headAboveHips < 0.55;
        if (headLow)
            verdict.reasons << QStringLiteral("голова опустилась к линии воды");
    }

    const bool handsUp = latest && latest->features.wristsAboveShoulders >= 1;
    if (handsUp)
        verdict.reasons << QStringLiteral("руки подняты — возможен зов о помощи");

    // Затухание движений: человек ещё шевелится, но всё слабее. Поздняя
    // стадия, и пропускать её нельзя.
    const bool fading = metrics.armActivity
                        && *metrics.armActivity > 0.02 && *metrics.armActivity < 0.15;
    if (fading)
        verdict.reasons << QStringLiteral("движения рук слабеют");

    const int supporting = int(noProgress) + int(headTrouble) + int(headLow)
                           + int(handsUp) + int(fading);

    if (held >= t.drowningUprightAlarm && supporting >= 1) {
        verdict.level = Level::Alarm;
    } else if (held >= t.drowningUprightAttention && supporting >= 1) {
        verdict.level = Level::Attention;
    } else if (headTrouble && headLow) {
        // Даже недолго: голова у воды и пропадает — ждать нечего.
        verdict.level = Level::Attention;
    }

    if (verdict.level == Level::Normal)
        verdict.reasons.clear();
    return verdict;
}

// ---------------------------------------------------------------------------
//  Лежит без движения
// ---------------------------------------------------------------------------
//
//  Самое надёжное из правил: горизонтальное положение и неподвижность
//  измеряются точно и почти не зависят от бликов. Спящего на лежаке оно тоже
//  поймает — и это правильно: отличить сон от потери сознания по картинке
//  нельзя, а цена ошибки в разные стороны несопоставима.
Verdict unconscious(const Track &track, const WindowMetrics &metrics, const Thresholds &t,
                    bool waterInView)
{
    Verdict verdict;
    verdict.situation = Situation::Unconscious;

    const double held = std::min(metrics.horizontalSeconds, metrics.stillSeconds);
    if (held < 1.0)
        return verdict;

    verdict.heldSeconds = held;
    verdict.reasons << QStringLiteral("лежит без движения %1 с").arg(seconds(held));

    const Sample *latest = track.latest();
    const bool inWater = waterInView && latest && latest->features.lowerBodyRatio <= 0.25;
    if (inWater)
        verdict.reasons << QStringLiteral("ног не видно — вероятно, в воде");

    if (latest && !latest->features.headVisible)
        verdict.reasons << QStringLiteral("голова не просматривается");

    // В воде и на суше — разное терпение. Неподвижность в воде недопустима
    // ни секунды сверх меры; неподвижность на бортике — это ещё и загорающий
    // на лежаке, и будить оператора из-за каждого отдыхающего нельзя: через
    // смену он перестанет верить сирене.
    if (inWater) {
        if (held >= t.unconsciousInWaterAlarm)
            verdict.level = Level::Alarm;
        else if (held >= t.unconsciousInWaterAttention)
            verdict.level = Level::Attention;
    } else if (held >= t.unconsciousStillAlarm) {
        verdict.level = Level::Alarm;
        verdict.reasons << QStringLiteral(
            "на суше: столько не лежат даже загорая");
    } else if (held >= t.unconsciousStillAttention) {
        verdict.level = Level::Attention;
        verdict.reasons << QStringLiteral(
            "возможно, просто отдыхает — взгляните");
    }

    if (verdict.level == Level::Normal)
        verdict.reasons.clear();
    return verdict;
}

// ---------------------------------------------------------------------------
//  Падение
// ---------------------------------------------------------------------------
//
//  Падение — это не «человек лежит», а «человек резко перешёл из вертикали в
//  горизонталь». Быстрый переход отличает падение от того, кто спокойно лёг на
//  лежак: лечь аккуратно за полсекунды невозможно.
//
//  Само по себе падение ещё не беда — упавший обычно встаёт. Опасно, если он
//  остался лежать.
Verdict fall(const Track &, const WindowMetrics &metrics, const Thresholds &t)
{
    Verdict verdict;
    verdict.situation = Situation::Fall;

    if (metrics.tiltChange < t.fallTiltRate)
        return verdict;

    const double lying = metrics.horizontalSeconds;

    // Резкого поворота МАЛО. Сбой распознавания на одном кадре выглядит точно
    // так же — модель на мгновение путает плечи с бёдрами, и наклон
    // «подскакивает». Отличие настоящего падения в том, что после него человек
    // ОСТАЁТСЯ лежать. Проверено на записи с идущими людьми: без этого условия
    // правило срабатывало на спокойно идущем человеке с наклоном туловища 1°.
    if (lying < 0.4)
        return verdict;

    verdict.heldSeconds = lying;
    verdict.reasons << QStringLiteral("резкий переход в горизонталь (%1°/с)")
                           .arg(int(metrics.tiltChange));

    if (lying >= t.fallStayAlarm) {
        verdict.reasons << QStringLiteral("после падения не встаёт %1 с").arg(seconds(lying));
        verdict.level = Level::Alarm;
    } else {
        verdict.reasons << QStringLiteral("лежит %1 с").arg(seconds(lying));
        verdict.level = Level::Attention;
    }
    return verdict;
}

// ---------------------------------------------------------------------------
//  Ребёнок без присмотра
// ---------------------------------------------------------------------------
//
//  Ребёнок опознаётся по пропорциям, а не по росту в точках экрана: дальний
//  взрослый мельче ближнего ребёнка. У детей голова относительно туловища
//  заметно крупнее — устойчивый признак из антропометрии, не зависящий от
//  расстояния до камеры. Рост используется как второй, вспомогательный
//  признак — в сравнении с другими людьми в кадре.
Verdict childAlone(const Track &track, const Thresholds &t,
                   const QVector<const Track *> &others, bool waterInView)
{
    Verdict verdict;
    verdict.situation = Situation::ChildAlone;

    const Sample *latest = track.latest();
    if (!latest || !latest->features.isUsable())
        return verdict;

    const PoseFeatures &features = latest->features;

    // Пропорции годятся в дело, только если туловище видно не в ракурсе.
    // Иначе ракурс сжимает его, и любой взрослый вблизи камеры выглядит как
    // младенец — именно так и получилась ложная тревога на веб-камере.
    //
    // Мерой служит отношение длины туловища к ширине плеч: оно не требует
    // видеть ноги, а значит работает и в воде.
    const bool proportionsTrustworthy =
        features.shoulderWidth && features.torsoLength
        && *features.shoulderWidth > 1e-6
        && *features.torsoLength / *features.shoulderWidth
               >= t.childMinTorsoToShoulders;

    const bool childByHead = proportionsTrustworthy && features.headToTorso
                             && *features.headToTorso >= t.childHeadToTorso
                             && *features.headToTorso <= t.childHeadToTorsoMax;

    // Рост в сравнении с остальными. Берём медиану, чтобы один ребёнок в
    // компании детей не «поднял планку» и не спрятал сам себя.
    bool childByHeight = false;
    std::vector<double> heights;
    for (const Track *other : others) {
        const Sample *sample = other->latest();
        if (sample && sample->features.torsoLength)
            heights.push_back(*sample->features.torsoLength);
    }
    const double medianHeight = medianOf(heights);
    if (medianHeight > 1e-6 && features.torsoLength)
        childByHeight = *features.torsoLength / medianHeight <= t.childHeightRatio;

    // РОСТ САМ ПО СЕБЕ НИЧЕГО НЕ ЗНАЧИТ. Камера не знает расстояния до
    // человека, поэтому «ниже остальных в кадре» и «стоит дальше остальных» —
    // для неё одно и то же. Проверено на записи с прохожими: дальний взрослый
    // исправно опознавался ребёнком. Поэтому рост оставлен только как довод в
    // пользу уже найденных пропорций, а поводом для вывода служить не может.
    if (!childByHead)
        return verdict;

    verdict.reasons << QStringLiteral("пропорции ребёнка (голова/туловище %1)")
                           .arg(*features.headToTorso, 0, 'f', 2);
    if (childByHeight)
        verdict.reasons << QStringLiteral("и ниже остальных в кадре");

    // Есть ли рядом взрослый — И БОДРСТВУЕТ ЛИ ОН.
    //
    // СПЯЩИЙ ВЗРОСЛЫЙ — НЕ ПРИСМОТР. Случай известный и кончался гибелью
    // ребёнка: мать спала на лежаке, дочь в это время утонула. Формально
    // взрослый был рядом, фактически ребёнок был один. Поэтому рядом стоящий
    // неподвижный человек присмотром не считается.
    const double scale = features.torsoLength.value_or(1.0);
    const double radius = t.adultNearRadius * scale;
    bool adultNear = false;
    bool adultAsleep = false;

    for (const Track *other : others) {
        if (other->id() == track.id())
            continue;
        const Sample *sample = other->latest();
        if (!sample || !sample->features.center || !features.center)
            continue;
        // Взрослым считаем того, кто заметно крупнее.
        if (sample->features.torsoLength && features.torsoLength
            && *sample->features.torsoLength < *features.torsoLength * 1.15) {
            continue;
        }
        if (distance(*sample->features.center, *features.center) > radius)
            continue;

        if (other->stillnessSeconds() >= t.adultAsleepSeconds) {
            // Взрослый есть, но он неподвижен дольше, чем бывает у человека,
            // который следит за ребёнком.
            adultAsleep = true;
            continue;
        }

        adultNear = true;
        break;
    }

    if (adultNear) {
        verdict.reasons.clear();
        return verdict;
    }

    verdict.reasons << (adultAsleep
                            ? QStringLiteral("взрослый рядом есть, но неподвижен "
                                             "— возможно, спит")
                            : QStringLiteral("взрослых рядом нет"));
    const double aloneFor = track.age();
    verdict.heldSeconds = aloneFor;

    if (waterInView && features.lowerBodyRatio <= 0.25) {
        verdict.reasons << QStringLiteral("ног не видно — вероятно, в воде");
        // В воде ребёнку без взрослого терпения куда меньше, чем на бортике,
        // но не ноль: одиночный сбой скелета не должен включать сирену.
        verdict.level = aloneFor >= t.childAloneAttention ? Level::Alarm
                                                          : Level::Attention;
    } else if (adultAsleep) {
        // Ребёнок на бортике, а единственный взрослый рядом спит. Само по
        // себе это ещё не беда, но именно отсюда начинается тот случай,
        // ради которого правило и переделывалось.
        verdict.level = Level::Attention;
    } else if (aloneFor >= t.childAloneAlarm) {
        verdict.level = Level::Alarm;
    } else if (aloneFor >= t.childAloneAttention) {
        verdict.level = Level::Attention;
    }

    // Признак ребёнка не проверен ни на одном настоящем ребёнке, поэтому по
    // умолчанию правило не имеет права на сирену — см. Thresholds.
    //
    // ИСКЛЮЧЕНИЕ: маленький человек один в воде, а взрослый рядом неподвижен.
    // Это ровно тот случай, ради которого правило и переделывалось, и глушить
    // его до «внимания» нельзя — цена ошибки здесь несопоставима с досадой от
    // ложной сирены.
    const bool childInWaterAsleepAdult =
        waterInView && features.lowerBodyRatio <= 0.25 && adultAsleep;

    if (!t.childAlarmAllowed && verdict.level == Level::Alarm
        && !childInWaterAsleepAdult) {
        verdict.level = Level::Attention;
        verdict.reasons << QStringLiteral(
            "тревога по этому признаку выключена: порог не проверен на детях");
    }

    if (verdict.level == Level::Normal)
        verdict.reasons.clear();
    return verdict;
}

// ---------------------------------------------------------------------------
//  Неуверенная походка
// ---------------------------------------------------------------------------
//
//  ЭТО ПОДОЗРЕНИЕ, А НЕ ДИАГНОЗ, и выше уровня «ВНИМАНИЕ» правило не
//  поднимается никогда. По видео нельзя установить опьянение: та же шаткая
//  походка бывает при головокружении, травме, болезни, у пожилого человека и
//  просто на мокром скользком кафеле.
//
//  Измеряется одно: насколько путь человека виляет. Трезвый идёт почти по
//  прямой, у пьяного центр тела уходит вбок и возвращается. Дополнительный
//  признак — неестественно широкая расстановка стоп: так удерживают
//  равновесие.
//
//  Польза есть: рядом с водой такому человеку нужно внимание независимо от
//  причины его состояния.
Verdict unsteady(const Track &track, const WindowMetrics &metrics, const Thresholds &t)
{
    Verdict verdict;
    verdict.situation = Situation::Unsteady;

    if (!metrics.sway || metrics.span < 2.0)
        return verdict;

    const Sample *latest = track.latest();

    // Человек слишком мелкий в кадре, чтобы судить о его походке. Дрожание
    // точек скелета измеряется в точках экрана, а виляние — в долях длины
    // туловища: у дальней фигуры первое раздувает второе на ровном месте.
    // Проверено: на записи с прохожими (туловище около 30 точек) правило
    // объявляло виляние 0,43–0,46 у людей, идущих строго по прямой.
    if (!latest || !latest->features.torsoLength
        || *latest->features.torsoLength < t.unsteadyMinTorsoPx) {
        return verdict;
    }

    // Идёт ли человек вообще и достаточно ли прошёл: на двух шагах любая
    // случайность выглядит как виляние.
    if (!metrics.displacement || *metrics.displacement < t.unsteadyMinDisplacement)
        return verdict;

    if (*metrics.sway < t.swayAttention)
        return verdict;

    verdict.reasons << QStringLiteral("путь виляет (отклонение %1 длины тела)")
                           .arg(*metrics.sway, 0, 'f', 2);

    if (latest->features.supportWidth && *latest->features.supportWidth > 0.75)
        verdict.reasons << QStringLiteral("широкая расстановка стоп — удержание равновесия");

    if (*metrics.sway >= t.swayAlarm && metrics.span >= t.unsteadyHold)
        verdict.reasons << QStringLiteral("держится устойчиво — нужен присмотр");

    // Уровень намеренно не поднимается до тревоги — см. пояснение выше.
    verdict.level = Level::Attention;
    verdict.heldSeconds = metrics.span;
    return verdict;
}

} // namespace

// ---------------------------------------------------------------------------

QString situationText(Situation situation)
{
    switch (situation) {
    case Situation::Drowning:    return QStringLiteral("завис в воде без движения");
    case Situation::Unconscious: return QStringLiteral("лежит без движения");
    case Situation::Fall:        return QStringLiteral("упал и не встаёт");
    case Situation::ChildAlone:  return QStringLiteral("ребёнок без присмотра");
    case Situation::Unsteady:    return QStringLiteral("неуверенная походка");
    }
    return QStringLiteral("неизвестное положение");
}

QString situationAction(Situation situation)
{
    switch (situation) {
    case Situation::Drowning:
        return QStringLiteral("Посмотрите на панель немедленно. "
                              "Если человек не отзывается — доставать из воды.");
    case Situation::Unconscious:
        return QStringLiteral("Посмотрите на панель. Окликните человека; "
                              "если не отзывается — подойти.");
    case Situation::Fall:
        return QStringLiteral("Посмотрите на панель: человек упал и не поднялся.");
    case Situation::ChildAlone:
        return QStringLiteral("Найдите взрослого, отвечающего за ребёнка.");
    case Situation::Unsteady:
        return QStringLiteral("Присмотритесь: рядом с водой такому человеку "
                              "нужно внимание.");
    }
    return QString();
}

QString levelText(Level level)
{
    switch (level) {
    case Level::Normal:    return QStringLiteral("норма");
    case Level::Attention: return QStringLiteral("ВНИМАНИЕ");
    case Level::Alarm:     return QStringLiteral("ТРЕВОГА");
    }
    return QString();
}

QVector<Verdict> SituationAnalyzer::analyse(const Track &track,
                                            const QVector<const Track *> &others,
                                            bool waterInView) const
{
    QVector<Verdict> found;

    const Sample *latest = track.latest();
    if (!latest || !latest->features.isUsable())
        return found;

    const WindowMetrics metrics = track.metrics(m_thresholds.window);

    const QVector<Verdict> all = {
        drowning(track, metrics, m_thresholds, waterInView),
        unconscious(track, metrics, m_thresholds, waterInView),
        fall(track, metrics, m_thresholds),
        childAlone(track, m_thresholds, others, waterInView),
        unsteady(track, metrics, m_thresholds),
    };

    for (const Verdict &verdict : all) {
        if (verdict.level == Level::Normal)
            continue;
        if (!m_thresholds.isEnabled(verdict.situation))
            continue;   // правило выключено оператором — молчим совсем
        found.append(verdict);
    }
    return found;
}

Verdict SituationAnalyzer::worst(const QVector<Verdict> &verdicts)
{
    Verdict best;
    for (const Verdict &verdict : verdicts) {
        if (verdict.level > best.level
            || (verdict.level == best.level && verdict.heldSeconds > best.heldSeconds)) {
            best = verdict;
        }
    }
    return best;
}

} // namespace core
