#pragma once

// ---------------------------------------------------------------------------
//  Источник кадров: камера, видеозапись или фотография.
//
//  ЕДИНЫЙ ИНТЕРФЕЙС НУЖЕН НЕ РАДИ КРАСОТЫ. Панель сетки не должна знать, что
//  именно ей показывают: оператору всё равно, откуда картинка, а модулю
//  распознавания (следующий этап) — тем более. Благодаря этому распознавание
//  подключится один раз и сразу заработает с любым входом: файлом, камерой
//  компьютера, а позже и живым потоком с камер объекта.
//
//  Декодированием занимается сам Qt в своих рабочих потоках, поэтому здесь
//  нет ни одного цикла чтения и ни одного таймера опроса.
// ---------------------------------------------------------------------------

#include "core/SourceDescriptor.h"
#include "core/MjpegWorker.h"

#include <QObject>
#include <QVideoFrame>
#include <QSize>
#include <QElapsedTimer>
#include <QThread>

class QTimer;
class QUrl;

class QCamera;
class QCameraDevice;
class QMediaCaptureSession;
class QMediaPlayer;
class QVideoSink;

namespace core {

class VideoSource : public QObject
{
    Q_OBJECT

public:
    explicit VideoSource(const SourceDescriptor &descriptor, QObject *parent = nullptr);
    ~VideoSource() override;

    const SourceDescriptor &descriptor() const { return m_descriptor; }

    /// Последний полученный кадр. Нужен, чтобы панель, подписавшаяся к уже
    /// работающему источнику, показала картинку немедленно, а не ждала
    /// следующего кадра (для фотографии ждать пришлось бы вечно).
    QVideoFrame lastFrame() const { return m_lastFrame; }

    QSize frameSize() const { return m_frameSize; }

    /// Наблюдаемая частота кадров. Считается по факту, а не берётся из
    /// заявленных возможностей устройства: они часто расходятся.
    double fps() const { return m_fps; }

    /// Короткое описание состояния для оператора: «поток идёт» либо причина,
    /// по которой картинки нет. Молча показывать чёрный прямоугольник нельзя —
    /// оператор должен отличать «в кадре пусто» от «камера отвалилась».
    QString statusText() const { return m_status; }

    bool isLive() const { return m_live; }

    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    void frameReady(const QVideoFrame &frame);
    void statusChanged(const QString &text);

protected:
    void publish(const QVideoFrame &frame);
    void setStatus(const QString &text);

    SourceDescriptor m_descriptor;

private:
    QVideoFrame m_lastFrame;
    QSize m_frameSize;
    QString m_status = QStringLiteral("подключение…");
    double m_fps = 0.0;
    bool m_live = false;

    QElapsedTimer m_fpsTimer;
    int m_fpsCounter = 0;
};

// --------------------------------------------------------------------- камера
class CameraSource : public VideoSource
{
    Q_OBJECT

public:
    CameraSource(const SourceDescriptor &descriptor, QObject *parent = nullptr);
    ~CameraSource() override;

    void start() override;
    void stop() override;

private:
    /// Ограничить разрешение и частоту камеры разумными значениями.
    void chooseCameraFormat(const QCameraDevice &device);

    QMediaCaptureSession *m_session = nullptr;
    QCamera *m_camera = nullptr;
    QVideoSink *m_sink = nullptr;
};

// ----------------------------------------------------------- видеозапись
class MediaFileSource : public VideoSource
{
    Q_OBJECT

public:
    MediaFileSource(const SourceDescriptor &descriptor, QObject *parent = nullptr);
    ~MediaFileSource() override;

    void start() override;
    void stop() override;

private:
    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;
};

// ------------------------------------------------------------- фотография
/// IP-камера в локальной сети.
///
///  ПОЧЕМУ ОТДЕЛЬНЫЙ КЛАСС, А НЕ ПОВТОРНОЕ ИСПОЛЬЗОВАНИЕ ФАЙЛОВОГО. Отличие
///  одно, но принципиальное: сетевой поток обрывается. Кабель, коммутатор,
///  перезагрузка камеры, потеря питания — всё это норма жизни, и наблюдение
///  не должно после такого замолкать навсегда. Поэтому здесь есть то, чего
///  нет у файла: обнаружение обрыва и переподключение по таймеру.
///
///  ПОТОК БЕРЁТСЯ ВТОРОСТЕПЕННЫЙ, ЕСЛИ ОН УКАЗАН В АДРЕСЕ. У большинства
///  камер есть «главный» поток (1080p и выше) и «второй» (704×576 и подобные).
///  Для распознавания второго достаточно: модель всё равно ужимает кадр до
///  640 точек, а сеть и процессор разгружаются в разы.
class NetworkSource : public VideoSource
{
    Q_OBJECT

public:
    NetworkSource(const SourceDescriptor &descriptor, QObject *parent = nullptr);
    ~NetworkSource() override;

