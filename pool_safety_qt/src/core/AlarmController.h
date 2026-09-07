#pragma once

// ---------------------------------------------------------------------------
//  Обработка обоих сигналов оператору.
//
//  Класс намеренно НИЧЕГО не знает ни об интерфейсе, ни о видео. Он держит
//  только состояние и правила перехода между ними, а наружу отдаёт сигналы.
//  Благодаря этому логику тревоги можно проверить без единого окна, а на
//  следующем этапе к ней подключится распознавание — не изменив здесь ни
//  строчки.
// ---------------------------------------------------------------------------

#include "core/AlarmTypes.h"

#include <QObject>
#include <QHash>

namespace core {

class AlarmController : public QObject
{
    Q_OBJECT

public:
    explicit AlarmController(QObject *parent = nullptr);

    AlarmState state() const { return m_state; }
    const AlarmEvent &currentEvent() const { return m_current; }

    /// Сколько людей сейчас в зоне.
    ///
    /// БЕРЁТСЯ МАКСИМУМ ПО ПАНЕЛЯМ, А НЕ СУММА. Все четыре камеры смотрят на
    /// один и тот же бассейн с разных сторон: человек, стоящий у воды, виден
    /// сразу с нескольких, и сложение превращало его в четверых. Оператор
    /// читал «в зоне людей: 8» там, где их двое, — и переставал верить
    /// счётчику вовсе.
    ///
    /// Максимум занижает счёт, когда люди в разных концах чаши и каждая
    /// камера видит своих. Это осознанный выбор: завышенный счёт врёт всегда,
    /// заниженный — только в редком случае, и ни одно правило от него не
    /// зависит. Правила работают по дорожкам людей внутри своей панели.
    int peopleInZone() const;

    /// Сколько людей на конкретной панели.
    int peopleOnPanel(int panelId) const;

public slots:
    // --- сигнал 1: присутствие -------------------------------------------
    /// Сообщить, сколько людей видно на панели. Вызывается модулем
    /// распознавания; на этом этапе — только вручную из проверок.
    void reportPresence(int panelId, int peopleCount);

    /// Забыть панель (сменили источник, панель опустела).
    void forgetPanel(int panelId);

    // --- сигнал 2: тревога ------------------------------------------------
    /// Объявить тревогу. Повторный вызов при уже объявленной тревоге
    /// игнорируется: вторая тревога поверх первой сбила бы оператора.
    void raiseAlarm(const AlarmEvent &event);

    /// Кнопка оператора «Принял». Только фиксирует, что сигнал замечен;
    /// решение принимается отдельно.
    void acknowledge();

    /// Решение оператора: тревога ложная. Возврат в спокойное состояние.
    void dismissAsFalse();

    /// Решение оператора: тревога настоящая. Начинается вызов помощи.
    void confirm();

    /// Закрыть подтверждённую тревогу (помощь оказана).
    void closeIncident();

    /// Дописать уточнение к идущей тревоге. Оператор вспоминает подробности
    /// уже во время звонка — текст для диспетчера должен успевать за ним.
    void updateDetails(const QString &details);

signals:
    // --- сигнал 1 ---------------------------------------------------------
    /// Изменилось число людей на панели.
    void presenceChanged(int panelId, int peopleCount);

    /// Зона перешла из пустой в занятую или наоборот. Именно это событие
    /// оператору и показывают: непрерывно мигать на каждом изменении счётчика
    /// незачем.
    void zoneOccupancyChanged(bool occupied, int peopleTotal);

    // --- сигнал 2 ---------------------------------------------------------
    void alarmRaised(const core::AlarmEvent &event);
    void alarmAcknowledged(const core::AlarmEvent &event);
    void alarmConfirmed(const core::AlarmEvent &event);
    void alarmClosed(const core::AlarmEvent &event, bool wasFalse);
    void stateChanged(core::AlarmState state);

private:
    void setState(AlarmState state);
    void updateOccupancy();

    AlarmState m_state = AlarmState::Idle;
    AlarmEvent m_current;
    QHash<int, int> m_presence;
    bool m_occupied = false;
};

} // namespace core
