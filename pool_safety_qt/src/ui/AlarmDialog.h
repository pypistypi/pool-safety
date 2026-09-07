#pragma once

// ---------------------------------------------------------------------------
//  Окно объявления тревоги: «что именно случилось».
//
//  ПОЧЕМУ СПРАШИВАЕМ, А НЕ ОБЪЯВЛЯЕМ СРАЗУ. Прежде тревога объявлялась одним
//  нажатием и не несла никаких сведений — оператор оставался с красной
//  полосой на экране и без единого слова для диспетчера. Между тем первое, что
//  спросят в трубке, — что случилось. Ответ на этот вопрос должен появиться
//  тогда же, когда и тревога, а не сочиняться на ходу.
//
//  ЦЕНА ВОПРОСА — ОДНО НАЖАТИЕ. Обстоятельства выбираются крупными кнопками,
//  и нажатие сразу объявляет тревогу: ни «ОК», ни подтверждений. Уточнить
//  словами можно потом, в карточке — когда помощь уже вызвана.
// ---------------------------------------------------------------------------

#include "core/AlarmTypes.h"

#include <QDialog>

class QVBoxLayout;

namespace ui {

class AlarmDialog : public QDialog
{
    Q_OBJECT

public:
    AlarmDialog(const QString &zoneLabel, QWidget *parent = nullptr);

    /// Что выбрал оператор. Осмысленно только при QDialog::Accepted.
    core::AlarmCircumstance circumstance() const { return m_circumstance; }

private:
    void addChoice(QVBoxLayout *layout, core::AlarmCircumstance circumstance,
                   const QString &hint);

    core::AlarmCircumstance m_circumstance = core::AlarmCircumstance::Drowning;
};

} // namespace ui
