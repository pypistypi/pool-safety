#pragma once

// ---------------------------------------------------------------------------
//  Журнал событий: что система видела и что решила.
//
//  ЗАЧЕМ ОН ПОЯВИЛСЯ. При проверке на живом человеке выяснилась простая вещь:
//  все измерения и вердикты жили только на экране. Проверяющий лежал на полу,
//  изображая пострадавшего, — и не мог прочитать ничего, потому что смотреть
//  было некому. Наблюдение, результат которого виден только тому, кто сидит
//  перед монитором, проверить невозможно.
//
//  Поэтому всё, что система решает, записывается в файл: `logs/ГГГГ-ММ-ДД.log`
//  рядом с программой. Файл обычный текстовый, читается блокнотом, и по нему
//  можно разобрать любую смену задним числом — когда появились люди, когда
//  подсветилась панель, почему поднялась тревога и как оператор её обработал.
//
//  ПЕРСОНАЛЬНЫХ ДАННЫХ В ЖУРНАЛЕ НЕТ. Записываются положения тела, время и
//  номера панелей. Ни лиц, ни имён, ни изображений — по журналу нельзя
//  установить, кто именно был в зоне.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QVector>

class QFile;
class QTextStream;

namespace core {

class EventLog : public QObject
{
    Q_OBJECT

public:
    enum class Kind {
        Info,        ///< запуск, смена источника, состояние моделей
        Presence,    ///< сигнал 1: люди в зоне
        Attention,   ///< признаки есть, до тревоги не дотягивают
        Alarm,       ///< сигнал 2
        Operator     ///< действие оператора
    };

    struct Entry {
        QDateTime at;
        Kind kind = Kind::Info;
        int panelId = -1;
        QString text;

        QString kindText() const;
    };

    explicit EventLog(QObject *parent = nullptr);
    ~EventLog() override;

    void write(Kind kind, const QString &text, int panelId = -1);

    /// Последние записи — для окна журнала. Хранится ограниченное число:
    /// смотреть глубже нужно уже в файле.
    const QVector<Entry> &recent() const { return m_recent; }

    /// Файл, в который идёт запись сейчас.
    QString currentFilePath() const { return m_path; }

    /// Папка с журналами.
    static QString directory();

signals:
    void entryAdded(const core::EventLog::Entry &entry);

private:
    void openForToday();

    QFile *m_file = nullptr;
    QString m_path;
    QDate m_day;
    QVector<Entry> m_recent;

    /// Сколько записей держать в памяти. Двухсот хватает на несколько часов
    /// спокойной смены и на любое происшествие целиком.
    static constexpr int kRecentLimit = 200;
};

} // namespace core

Q_DECLARE_METATYPE(core::EventLog::Entry)
