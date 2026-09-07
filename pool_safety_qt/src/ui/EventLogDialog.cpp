#include "ui/EventLogDialog.h"
#include "ui/Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QCheckBox>
#include <QDesktopServices>
#include <QUrl>

namespace ui {

namespace {

QColor colorFor(core::EventLog::Kind kind)
{
    switch (kind) {
    case core::EventLog::Kind::Alarm:     return theme::alarm();
    case core::EventLog::Kind::Attention: return theme::attention();
    case core::EventLog::Kind::Presence:  return theme::presence();
    case core::EventLog::Kind::Operator:  return theme::calm();
    case core::EventLog::Kind::Info:      break;
    }
    return theme::textMuted();
}

} // namespace

EventLogDialog::EventLogDialog(core::EventLog *log, QWidget *parent)
    : QDialog(parent)
    , m_log(log)
{
    setWindowTitle(QStringLiteral("Журнал событий"));
    setMinimumSize(900, 520);

    auto *root = new QVBoxLayout(this);

    auto *path = new QLabel(
        QStringLiteral("Полная запись ведётся в файл: %1").arg(log->currentFilePath()),
        this);
    path->setWordWrap(true);
    path->setStyleSheet(QStringLiteral("color: #939fb0;"));
    root->addWidget(path);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Время"),
                                        QStringLiteral("Событие"),
                                        QStringLiteral("Панель"),
                                        QStringLiteral("Что произошло")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    root->addWidget(m_table, 1);

    for (const core::EventLog::Entry &entry : log->recent())
        addRow(entry);
    m_table->scrollToBottom();

    auto *bottom = new QHBoxLayout;

    m_onlyImportant = new QCheckBox(
        QStringLiteral("Показывать только внимание и тревоги"), this);
    connect(m_onlyImportant, &QCheckBox::toggled, this, [this](bool only) {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const int kind = m_table->item(row, 1)->data(Qt::UserRole).toInt();
            const bool important = kind == int(core::EventLog::Kind::Alarm)
                                   || kind == int(core::EventLog::Kind::Attention);
            m_table->setRowHidden(row, only && !important);
        }
    });
    bottom->addWidget(m_onlyImportant);

    bottom->addStretch(1);

    auto *openFolder = new QPushButton(QStringLiteral("Открыть папку с журналами"), this);
    connect(openFolder, &QPushButton::clicked, this, &EventLogDialog::openLogFolder);
    bottom->addWidget(openFolder);

    auto *close = new QPushButton(QStringLiteral("Закрыть"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    bottom->addWidget(close);

    root->addLayout(bottom);

    connect(log, &core::EventLog::entryAdded, this, &EventLogDialog::appendEntry);
}

void EventLogDialog::addRow(const core::EventLog::Entry &entry)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto *time = new QTableWidgetItem(entry.at.toString(QStringLiteral("HH:mm:ss")));
    auto *kind = new QTableWidgetItem(entry.kindText());
    kind->setData(Qt::UserRole, int(entry.kind));
    kind->setForeground(colorFor(entry.kind));
    auto *panel = new QTableWidgetItem(
        entry.panelId >= 0 ? QString::number(entry.panelId + 1) : QStringLiteral("—"));
    auto *text = new QTableWidgetItem(entry.text);

    m_table->setItem(row, 0, time);
    m_table->setItem(row, 1, kind);
    m_table->setItem(row, 2, panel);
    m_table->setItem(row, 3, text);
}

void EventLogDialog::appendEntry(const core::EventLog::Entry &entry)
{
    addRow(entry);
    if (m_onlyImportant && m_onlyImportant->isChecked()) {
        const bool important = entry.kind == core::EventLog::Kind::Alarm
                               || entry.kind == core::EventLog::Kind::Attention;
        m_table->setRowHidden(m_table->rowCount() - 1, !important);
    }
    m_table->scrollToBottom();
}

void EventLogDialog::openLogFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(core::EventLog::directory()));
}

} // namespace ui
