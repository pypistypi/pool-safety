#include "ui/AlarmCard.h"
#include "ui/Theme.h"

#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QClipboard>
#include <QGuiApplication>
#include <QDateTime>
#include <QStringList>

namespace ui {

AlarmCard::AlarmCard(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("AlarmCard"));
    setStyleSheet(QStringLiteral(
        "QFrame#AlarmCard { background-color: #3a1414; border: 2px solid #ff4545;"
        "                   border-radius: 8px; }"
        "QFrame#AlarmCard QLabel { background: transparent; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(8);

    // --- заголовок: что случилось и сколько прошло ------------------------
    auto *header = new QHBoxLayout;
    header->setSpacing(12);

    m_title = new QLabel(this);
    QFont titleFont = m_title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
    m_title->setWordWrap(true);
    header->addWidget(m_title, 1);

    m_elapsed = new QLabel(this);
    QFont elapsedFont = m_elapsed->font();
    elapsedFont.setPointSize(15);
    elapsedFont.setBold(true);
    m_elapsed->setFont(elapsedFont);
    m_elapsed->setStyleSheet(QStringLiteral("color: #ffd0d0;"));
    header->addWidget(m_elapsed);

    root->addLayout(header);

    // --- три колонки: где / что говорить / куда звонить -------------------
    auto *columns = new QGridLayout;
    columns->setHorizontalSpacing(16);
    columns->setVerticalSpacing(4);

    // колонка 1 — место
    auto *whereTitle = new QLabel(QStringLiteral("АДРЕС ДЛЯ СКОРОЙ"), this);
    whereTitle->setStyleSheet(QStringLiteral("color: #e6c9c9; font-weight: 600;"));
    columns->addWidget(whereTitle, 0, 0);

    m_address = new QLabel(this);
    QFont addressFont = m_address->font();
    addressFont.setPointSize(12);
    addressFont.setBold(true);
    m_address->setFont(addressFont);
    m_address->setWordWrap(true);
    m_address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    columns->addWidget(m_address, 1, 0);

    m_route = new QLabel(this);
    m_route->setWordWrap(true);
    m_route->setStyleSheet(QStringLiteral("color: #e6c9c9;"));
    columns->addWidget(m_route, 2, 0);

    m_action = new QLabel(this);
    m_action->setWordWrap(true);
    m_action->setStyleSheet(QStringLiteral("color: #ffe0a0;"));
    columns->addWidget(m_action, 3, 0);

    // Откуда взялась тревога. Для автоматической здесь перечислено, ПОЧЕМУ
    // система её подняла: оператор должен понимать, что именно она увидела,
    // иначе он не сможет ни довериться ей, ни осмысленно отменить.
    m_origin = new QLabel(this);
    m_origin->setWordWrap(true);
    m_origin->setStyleSheet(QStringLiteral("color: #d8b8b8;"));
    columns->addWidget(m_origin, 4, 0);

    // колонка 2 — готовый текст для звонка
    auto *briefTitle = new QLabel(QStringLiteral("ЧТО СКАЗАТЬ ДИСПЕТЧЕРУ"), this);
    briefTitle->setStyleSheet(QStringLiteral("color: #e6c9c9; font-weight: 600;"));
    columns->addWidget(briefTitle, 0, 1);

    m_brief = new QPlainTextEdit(this);
    m_brief->setReadOnly(true);
    m_brief->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background-color: #2a0f0f; border: 1px solid #7a3030;"
        "                 border-radius: 6px; color: #ffeaea; }"));
    m_brief->setMinimumHeight(120);
    columns->addWidget(m_brief, 1, 1, 3, 1);

    // колонка 3 — телефоны
    auto *callTitle = new QLabel(QStringLiteral("КУДА ЗВОНИТЬ"), this);
    callTitle->setStyleSheet(QStringLiteral("color: #e6c9c9; font-weight: 600;"));
    columns->addWidget(callTitle, 0, 2);

    auto *contactsHost = new QWidget(this);
    m_contactsLayout = new QVBoxLayout(contactsHost);
    m_contactsLayout->setContentsMargins(0, 0, 0, 0);
    m_contactsLayout->setSpacing(4);
    columns->addWidget(contactsHost, 1, 2, 3, 1);

    columns->setColumnStretch(0, 4);
    columns->setColumnStretch(1, 5);
    columns->setColumnStretch(2, 3);
    root->addLayout(columns);

    // --- уточнение и действия --------------------------------------------
    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(8);

