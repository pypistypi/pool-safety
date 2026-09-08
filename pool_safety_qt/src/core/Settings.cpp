#include "core/Settings.h"

#include "core/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace core {

namespace {

// Ключи в этом файле русские, и читать их надо КАК UTF-8.
//
// Здесь стоял QLatin1String — и настройки молча не читались: каждый байт
// UTF-8 он считает отдельным символом, поэтому «громкость_тревоги» из
// исходника никогда не совпадала с тем же словом из файла. Программа
// послушно подставляла значения по умолчанию, а оператор был уверен, что
// его правки применились. Поймано проверкой settingsSurviveSaveAndLoad.
QJsonValue at(const QJsonObject &object, const char *key)
{
    return object.value(QString::fromUtf8(key));
}

double number(const QJsonObject &object, const char *key, double fallback)
{
    const QJsonValue value = at(object, key);
    return value.isDouble() ? value.toDouble() : fallback;
}

int integer(const QJsonObject &object, const char *key, int fallback)
{
    const QJsonValue value = at(object, key);
    return value.isDouble() ? value.toInt() : fallback;
}

bool flag(const QJsonObject &object, const char *key, bool fallback)
{
    const QJsonValue value = at(object, key);
    return value.isBool() ? value.toBool() : fallback;
}

} // namespace

QString Settings::defaultPath()
{
    return paths::settingsFile();
}

