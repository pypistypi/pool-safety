#pragma once

// ---------------------------------------------------------------------------
//  Окно журнала событий.
//
//  ЗАЧЕМ ОНО. При проверке на живом человеке выяснилось, что все решения
//  системы видны только тому, кто смотрит на экран. Проверяющий лежал на полу,
//  изображая пострадавшего, и прочитать не мог ничего. Теперь всё пишется в
//  файл, а это окно показывает записи, не выходя из программы: что видела
//  система, когда подсветила панель, почему подняла тревогу и что сделал
//  оператор.
//
//  Окно не модальное: наблюдение при открытом журнале не прерывается.
// ---------------------------------------------------------------------------

#include "core/EventLog.h"

#include <QDialog>

class QTableWidget;
class QCheckBox;

namespace ui {

class EventLogDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EventLogDialog(core::EventLog *log, QWidget *parent = nullptr);

private slots:
    void appendEntry(const core::EventLog::Entry &entry);
    void openLogFolder();

private:
    void addRow(const core::EventLog::Entry &entry);

    core::EventLog *m_log = nullptr;
    QTableWidget *m_table = nullptr;
    QCheckBox *m_onlyImportant = nullptr;
};

} // namespace ui
