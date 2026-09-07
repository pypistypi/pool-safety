#pragma once

// ---------------------------------------------------------------------------
//  Звуковые уведомления оператору.
//
//  ТРИ ЗВУКА, И СПУТАТЬ ИХ НЕЛЬЗЯ:
//
//    * ПРИСУТСТВИЕ — короткий мягкий сигнал из двух нот вверх. Сообщает, что
//      в зоне появились люди, и ничего не требует. Услышав его тридцатый раз
//      за смену, оператор не должен вздрагивать.
//    * ВНИМАНИЕ — одна короткая нота средней высоты. Признаки есть, но до
//      тревоги не дотягивают: «взгляните на панель».
//    * ТРЕВОГА — СИРЕНА. Не «сигнал», а именно сирена: две чередующиеся ноты,
//      звучащие непрерывно, пока оператор не нажмёт «ПРИНЯЛ». Один короткий
//      звук в начале можно прослушать, отвернувшись или разговаривая, — и
//      тогда тревога бессмысленна.
//
//  СИРЕНА УСИЛИВАЕТСЯ. Если реакции нет дольше заданного времени, размах
//  частот растёт, а громкость поднимается до предела. Расчёт на то, что
//  человек может находиться в соседнем помещении.
//
//  ЗВУК СЧИТАЕТСЯ, А НЕ БЕРЁТСЯ ИЗ ФАЙЛОВ. Отдельные звуковые файлы пришлось
//  бы класть рядом с программой, и любой из них можно потерять при
//  копировании — а тревога без звука это уже не тревога. Здесь тон
//  вычисляется на месте: терять нечего.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QByteArray>
#include <QAudioFormat>

#include <memory>

class QAudioSink;
class QBuffer;

namespace ui {

class Chime : public QObject
{
    Q_OBJECT

public:
    explicit Chime(QObject *parent = nullptr);
    ~Chime() override;

    /// Сигнал 1: в зоне появились люди.
    void playPresence();

    /// Признаки опасности, до тревоги не дотягивающие.
    void playAttention();

    /// Сигнал 2: включить сирену. Звучит, пока не вызван stopAlarm().
    void startAlarm();

    /// Усилить сирену: громче и резче. Вызывается, когда реакции нет.
    void escalate();

    /// Выключить сирену.
    void stopAlarm();

    bool isAlarmSounding() const { return m_alarmRunning; }

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    /// Громкость тревоги и спокойных уведомлений, 0..1.
    void setVolumes(double alarmVolume, double calmVolume);

private slots:
    void onSinkStateChanged();

private:
    void play(const QByteArray &samples, double volume);
    void rebuildTones();

    QByteArray buildTone(double frequency, int milliseconds,
                         double volume, bool fadeIn) const;

    /// Сирена: две чередующиеся ноты. `sweep` — насколько далеко они разведены
    /// по высоте; чем больше, тем тревожнее звучит.
    QByteArray buildSiren(double lowFrequency, double highFrequency,
                          int noteMs, int repeats, double volume) const;

    QAudioFormat m_format;
    std::unique_ptr<QAudioSink> m_sink;
    std::unique_ptr<QBuffer> m_buffer;

    QByteArray m_presence;
    QByteArray m_attention;
    QByteArray m_siren;

    bool m_enabled = true;
    bool m_alarmRunning = false;
    bool m_escalated = false;
    double m_alarmVolume = 0.9;
    double m_calmVolume = 0.22;
};

} // namespace ui
