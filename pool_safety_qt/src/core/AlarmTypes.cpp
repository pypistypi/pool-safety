#include "core/AlarmTypes.h"
#include "core/SiteInfo.h"

#include <QStringList>

namespace core {

QString circumstanceText(AlarmCircumstance circumstance)
{
    switch (circumstance) {
    case AlarmCircumstance::UnconsciousInWater:
        return QStringLiteral("человек без сознания в воде");
    case AlarmCircumstance::Drowning:
        return QStringLiteral("человек тонет");
    case AlarmCircumstance::UnconsciousPoolside:
        return QStringLiteral("человек без сознания на бортике");
    case AlarmCircumstance::Injury:
        return QStringLiteral("травма, есть пострадавший");
    case AlarmCircumstance::ChildUnattended:
        return QStringLiteral("ребёнок в воде без взрослого");
    case AlarmCircumstance::Other:
        return QStringLiteral("происшествие в зоне бассейна");
    }
    return QStringLiteral("происшествие в зоне бассейна");
}

QString circumstanceAction(AlarmCircumstance circumstance)
{
    switch (circumstance) {
    case AlarmCircumstance::UnconsciousInWater:
    case AlarmCircumstance::Drowning:
        return QStringLiteral(
            "Извлечь из воды. Вызвать спасателя и скорую. "
            "Проверить дыхание, при отсутствии — начать реанимацию.");
    case AlarmCircumstance::UnconsciousPoolside:
        return QStringLiteral(
            "Не перемещать без нужды. Проверить дыхание и пульс. "
            "Вызвать скорую, обеспечить проход бригаде.");
    case AlarmCircumstance::Injury:
        return QStringLiteral(
            "Остановить кровотечение. Вызвать скорую. "
            "Не оставлять пострадавшего одного.");
    case AlarmCircumstance::ChildUnattended:
        return QStringLiteral(
            "Немедленно направить спасателя к ребёнку. "
            "Найти сопровождающего взрослого.");
    case AlarmCircumstance::Other:
        return QStringLiteral(
            "Направить спасателя к месту. Уточнить обстановку, "
            "при угрозе жизни вызвать скорую.");
    }
    return QString();
}

QString alarmStateText(AlarmState state)
{
    switch (state) {
    case AlarmState::Idle:         return QStringLiteral("норма");
    case AlarmState::Raised:       return QStringLiteral("ТРЕВОГА — требуется реакция");
    case AlarmState::Acknowledged: return QStringLiteral("тревога принята, решение не принято");
    case AlarmState::Confirmed:    return QStringLiteral("тревога подтверждена — вызов помощи");
    }
    return QStringLiteral("норма");
}

QString buildCallBrief(const AlarmEvent &event, const SiteInfo &site)
{
    QStringList lines;

    lines << QStringLiteral("ЧТО СЛУЧИЛОСЬ: %1.").arg(circumstanceText(event.circumstance));

    if (!event.details.trimmed().isEmpty())
        lines << QStringLiteral("Уточнение: %1").arg(event.details.trimmed());

    lines << QStringLiteral("ГДЕ: %1").arg(
        site.objectName.trimmed().isEmpty() ? QStringLiteral("бассейн") : site.objectName);

    if (!event.zoneLabel.trimmed().isEmpty())
        lines << QStringLiteral("Участок: %1").arg(event.zoneLabel);

    // Адрес — самое важное в звонке. Если он не заполнен, оператор должен
    // увидеть не пустую строку, а прямое указание, что делать.
    if (site.isAddressReady())
        lines << QStringLiteral("АДРЕС: %1").arg(site.address);
    else
        lines << QStringLiteral(
            "АДРЕС НЕ ЗАДАН В ПРОГРАММЕ — назовите адрес объекта сами "
            "и заполните config/site.json после происшествия!");

    if (!site.landmark.trimmed().isEmpty())
        lines << QStringLiteral("Как проехать: %1").arg(site.landmark);
    if (!site.entrance.trimmed().isEmpty())
        lines << QStringLiteral("Куда идти: %1").arg(site.entrance);

    lines << QStringLiteral("Время происшествия: %1").arg(
        event.raisedAt.isValid()
            ? event.raisedAt.toString(QStringLiteral("dd.MM.yyyy HH:mm:ss"))
            : QStringLiteral("—"));

    if (event.peopleInZone > 0)
        lines << QStringLiteral("Людей в зоне на момент тревоги: %1").arg(event.peopleInZone);

    lines << QStringLiteral("Тревога %1.").arg(
        event.origin == AlarmOrigin::Manual
            ? QStringLiteral("объявлена оператором")
            : QStringLiteral("поднята системой видеонаблюдения"));

    if (!site.responsible.trimmed().isEmpty())
        lines << QStringLiteral("Ответственный на объекте: %1").arg(site.responsible);

    return lines.join(QLatin1Char('\n'));
}

} // namespace core