Settings Settings::load(const QString &path)
{
    Settings settings;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // Файла ещё нет — создаём с настройками по умолчанию, чтобы было что
        // править и чтобы список полей не приходилось искать в документации.
        settings.save(path);
        return settings;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return settings;

    const QJsonObject root = document.object();

    // Версии может не быть вовсе — значит файл написан до её появления.
    settings.version = integer(root, "версия_настроек", 1);

    const QJsonObject reactions = root.value(QStringLiteral("отклики")).toObject();
    settings.reactionsEnabled = flag(reactions, "включены", settings.reactionsEnabled);
    settings.presenceReaction = flag(reactions, "на_появление_людей",
                                     settings.presenceReaction);
    Thresholds &rules = settings.thresholds;
    rules.ruleDrowning    = flag(reactions, "завис_в_воде", rules.ruleDrowning);
    rules.ruleUnconscious = flag(reactions, "лежит_без_движения", rules.ruleUnconscious);
    rules.ruleFall        = flag(reactions, "упал_и_не_встаёт", rules.ruleFall);
    rules.ruleChildAlone  = flag(reactions, "ребёнок_без_присмотра", rules.ruleChildAlone);
    rules.ruleUnsteady    = flag(reactions, "неуверенная_походка", rules.ruleUnsteady);

    const QJsonObject sound = root.value(QStringLiteral("звук")).toObject();
    settings.soundEnabled       = flag(sound, "включён", settings.soundEnabled);
    settings.alarmVolume        = number(sound, "громкость_тревоги", settings.alarmVolume);
    settings.presenceVolume     = number(sound, "громкость_присутствия", settings.presenceVolume);
    settings.alarmRepeatSeconds = integer(sound, "повтор_тревоги_с", settings.alarmRepeatSeconds);
    settings.escalateAfterSeconds =
        integer(sound, "усилить_через_с", settings.escalateAfterSeconds);
    settings.presenceSound      = flag(sound, "сигнал_присутствия", settings.presenceSound);

    const QJsonObject window = root.value(QStringLiteral("окно_при_тревоге")).toObject();
    settings.popupLevel = integer(window, "всплывать_при_уровне", settings.popupLevel);
    settings.minimizeToTray = flag(window, "сворачивать_в_трей", settings.minimizeToTray);
    settings.raiseWindowOnAlarm = flag(window, "поднимать_окно", settings.raiseWindowOnAlarm);
    settings.flashTaskbarOnAlarm = flag(window, "мигать_в_панели_задач",
                                        settings.flashTaskbarOnAlarm);
    settings.blinkOnAlarm       = flag(window, "мигать_рамкой", settings.blinkOnAlarm);

    const QJsonObject video = root.value(QStringLiteral("видео")).toObject();
    settings.displayFpsCap = integer(video, "предел_кадров_в_секунду",
                                     settings.displayFpsCap);
    // Файл мог прийти со старым или испорченным значением — не даём панели
    // остаться совсем без ограничения показа или уйти в абсурдный предел.
    if (settings.displayFpsCap != 24 && settings.displayFpsCap != 30
        && settings.displayFpsCap != 60) {
        settings.displayFpsCap = 30;
    }

    const QJsonObject phones = root.value(QStringLiteral("телефоны")).toObject();
    settings.serverEnabled = flag(phones, "раздавать_события", settings.serverEnabled);
    settings.serverPort = integer(phones, "порт", settings.serverPort);

    const QJsonObject detection = root.value(QStringLiteral("распознавание")).toObject();
    settings.detectIntervalMs   = integer(detection, "интервал_разбора_мс",
                                          settings.detectIntervalMs);
    settings.searchConfidence   = number(detection, "порог_поиска_людей",
                                         settings.searchConfidence);
    settings.poseEnabled        = flag(detection, "разбор_позы", settings.poseEnabled);
    settings.selfLearning       = flag(detection, "учить_постоянное_окружение",
                                       settings.selfLearning);
    const int model = integer(detection, "модель_поиска_людей",
                              int(settings.detectorModel));
    if (model >= 0 && model <= int(PersonDetector::Model::Legacy))
        settings.detectorModel = PersonDetector::Model(model);
    settings.autoAlarm          = flag(detection, "тревога_автоматически", settings.autoAlarm);

    // ПЕРЕВОД СТАРЫХ НАСТРОЕК.
    //
    // Разбор шёл раз в 400 мс — два с половиной раза в секунду, — и рамка
    // заметно отставала от человека. Причина была не в скорости моделей, а в
    // этой паузе. Новое умолчание (120 мс) само по себе до уже настроенного
    // объекта не доехало бы: файл настроек перекрывает умолчания.
    //
    // Меняем только явно старое значение. Если оператор поставил свой темп,
    // трогать его нельзя: он мог сознательно разгрузить слабый компьютер.
    const bool needsMigration = settings.version < Settings::kCurrentVersion;

    if (settings.version < 2 && settings.detectIntervalMs == 400)
        settings.detectIntervalMs = 120;
    settings.version = Settings::kCurrentVersion;

    const QJsonArray water = root.value(QStringLiteral("панели_с_водой")).toArray();
    if (water.size() == settings.panelHasWater.size()) {
        for (int i = 0; i < water.size(); ++i)
            settings.panelHasWater[i] = water.at(i).toBool();
    }

    const QJsonObject t = root.value(QStringLiteral("пороги")).toObject();
    Thresholds &th = settings.thresholds;
    th.drowningUprightAttention = number(t, "вода_вертикально_внимание_с",
                                         th.drowningUprightAttention);
    th.drowningUprightAlarm     = number(t, "вода_вертикально_тревога_с",
                                         th.drowningUprightAlarm);
    th.drowningMaxDisplacement  = number(t, "вода_без_продвижения_длин",
                                         th.drowningMaxDisplacement);
    th.unconsciousStillAttention = number(t, "неподвижен_внимание_с",
                                          th.unconsciousStillAttention);
    th.unconsciousStillAlarm    = number(t, "неподвижен_тревога_с",
                                         th.unconsciousStillAlarm);
    th.unconsciousInWaterAlarm  = number(t, "неподвижен_в_воде_тревога_с",
                                         th.unconsciousInWaterAlarm);
    th.unconsciousInWaterAttention = number(t, "неподвижен_в_воде_внимание_с",
                                            th.unconsciousInWaterAttention);
    th.adultAsleepSeconds       = number(t, "взрослый_спит_через_с",
                                         th.adultAsleepSeconds);
    th.fallTiltRate             = number(t, "падение_скорость_град_с", th.fallTiltRate);
    th.fallStayAlarm            = number(t, "падение_не_встаёт_с", th.fallStayAlarm);
    th.childHeadToTorso         = number(t, "ребёнок_голова_к_туловищу",
                                         th.childHeadToTorso);
    th.childMinTorsoToShoulders = number(t, "ребёнок_туловище_к_плечам",
                                         th.childMinTorsoToShoulders);
    th.childAloneAlarm          = number(t, "ребёнок_один_тревога_с", th.childAloneAlarm);
    th.childAlarmAllowed        = flag(t, "ребёнок_разрешить_тревогу",
                                       th.childAlarmAllowed);
    th.window                   = number(t, "окно_разбора_с", th.window);

    // ФАЙЛ ЗАПИСЫВАЕТСЯ СРАЗУ, ЕСЛИ ЧТО-ТО ПЕРЕВЕДЕНО. Иначе перевод живёт
    // только в памяти этого запуска: программа работает уже по новым
    // значениям, а файл на диске выглядит нетронутым — старым, с прежним
    // "версия_настроек" отсутствующим вовсе. Ровно это увело по ложному следу
    // при разборе жалобы на нагрузку: файл показывал "интервал_разбора_мс":
    // 400, хотя запущенная программа честно работала на 120 — миграция
    // сработала, просто ни разу не сохранилась.
    if (needsMigration)
        settings.save(path);

    return settings;
}

