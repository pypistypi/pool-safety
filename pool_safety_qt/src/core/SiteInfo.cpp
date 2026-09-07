#include "core/SiteInfo.h"

#include "core/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>

namespace core {

namespace {

// Метка незаполненного поля. Видна и в файле, и на экране, и её невозможно
// принять за настоящий адрес.
const QString kUnset = QStringLiteral("НЕ ЗАПОЛНЕНО");

QString valueOr(const QJsonObject &object, const char *key, const QString &fallback)
{
    const QString value = object.value(QLatin1String(key)).toString().trimmed();
    return value.isEmpty() ? fallback : value;
}

} // namespace

QString SiteInfo::defaultPath()
{
    return paths::siteFile();
}

bool SiteInfo::isAddressReady() const
{
    return !address.trimmed().isEmpty() && !address.contains(kUnset);
}

SiteInfo SiteInfo::defaults()
{
    SiteInfo info;
    info.objectName = kUnset + QStringLiteral(" — название объекта и бассейна");
    info.address = kUnset + QStringLiteral(" — полный адрес для скорой помощи");
    info.landmark = kUnset + QStringLiteral(" — ориентир, где заезд на территорию");
    info.entrance = kUnset + QStringLiteral(" — корпус, этаж, как пройти к бассейну");
    info.responsible = kUnset + QStringLiteral(" — ответственный за объект");

    // Номера экстренных служб — общероссийские, их менять не нужно.
    // Внутренние телефоны объекта заполняет администратор.
    info.contacts = {
        {QStringLiteral("Единая служба спасения"), QStringLiteral("112"),
         QStringLiteral("звонить первым делом")},
        {QStringLiteral("Скорая медицинская помощь"), QStringLiteral("103"),
         QStringLiteral("если 112 занят")},
        {QStringLiteral("Дежурный спасатель бассейна"), kUnset,
         QStringLiteral("вытащить из воды немедленно")},
        {QStringLiteral("Старший смены"), kUnset,
         QStringLiteral("сообщить после вызова помощи")},
    };
    return info;
}

SiteInfo SiteInfo::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // Файла ещё нет — создаём заготовку, чтобы администратору было что
        // заполнять. Искать в документации, какие поля бывают, не придётся.
        const SiteInfo info = defaults();
        info.save(path);
        return info;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return defaults();

    const QJsonObject root = document.object();
    const SiteInfo fallback = defaults();

    SiteInfo info;
    info.objectName  = valueOr(root, "object_name", fallback.objectName);
    info.address     = valueOr(root, "address", fallback.address);
    info.landmark    = valueOr(root, "landmark", fallback.landmark);
    info.entrance    = valueOr(root, "entrance", fallback.entrance);
    info.responsible = valueOr(root, "responsible", fallback.responsible);

    const QJsonArray contacts = root.value(QLatin1String("contacts")).toArray();
    for (const QJsonValue &value : contacts) {
        const QJsonObject item = value.toObject();
        EmergencyContact contact;
        contact.title = item.value(QLatin1String("title")).toString();
        contact.phone = item.value(QLatin1String("phone")).toString();
        contact.note  = item.value(QLatin1String("note")).toString();
        if (!contact.title.isEmpty() && !contact.phone.isEmpty())
            info.contacts.append(contact);
    }
    if (info.contacts.isEmpty())
        info.contacts = fallback.contacts;

    return info;
}

bool SiteInfo::save(const QString &path) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonArray contactsArray;
    for (const EmergencyContact &contact : contacts) {
        QJsonObject item;
        item.insert(QStringLiteral("title"), contact.title);
        item.insert(QStringLiteral("phone"), contact.phone);
        item.insert(QStringLiteral("note"), contact.note);
        contactsArray.append(item);
    }

    QJsonObject root;
    root.insert(QStringLiteral("_комментарий"), QStringLiteral(
        "Эти сведения программа показывает оператору при тревоге и готовит "
        "для звонка в экстренные службы. Заполните все поля, помеченные "
        "НЕ ЗАПОЛНЕНО, до ввода системы в работу."));
    root.insert(QStringLiteral("object_name"), objectName);
    root.insert(QStringLiteral("address"), address);
    root.insert(QStringLiteral("landmark"), landmark);
    root.insert(QStringLiteral("entrance"), entrance);
    root.insert(QStringLiteral("responsible"), responsible);
    root.insert(QStringLiteral("contacts"), contactsArray);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace core
