#pragma once

// ---------------------------------------------------------------------------
//  Карточка тревоги — всё, что нужно оператору, пока он звонит.
//
//  ЧТО БЫЛО НЕ ТАК. Прежняя полоса тревоги сообщала только «ТРЕВОГА» и
//  предлагала кнопки. Оператор оставался один на один с телефоном: адрес надо
//  вспоминать, обстоятельства формулировать, порядок действий — знать наизусть.
//  В происшествии, где счёт идёт на секунды, это никуда не годится.
//
//  ЧТО ЗДЕСЬ ТЕПЕРЬ, СВЕРХУ ВНИЗ И СЛЕВА НАПРАВО — в том порядке, в каком это
//  спрашивают в трубке:
//
//    1. ЧТО СЛУЧИЛОСЬ — обстоятельство крупно, в заголовке.
//    2. ГДЕ — адрес объекта, ориентир для въезда, как пройти внутри.
//    3. ЧТО ГОВОРИТЬ — готовый текст целиком, его можно просто читать вслух;
//       кнопка кладёт его в буфер обмена, чтобы переслать в мессенджер.
//    4. КУДА ЗВОНИТЬ — телефоны с пометкой, кому и когда; нажатие копирует
//       номер.
//    5. ЧТО ДЕЛАТЬ РУКАМИ — первые действия по этому обстоятельству.
//    6. СКОЛЬКО ПРОШЛО с момента объявления — врачи спросят.
//
//  Карточка не модальная и не перекрывает панели: смотреть на воду оператор
//  должен продолжать.
// ---------------------------------------------------------------------------

#include "core/AlarmTypes.h"
#include "core/SiteInfo.h"

#include <QFrame>

class QLabel;
class QPushButton;
class QPlainTextEdit;
class QLineEdit;
class QVBoxLayout;

namespace ui {

class AlarmCard : public QFrame
{
    Q_OBJECT

public:
    explicit AlarmCard(QWidget *parent = nullptr);

    /// Привести карточку в соответствие состоянию тревоги.
    void applyState(core::AlarmState state, const core::AlarmEvent &event,
                    const core::SiteInfo &site);

    /// Обновить счётчик «прошло с момента объявления». Вызывается общим
    /// таймером окна раз в секунду.
    void tick();

    /// Фаза мигания рамки. Мигает только непринятая тревога: как только
    /// оператор нажал «ПРИНЯЛ», дёргать его уже незачем — он занят делом.
    void setBlinkPhase(bool bright);

signals:
    void acknowledgeRequested();
    void falseAlarmRequested();
    void confirmRequested();
    void closeIncidentRequested();

    /// Оператор дописал уточнение — оно должно попасть в событие и в текст
    /// для звонка.
    void detailsEdited(const QString &details);

private:
    void rebuildContacts(const core::SiteInfo &site);
    void copyToClipboard(const QString &text, const QString &what);

    core::AlarmEvent m_event;
    core::SiteInfo m_site;

    QLabel *m_title = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_address = nullptr;
    QLabel *m_route = nullptr;
    QLabel *m_action = nullptr;
    QLabel *m_origin = nullptr;
    QLabel *m_copyHint = nullptr;
    QPlainTextEdit *m_brief = nullptr;
    QLineEdit *m_details = nullptr;
    QVBoxLayout *m_contactsLayout = nullptr;

    QPushButton *m_copyBrief = nullptr;
    QPushButton *m_acknowledge = nullptr;
    QPushButton *m_false = nullptr;
    QPushButton *m_confirm = nullptr;
    QPushButton *m_close = nullptr;

    bool m_blinking = false;
};

} // namespace ui