bool Settings::save(const QString &path) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject reactions;
    reactions.insert(QStringLiteral("включены"), reactionsEnabled);
    reactions.insert(QStringLiteral("на_появление_людей"), presenceReaction);
    reactions.insert(QStringLiteral("завис_в_воде"), thresholds.ruleDrowning);
    reactions.insert(QStringLiteral("лежит_без_движения"), thresholds.ruleUnconscious);
    reactions.insert(QStringLiteral("упал_и_не_встаёт"), thresholds.ruleFall);
    reactions.insert(QStringLiteral("ребёнок_без_присмотра"), thresholds.ruleChildAlone);
    reactions.insert(QStringLiteral("неуверенная_походка"), thresholds.ruleUnsteady);

    QJsonObject sound;
    sound.insert(QStringLiteral("включён"), soundEnabled);
    sound.insert(QStringLiteral("громкость_тревоги"), alarmVolume);
    sound.insert(QStringLiteral("громкость_присутствия"), presenceVolume);
    sound.insert(QStringLiteral("повтор_тревоги_с"), alarmRepeatSeconds);
    sound.insert(QStringLiteral("усилить_через_с"), escalateAfterSeconds);
    sound.insert(QStringLiteral("сигнал_присутствия"), presenceSound);

    QJsonObject video;
    video.insert(QStringLiteral("предел_кадров_в_секунду"), displayFpsCap);

    QJsonObject window;
    window.insert(QStringLiteral("всплывать_при_уровне"), popupLevel);
    window.insert(QStringLiteral("сворачивать_в_трей"), minimizeToTray);
    window.insert(QStringLiteral("поднимать_окно"), raiseWindowOnAlarm);
    window.insert(QStringLiteral("мигать_в_панели_задач"), flashTaskbarOnAlarm);
    window.insert(QStringLiteral("мигать_рамкой"), blinkOnAlarm);

    QJsonObject detection;
    detection.insert(QStringLiteral("интервал_разбора_мс"), detectIntervalMs);
    detection.insert(QStringLiteral("порог_поиска_людей"), searchConfidence);
    detection.insert(QStringLiteral("разбор_позы"), poseEnabled);
    detection.insert(QStringLiteral("учить_постоянное_окружение"), selfLearning);
    detection.insert(QStringLiteral("модель_поиска_людей"), int(detectorModel));
    detection.insert(QStringLiteral("тревога_автоматически"), autoAlarm);

    QJsonObject t;
    t.insert(QStringLiteral("вода_вертикально_внимание_с"), thresholds.drowningUprightAttention);
    t.insert(QStringLiteral("вода_вертикально_тревога_с"), thresholds.drowningUprightAlarm);
    t.insert(QStringLiteral("вода_без_продвижения_длин"), thresholds.drowningMaxDisplacement);
    t.insert(QStringLiteral("неподвижен_внимание_с"), thresholds.unconsciousStillAttention);
    t.insert(QStringLiteral("неподвижен_тревога_с"), thresholds.unconsciousStillAlarm);
    t.insert(QStringLiteral("неподвижен_в_воде_тревога_с"), thresholds.unconsciousInWaterAlarm);
    t.insert(QStringLiteral("неподвижен_в_воде_внимание_с"),
             thresholds.unconsciousInWaterAttention);
    t.insert(QStringLiteral("взрослый_спит_через_с"), thresholds.adultAsleepSeconds);
    t.insert(QStringLiteral("падение_скорость_град_с"), thresholds.fallTiltRate);
    t.insert(QStringLiteral("падение_не_встаёт_с"), thresholds.fallStayAlarm);
    t.insert(QStringLiteral("ребёнок_голова_к_туловищу"), thresholds.childHeadToTorso);
    t.insert(QStringLiteral("ребёнок_туловище_к_плечам"),
             thresholds.childMinTorsoToShoulders);
    t.insert(QStringLiteral("ребёнок_один_тревога_с"), thresholds.childAloneAlarm);
    t.insert(QStringLiteral("ребёнок_разрешить_тревогу"), thresholds.childAlarmAllowed);
    t.insert(QStringLiteral("окно_разбора_с"), thresholds.window);

    QJsonArray water;
    for (bool value : panelHasWater)
        water.append(value);

    QJsonObject root;
    root.insert(QStringLiteral("версия_настроек"), Settings::kCurrentVersion);
    root.insert(QStringLiteral("_комментарий"), QStringLiteral(
        "Настройки поста наблюдения. Правятся из окна «Настройки» в самой "
        "программе; этот файл — то же самое, только текстом. Пороги заданы в "
        "секундах и в долях длины туловища человека."));
    QJsonObject phones;
    phones.insert(QStringLiteral("раздавать_события"), serverEnabled);
    phones.insert(QStringLiteral("порт"), serverPort);

    root.insert(QStringLiteral("отклики"), reactions);
    root.insert(QStringLiteral("телефоны"), phones);
    root.insert(QStringLiteral("звук"), sound);
    root.insert(QStringLiteral("видео"), video);
    root.insert(QStringLiteral("окно_при_тревоге"), window);
    root.insert(QStringLiteral("распознавание"), detection);
    root.insert(QStringLiteral("панели_с_водой"), water);
    root.insert(QStringLiteral("пороги"), t);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace core