    m_details = new QLineEdit(this);
    m_details->setPlaceholderText(
        QStringLiteral("Уточнение: возраст, пол, что видно — попадёт в текст для звонка"));
    m_details->setStyleSheet(QStringLiteral(
        "QLineEdit { background-color: #2a0f0f; border: 1px solid #7a3030;"
        "            border-radius: 6px; padding: 5px 8px; color: #ffeaea; }"));
    connect(m_details, &QLineEdit::textEdited, this, &AlarmCard::detailsEdited);
    bottom->addWidget(m_details, 1);

    m_copyBrief = new QPushButton(QStringLiteral("Скопировать для звонка"), this);
    m_copyBrief->setCursor(Qt::PointingHandCursor);
    connect(m_copyBrief, &QPushButton::clicked, this, [this] {
        copyToClipboard(m_brief->toPlainText(), QStringLiteral("Текст"));
    });
    bottom->addWidget(m_copyBrief);

    // «ПРИНЯЛ» крупнее прочих и стоит первой: это единственная кнопка,
    // выключающая сирену, и искать её в ряду одинаковых в такой момент нельзя.
    m_acknowledge = new QPushButton(QStringLiteral("ПРИНЯЛ — выключить сирену"), this);
    m_acknowledge->setObjectName(QStringLiteral("PrimaryButton"));
    m_acknowledge->setMinimumHeight(44);
    QFont ackFont = m_acknowledge->font();
    ackFont.setPointSize(11);
    ackFont.setBold(true);
    m_acknowledge->setFont(ackFont);
    m_false = new QPushButton(QStringLiteral("Ложная тревога"), this);
    m_confirm = new QPushButton(QStringLiteral("Подтверждаю — помощь вызвана"), this);
    m_confirm->setObjectName(QStringLiteral("AlarmButton"));
    m_close = new QPushButton(QStringLiteral("Инцидент закрыт"), this);

    for (QPushButton *button : {m_acknowledge, m_false, m_confirm, m_close}) {
        button->setCursor(Qt::PointingHandCursor);
        bottom->addWidget(button);
    }

    connect(m_acknowledge, &QPushButton::clicked, this, &AlarmCard::acknowledgeRequested);
    connect(m_false, &QPushButton::clicked, this, &AlarmCard::falseAlarmRequested);
    connect(m_confirm, &QPushButton::clicked, this, &AlarmCard::confirmRequested);
    connect(m_close, &QPushButton::clicked, this, &AlarmCard::closeIncidentRequested);

    root->addLayout(bottom);

    m_copyHint = new QLabel(this);
    m_copyHint->setStyleSheet(QStringLiteral("color: #9fe6a0;"));
    root->addWidget(m_copyHint);
}

void AlarmCard::rebuildContacts(const core::SiteInfo &site)
{
    // Список телефонов пересобирается заново: он берётся из настроек объекта и
    // может отличаться от того, что было при прошлой тревоге.
    while (QLayoutItem *item = m_contactsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    for (const core::EmergencyContact &contact : site.contacts) {
        const bool filled = !contact.phone.contains(QStringLiteral("НЕ ЗАПОЛНЕНО"))
                            && !contact.phone.trimmed().isEmpty();

        auto *button = new QPushButton(this);
        button->setCursor(filled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        button->setEnabled(filled);
        button->setText(filled
                            ? QStringLiteral("%1  —  %2").arg(contact.phone, contact.title)
                            : QStringLiteral("%1  —  телефон не задан").arg(contact.title));
        button->setToolTip(contact.note);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: #4a1a1a; border: 1px solid #a04040;"
            "              border-radius: 6px; padding: 6px 10px; text-align: left; }"
            "QPushButton:hover { background-color: #5c2020; }"
            "QPushButton:disabled { color: #a08080; border-color: #6a3a3a; }"));

        const QString phone = contact.phone;
        connect(button, &QPushButton::clicked, this, [this, phone] {
            copyToClipboard(phone, QStringLiteral("Номер"));
        });

        m_contactsLayout->addWidget(button);
    }
    m_contactsLayout->addStretch(1);
}