    void start() override;
    void stop() override;

    /// Как часто сторож проверяет, идут ли кадры.
    static constexpr int kStallCheckMs = 2000;

    /// Сколько молчания считается обрывом. Три секунды: живая камера отдаёт
    /// кадры десятками в секунду, и три секунды тишины — это уже не заминка.
    static constexpr int kStallTimeoutMs = 3000;

    /// После скольких пустых попыток пробовать запасной путь.
    ///
    /// Три — это около десяти секунд молчания. Меньше брать нельзя: камера
    /// после перезагрузки поднимается не мгновенно, и сдаваться на первой же
    /// заминке значило бы дёргать поток на ровном месте.
    static constexpr int kAttemptsBeforeFallback = 3;

    /// Наибольшая пауза между попытками переподключения.
    ///
    /// БЕЗ ЭТОГО ОГРАНИЧЕНИЯ ПРОГРАММА ГРУЗИЛА ПРОЦЕССОР ВХОЛОСТУЮ. Сторож
    /// проверял связь каждые kStallCheckMs и, найдя молчание, пересоздавал
    /// проигрыватель — тоже каждые две-три секунды, СКОЛЬКО БЫ ПОДРЯД ПОПЫТКА
    /// НИ ПРОВАЛИВАЛАСЬ. На объекте с нестабильной Wi-Fi-камерой это дало
    /// 163 попытки за десять минут работы, никогда не убывающей нагрузкой:
    /// «Юго-восточный угол» в это время показывал «переподключение (попытка
    /// 163)…», а программа держала в среднем три с половиной ядра процессора.
    /// Разумная камера с перебоями раз в минуту не отличалась от полностью
    /// мёртвой — ту и другую программа долбила одинаково часто.
    ///
    /// Теперь пауза растёт с числом подряд неудачных попыток (секунда,
    /// две, три, …) и не превышает эту величину — 30 секунд достаточно
    /// редко, чтобы не заметно грузить систему, и достаточно часто, чтобы
    /// связь восстановилась быстро, как только камера снова стала доступна.
    static constexpr qint64 kMaxBackoffMs = 30000;

    /// Сколько ждать перед следующей попыткой при данном числе подряд
    /// неудачных. Вынесена отдельно и без побочных эффектов — так её можно
    /// проверить без настоящего сетевого потока.
    static qint64 backoffMs(int attempts)
    {
        return qMin(qint64(1000) * qMax(attempts, 0), kMaxBackoffMs);
    }

private slots:
    void reconnect();
    void onMjpegFrame(const QImage &image);
    void onMjpegError(const QString &reason);

private:
    void openStream();
    void openPlayerStream(const QUrl &url);
    void ensureMjpegWorker();

    /// Адрес, который проигрывается сейчас: либо заданный оператором, либо
    /// запасной, если основной замолчал.
    QString m_activeUrl;
    bool m_usingFallback = false;

    /// true — открыт через MjpegWorker (http://), false — через QMediaPlayer
    /// (rtsp://). См. MjpegWorker.h — там же измерение, почему это разделение
    /// того стоило.
    bool m_usingMjpeg = false;

    // --- путь http://: собственный разбор, вне потока интерфейса -----------
    QThread m_mjpegThread;
    MjpegWorker *m_mjpegWorker = nullptr;

    // --- путь rtsp://: как раньше, через Qt Multimedia ----------------------
    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;

    QTimer *m_watchdog = nullptr;
    qint64 m_lastFrameMs = 0;
    int m_attempts = 0;

    /// Когда была последняя попытка переподключения — чтобы выдержать паузу,
    /// растущую с числом неудач, а не гнать сторож на каждый его тик.
    qint64 m_lastAttemptMs = 0;
};

class StillImageSource : public VideoSource
{
    Q_OBJECT

public:
    StillImageSource(const SourceDescriptor &descriptor, QObject *parent = nullptr);

    void start() override;
    void stop() override;
};

/// Создать источник, подходящий описанию. Единственное место, где
/// принимается решение «камера, запись или фотография».
VideoSource *createSource(const SourceDescriptor &descriptor, QObject *parent = nullptr);

} // namespace core
