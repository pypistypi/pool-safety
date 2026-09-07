#include "core/EventLog.h"

#include "core/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace core {

QString EventLog::Entry::kindText() const
{
    switch (kind) {
    case Kind::Info:      return QStringLiteral("сведения");
    case Kind::Presence:  return QStringLiteral("присутствие");
    case Kind::Attention: return QStringLiteral("ВНИМАНИЕ");
    case Kind::Alarm:     return QStringLiteral("ТРЕВОГА");
    case Kind::Operator:  return QStringLiteral("оператор");
    }
    return QString();
}

QString EventLog::directory()
{
    return paths::logsDir();
}

EventLog::EventLog(QObject *parent)
    : QObject(parent)
{
    openForToday();
}

EventLog::~EventLog()
{
    if (m_file) {
        m_file->close();
        delete m_file;
    }
}

void EventLog::openForToday()
{
    const QDate today = QDate::currentDate();
    if (m_file && m_day == today)
        return;

    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }

    QDir().mkpath(directory());
    m_day = today;
    m_path = QDir(directory()).filePath(today.toString(QStringLiteral("yyyy-MM-dd"))
                                        + QStringLiteral(".log"));

    // Дописываем, а не перезаписываем: за смену программу могут перезапустить,
    // и потерять при этом первую половину дня недопустимо.
    auto *file = new QFile(m_path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        delete file;
        return;
    }
    m_file = file;

    QTextStream stream(m_file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << QStringLiteral("\n=== запуск программы %1 ===\n")
                  .arg(QDateTime::currentDateTime().toString(
                      QStringLiteral("dd.MM.yyyy HH:mm:ss")));
    stream.flush();
}

void EventLog::write(Kind kind, const QString &text, int panelId)
{
    openForToday();

    Entry entry;
    entry.at = QDateTime::currentDateTime();
    entry.kind = kind;
    entry.panelId = panelId;
    entry.text = text;

    m_recent.append(entry);
    if (m_recent.size() > kRecentLimit)
        m_recent.remove(0, m_recent.size() - kRecentLimit);

    if (m_file) {
        QTextStream stream(m_file);
        stream.setEncoding(QStringConverter::Utf8);
        stream << entry.at.toString(QStringLiteral("HH:mm:ss")) << QLatin1Char('\t')
               << entry.kindText() << QLatin1Char('\t')
               << (panelId >= 0 ? QStringLiteral("панель %1").arg(panelId + 1)
                                : QStringLiteral("—"))
               << QLatin1Char('\t') << text << QLatin1Char('\n');
        // Сбрасываем сразу: журнал нужен именно тогда, когда программа
        // завершилась не по-хорошему, и держать последние строки в буфере —
        // значит потерять ровно то, ради чего журнал заводился.
        stream.flush();
    }

    emit entryAdded(entry);
}

} // namespace core
