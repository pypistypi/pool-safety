#include "core/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QUuid>

namespace core {

namespace paths {

namespace {

/// Можно ли писать в этот каталог. Проверяем делом, а не правами: права умеют
/// выглядеть достаточными и всё равно не давать записи — на сетевом ресурсе,
/// на носителе с защитой, при перенаправлении папок.
bool isWritable(const QString &directory)
{
    QDir dir(directory);
    if (!dir.exists() && !QDir().mkpath(directory))
        return false;

    const QString probe = dir.filePath(QStringLiteral(".запись-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::Id128)));
    QFile file(probe);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.close();
    file.remove();
    return true;
}

struct Location {
    QString directory;
    bool portable = false;
};

const Location &resolve()
{
    // Считается один раз: каталог за время работы программы не меняется, а
    // проверка записи — обращение к диску.
    static const Location location = [] {
        const QString nearProgram = QCoreApplication::applicationDirPath();

        // Рядом с программой — если туда можно писать. Это обычный случай для
        // распакованного архива и для наладки на объекте.
        if (isWritable(nearProgram))
            return Location{nearProgram, true};

        // Иначе программа установлена в защищённую папку. Данные — в общий
        // каталог ProgramData: они принадлежат объекту, а не пользователю.
        // Оператор, войдя под другой учётной записью, должен видеть те же
        // настройки и тот же журнал происшествий — иначе адрес объекта
        // придётся заполнять каждой смене заново.
        QString shared = qEnvironmentVariable("ProgramData");
        if (shared.isEmpty() || !QDir(shared).exists()) {
            // Не Windows или переменная потеряна — откатываемся на
            // пользовательский каталог. Хуже, но работает.
            shared = QStandardPaths::writableLocation(
                QStandardPaths::AppDataLocation);
        }
        if (shared.isEmpty())
            shared = QDir::homePath();

        const QString directory = QDir(shared).filePath(QStringLiteral("PoolSafety"));
        QDir().mkpath(directory);
        return Location{directory, false};
    }();

    return location;
}

} // namespace

QString dataDir()
{
    return resolve().directory;
}

bool isPortable()
{
    return resolve().portable;
}

QString settingsFile()
{
    return QDir(dataDir()).filePath(QStringLiteral("config/settings.json"));
}

QString siteFile()
{
    return QDir(dataDir()).filePath(QStringLiteral("config/site.json"));
}

QString logsDir()
{
    return QDir(dataDir()).filePath(QStringLiteral("logs"));
}

} // namespace paths

} // namespace core
