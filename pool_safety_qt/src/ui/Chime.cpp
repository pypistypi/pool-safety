#include "ui/Chime.h"

#include <QAudioSink>
#include <QBuffer>
#include <QMediaDevices>
#include <QAudioDevice>

#include <cmath>

namespace ui {

namespace {
constexpr int kSampleRate = 44100;
constexpr double kTwoPi = 6.283185307179586;
}

Chime::Chime(QObject *parent)
    : QObject(parent)
{
    m_format.setSampleRate(kSampleRate);
    m_format.setChannelCount(1);
    m_format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        // Звуковой карты нет — не беда, но и притворяться, что звук есть, не
        // будем: остальные способы уведомить оператора остаются.
        m_enabled = false;
        return;
    }
    if (!device.isFormatSupported(m_format))
        m_format = device.preferredFormat();

    rebuildTones();
}

Chime::~Chime() = default;

void Chime::setEnabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled)
        stopAlarm();
}

void Chime::setVolumes(double alarmVolume, double calmVolume)
{
    m_alarmVolume = qBound(0.05, alarmVolume, 1.0);
    m_calmVolume = qBound(0.02, calmVolume, 1.0);
    rebuildTones();
}

void Chime::rebuildTones()
{
    // Сигнал присутствия: две ноты вверх — вопросительно, а не тревожно.
    m_presence = buildTone(660.0, 90, m_calmVolume, true)
                 + buildTone(880.0, 130, m_calmVolume, false);

    // Внимание: одна нота, чуть настойчивее спокойной.
    m_attention = buildTone(750.0, 160, qMin(1.0, m_calmVolume * 1.6), true);

    // Сирена собирается при каждом включении: её размах зависит от того,
    // усилена она или нет.
    m_siren.clear();
}

QByteArray Chime::buildTone(double frequency, int milliseconds,
                            double volume, bool fadeIn) const
{
    const int rate = m_format.sampleRate() > 0 ? m_format.sampleRate() : kSampleRate;
    const int channels = qMax(1, m_format.channelCount());
    const int frames = rate * milliseconds / 1000;

    QByteArray data;
    data.resize(frames * channels * int(sizeof(qint16)));
    auto *samples = reinterpret_cast<qint16 *>(data.data());

    // Плавные начало и конец. Без них в динамике слышен щелчок — обрыв
    // синусоиды на полуслове звучит как треск, а не как сигнал.
    const int rampFrames = qMin(frames / 4, rate / 100);

    for (int i = 0; i < frames; ++i) {
        double envelope = 1.0;
        if (fadeIn && i < rampFrames && rampFrames > 0)
            envelope = double(i) / rampFrames;
        if (rampFrames > 0 && i > frames - rampFrames)
            envelope = qMin(envelope, double(frames - i) / rampFrames);

        const double value = std::sin(kTwoPi * frequency * i / rate) * volume * envelope;
        const auto sample = qint16(qBound(-1.0, value, 1.0) * 32767);
        for (int c = 0; c < channels; ++c)
            samples[i * channels + c] = sample;
    }
    return data;
}

QByteArray Chime::buildSiren(double lowFrequency, double highFrequency,
                             int noteMs, int repeats, double volume) const
{
    QByteArray data;
    for (int i = 0; i < repeats; ++i) {
        data += buildTone(highFrequency, noteMs, volume, i == 0);
        data += buildTone(lowFrequency, noteMs, volume, false);
    }
    return data;
}

void Chime::play(const QByteArray &samples, double volume)
{
    if (!m_enabled || samples.isEmpty())
        return;

    // Прежний звук обрывается: два наложившихся сигнала сливаются в кашу, из
    // которой оператор не поймёт, что именно ему сообщили.
    if (m_sink) {
        m_sink->stop();
        m_sink.reset();
    }

    m_buffer = std::make_unique<QBuffer>();
    m_buffer->setData(samples);
    if (!m_buffer->open(QIODevice::ReadOnly))
        return;

    m_sink = std::make_unique<QAudioSink>(m_format);
    m_sink->setVolume(qreal(qBound(0.0, volume, 1.0)));
    connect(m_sink.get(), &QAudioSink::stateChanged,
            this, &Chime::onSinkStateChanged, Qt::QueuedConnection);
    m_sink->start(m_buffer.get());
}

void Chime::onSinkStateChanged()
{
    if (!m_alarmRunning || !m_sink)
        return;

    // Сирена звучит по кругу: буфер доиграл — начинаем сначала. Так тревога
    // не превращается в единственный «бип», который легко пропустить.
    if (m_sink->state() == QAudio::IdleState && m_buffer) {
        m_buffer->seek(0);
        m_sink->start(m_buffer.get());
    }
}

void Chime::playPresence()
{
    if (m_alarmRunning)
        return;   // во время тревоги спокойные звуки только мешают
    play(m_presence, m_calmVolume);
}

void Chime::playAttention()
{
    if (m_alarmRunning)
        return;
    play(m_attention, m_calmVolume);
}

void Chime::startAlarm()
{
    if (m_alarmRunning)
        return;

    m_escalated = false;
    // Обычная сирена: 880 ↔ 620 Гц, ноты по 300 мс. Пять пар — примерно три
    // секунды, после чего запись начинается сначала.
    m_siren = buildSiren(620.0, 880.0, 300, 5, 1.0);
    m_alarmRunning = true;
    play(m_siren, m_alarmVolume);
}

void Chime::escalate()
{
    if (!m_alarmRunning || m_escalated)
        return;

    m_escalated = true;
    // Усиленная сирена: размах шире (520 ↔ 1050 Гц) и ноты короче — звучит
    // резче и «быстрее», а громкость выставляется на предел.
    m_siren = buildSiren(520.0, 1050.0, 190, 8, 1.0);

    m_alarmRunning = false;   // чтобы play() не пытался зациклить прежний буфер
    play(m_siren, 1.0);
    m_alarmRunning = true;
}

void Chime::stopAlarm()
{
    m_alarmRunning = false;
    m_escalated = false;
    if (m_sink) {
        m_sink->stop();
        m_sink.reset();
    }
    m_buffer.reset();
}

} // namespace ui
