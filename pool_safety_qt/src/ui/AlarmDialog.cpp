#include "ui/AlarmDialog.h"
#include "ui/Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace ui {

AlarmDialog::AlarmDialog(const QString &zoneLabel, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Объявление тревоги"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("Что случилось?"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(15);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *zone = new QLabel(QStringLiteral("Зона: %1").arg(zoneLabel), this);
    zone->setObjectName(QStringLiteral("Subtitle"));
    layout->addWidget(zone);

    auto *hint = new QLabel(
        QStringLiteral("Нажмите — тревога объявится сразу, "
                       "а выбранное вы продиктуете диспетчеру."), this);
    hint->setObjectName(QStringLiteral("Subtitle"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addSpacing(6);

    // Порядок не алфавитный, а по тяжести: наверху то, где счёт идёт на
    // секунды. В спешке нажимают по верхним кнопкам.
    addChoice(layout, core::AlarmCircumstance::UnconsciousInWater,
              QStringLiteral("не двигается, лицо в воде"));
    addChoice(layout, core::AlarmCircumstance::Drowning,
              QStringLiteral("барахтается, уходит под воду, зовёт на помощь"));
    addChoice(layout, core::AlarmCircumstance::UnconsciousPoolside,
              QStringLiteral("упал, не реагирует"));
    addChoice(layout, core::AlarmCircumstance::ChildUnattended,
              QStringLiteral("рядом нет взрослого"));
    addChoice(layout, core::AlarmCircumstance::Injury,
              QStringLiteral("порез, ушиб, кровотечение"));
    addChoice(layout, core::AlarmCircumstance::Other,
              QStringLiteral("уточните словами в карточке тревоги"));

    layout->addSpacing(6);

    auto *cancel = new QPushButton(QStringLiteral("Отмена"), this);
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    auto *bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(cancel);
    layout->addLayout(bottom);

    setMinimumWidth(460);
}

void AlarmDialog::addChoice(QVBoxLayout *layout, core::AlarmCircumstance circumstance,
                            const QString &hint)
{
    auto *button = new QPushButton(this);
    button->setObjectName(QStringLiteral("CircumstanceButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setText(QStringLiteral("%1\n%2")
                        .arg(core::circumstanceText(circumstance), hint));
    button->setMinimumHeight(52);

    connect(button, &QPushButton::clicked, this, [this, circumstance] {
        m_circumstance = circumstance;
        accept();
    });

    layout->addWidget(button);
}

} // namespace ui