void AlarmCard::copyToClipboard(const QString &text, const QString &what)
{
    if (text.trimmed().isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    m_copyHint->setText(QStringLiteral("%1 скопирован в буфер обмена").arg(what));
}

void AlarmCard::applyState(core::AlarmState state, const core::AlarmEvent &event,
                           const core::SiteInfo &site)
{
    if (state == core::AlarmState::Idle) {
        hide();
        m_details->clear();
        m_copyHint->clear();
        return;
    }

    const bool sameIncident = m_event.raisedAt == event.raisedAt;
    m_event = event;
    m_site = site;

    if (!sameIncident) {
        // Новое происшествие — прежние уточнения и подсказки к нему не
        // относятся.
        m_details->clear();
        m_copyHint->clear();
        rebuildContacts(site);
    }

    const QString zone = event.zoneLabel.isEmpty()
                             ? QStringLiteral("зона бассейна")
                             : event.zoneLabel;

    QString stateWord;
    switch (state) {
    case core::AlarmState::Raised:       stateWord = QStringLiteral("ТРЕВОГА"); break;
    case core::AlarmState::Acknowledged: stateWord = QStringLiteral("ТРЕВОГА ПРИНЯТА"); break;
    case core::AlarmState::Confirmed:    stateWord = QStringLiteral("ПОМОЩЬ ВЫЗВАНА"); break;
    case core::AlarmState::Idle:         break;
    }

    m_title->setText(QStringLiteral("%1 · %2 · %3")
                         .arg(stateWord,
                              core::circumstanceText(event.circumstance).toUpper(),
                              zone));

    if (site.isAddressReady()) {
        m_address->setText(site.address);
        m_address->setStyleSheet(QStringLiteral("color: #ffffff;"));
    } else {
        // Молча показать пустоту нельзя: оператор решит, что адрес программе
        // известен, и будет ждать подсказки, которой нет.
        m_address->setText(QStringLiteral(
            "АДРЕС НЕ ЗАДАН — назовите его сами!\n"
            "Заполните файл config/site.json рядом с программой."));
        m_address->setStyleSheet(QStringLiteral("color: #ffd23f;"));
    }

    QStringList route;
    if (!site.landmark.trimmed().isEmpty())
        route << QStringLiteral("Заезд: %1").arg(site.landmark);
    if (!site.entrance.trimmed().isEmpty())
        route << QStringLiteral("Проход: %1").arg(site.entrance);
    m_route->setText(route.join(QLatin1Char('\n')));

    m_action->setText(QStringLiteral("Первым делом: %1")
                          .arg(core::circumstanceAction(event.circumstance)));

    if (event.origin == core::AlarmOrigin::Automatic) {
        m_origin->setText(QStringLiteral("Подняла система. Что увидела: %1")
                              .arg(event.details.isEmpty()
                                       ? QStringLiteral("— (причина не записана)")
                                       : event.details));
    } else {
        m_origin->setText(QStringLiteral("Объявил оператор вручную."));
    }

    m_brief->setPlainText(core::buildCallBrief(event, site));

    // Показываются только уместные сейчас действия: лишние кнопки в такой
    // момент — источник неверных нажатий.
    m_acknowledge->setVisible(state == core::AlarmState::Raised);
    m_false->setVisible(state == core::AlarmState::Raised
                        || state == core::AlarmState::Acknowledged);
    m_confirm->setVisible(state == core::AlarmState::Raised
                          || state == core::AlarmState::Acknowledged);
    m_close->setVisible(state == core::AlarmState::Confirmed);

    // Мигает только непринятая тревога. После «ПРИНЯЛ» оператор уже занят
    // делом, и мигание превращается из способа привлечь внимание в помеху.
    m_blinking = state == core::AlarmState::Raised;
    if (!m_blinking) {
        setStyleSheet(QStringLiteral(
            "QFrame#AlarmCard { background-color: #3a1414; border: 2px solid #ff4545;"
            "                   border-radius: 8px; }"
            "QFrame#AlarmCard QLabel { background: transparent; }"));
    }

    tick();
    show();
}

void AlarmCard::setBlinkPhase(bool bright)
{
    if (!m_blinking)
        return;
    // Меняется только рамка и фон: содержимое мигать не должно — его читают.
    setStyleSheet(bright
        ? QStringLiteral(
              "QFrame#AlarmCard { background-color: #5a1414; border: 3px solid #ff4545;"
              "                   border-radius: 8px; }"
              "QFrame#AlarmCard QLabel { background: transparent; }")
        : QStringLiteral(
              "QFrame#AlarmCard { background-color: #2e1010; border: 3px solid #8a2020;"
              "                   border-radius: 8px; }"
              "QFrame#AlarmCard QLabel { background: transparent; }"));
}

void AlarmCard::tick()
{
    if (!isVisible() || !m_event.raisedAt.isValid())
        return;

    // Сколько прошло с момента объявления. Врачи спрашивают об этом в первую
    // очередь: от времени без дыхания зависит, что делать по приезде.
    const qint64 seconds = m_event.raisedAt.secsTo(QDateTime::currentDateTime());
    m_elapsed->setText(QStringLiteral("прошло %1:%2")
                           .arg(seconds / 60, 2, 10, QLatin1Char('0'))
                           .arg(seconds % 60, 2, 10, QLatin1Char('0')));
}

} // namespace ui
