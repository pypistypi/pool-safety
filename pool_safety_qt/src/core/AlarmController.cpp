#include "core/AlarmController.h"

#include <QtGlobal>

namespace core {

AlarmController::AlarmController(QObject *parent)
    : QObject(parent)
{
}

// ------------------------------------------------------------- сигнал 1

int AlarmController::peopleInZone() const
{
    // Максимум по панелям: камеры смотрят на одну чашу с разных сторон, и
    // сумма считала бы одного человека столько раз, сколько камер его видят.
    int most = 0;
    for (int count : m_presence)
        most = qMax(most, count);
    return most;
}

int AlarmController::peopleOnPanel(int panelId) const
{
    return m_presence.value(panelId, 0);
}

void AlarmController::reportPresence(int panelId, int peopleCount)
{
    const int normalized = peopleCount > 0 ? peopleCount : 0;
    if (m_presence.value(panelId, 0) == normalized)
        return;

    m_presence[panelId] = normalized;
    emit presenceChanged(panelId, normalized);
    updateOccupancy();
}

void AlarmController::forgetPanel(int panelId)
{
    if (m_presence.remove(panelId) > 0) {
        emit presenceChanged(panelId, 0);
        updateOccupancy();
    }
}

void AlarmController::updateOccupancy()
{
    const int total = peopleInZone();
    const bool occupied = total > 0;
    if (occupied == m_occupied)
        return;
    m_occupied = occupied;
    emit zoneOccupancyChanged(occupied, total);
}

// ------------------------------------------------------------- сигнал 2

void AlarmController::raiseAlarm(const AlarmEvent &event)
{
    if (m_state != AlarmState::Idle)
        return;   // тревога уже идёт — вторая поверх неё только запутает

    m_current = event;
    if (!m_current.raisedAt.isValid())
        m_current.raisedAt = QDateTime::currentDateTime();
    m_current.acknowledgedAt = QDateTime();

    // Число людей в зоне запоминается на момент объявления и дальше не
    // меняется: диспетчеру нужно знать обстановку в момент происшествия, а не
    // то, сколько народу сбежалось потом.
    if (m_current.peopleInZone <= 0)
        m_current.peopleInZone = peopleInZone();

    setState(AlarmState::Raised);
    emit alarmRaised(m_current);
}

void AlarmController::acknowledge()
{
    if (m_state != AlarmState::Raised)
        return;

    m_current.acknowledgedAt = QDateTime::currentDateTime();
    setState(AlarmState::Acknowledged);
    emit alarmAcknowledged(m_current);
}

void AlarmController::dismissAsFalse()
{
    // Отменить можно и не приняв: если оператор видит на экране, что тревога
    // ложная, заставлять его сперва нажать «Принял» — лишний шаг в момент,
    // когда дорога каждая секунда.
    if (m_state == AlarmState::Idle)
        return;

    const AlarmEvent finished = m_current;
    m_current = AlarmEvent();
    setState(AlarmState::Idle);
    emit alarmClosed(finished, true);
}

void AlarmController::confirm()
{
    if (m_state != AlarmState::Raised && m_state != AlarmState::Acknowledged)
        return;

    if (!m_current.acknowledgedAt.isValid())
        m_current.acknowledgedAt = QDateTime::currentDateTime();

    setState(AlarmState::Confirmed);
    emit alarmConfirmed(m_current);
}

void AlarmController::closeIncident()
{
    if (m_state != AlarmState::Confirmed)
        return;

    const AlarmEvent finished = m_current;
    m_current = AlarmEvent();
    setState(AlarmState::Idle);
    emit alarmClosed(finished, false);
}

void AlarmController::updateDetails(const QString &details)
{
    if (m_state == AlarmState::Idle)
        return;
    m_current.details = details;
}

void AlarmController::setState(AlarmState state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

} // namespace core
