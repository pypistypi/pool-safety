#include "core/VideoSource.h"

#include "core/CameraDiscovery.h"
#include "core/DeviceRegistry.h"

#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QMediaCaptureSession>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QMediaDevices>
#include <QImage>
#include <QImageReader>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>
#include <QUrl>

namespace core {

// ===========================================================================
//  Базовый класс
// ===========================================================================

VideoSource::VideoSource(const SourceDescriptor &descriptor, QObject *parent)
    : QObject(parent)
    , m_descriptor(descriptor)
{
    m_fpsTimer.start();
}

VideoSource::~VideoSource() = default;

void VideoSource::publish(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return;

    m_lastFrame = frame;
    m_frameSize = frame.size();

    // Частота считается раз в секунду накопленным счётчиком. Делить на каждом
    // кадре не нужно, а усреднение за секунду даёт устойчивое число вместо
    // прыгающего.
    ++m_fpsCounter;
    const qint64 elapsed = m_fpsTimer.elapsed();
    if (elapsed >= 1000) {
        m_fps = m_fpsCounter * 1000.0 / double(elapsed);
        m_fpsCounter = 0;
        m_fpsTimer.restart();
    }

    if (!m_live) {
        m_live = true;
        setStatus(QStringLiteral("поток идёт"));
    }

    emit frameReady(frame);
}

void VideoSource::setStatus(const QString &text)
{
    if (m_status == text)
        return;
    m_status = text;
    emit statusChanged(text);
}

// ===========================================================================
//  Камера компьютера
// ===========================================================================

CameraSource::CameraSource(const SourceDescriptor &descriptor, QObject *parent)
    : VideoSource(descriptor, parent)
{
}

CameraSource::~CameraSource()
{
    CameraSource::stop();
}

void CameraSource::start()
{
    if (m_camera)
        return;

    // Устройство ищем по идентификатору, а не по номеру в списке: номер
    // меняется от подключения любой другой камеры, а идентификатор — нет.
    QCameraDevice device;
    const auto inputs = QMediaDevices::videoInputs();
    const QByteArray needle = m_descriptor.target().toUtf8();
    for (const QCameraDevice &candidate : inputs) {
        if (candidate.id() == needle) {
            device = candidate;
            break;
        }
    }

    if (device.isNull()) {
        setStatus(QStringLiteral("камера недоступна — отключена или занята"));
        return;
    }

    m_session = new QMediaCaptureSession(this);
    m_camera  = new QCamera(device, this);
    m_sink    = new QVideoSink(this);

    m_session->setCamera(m_camera);
    m_session->setVideoSink(m_sink);

    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) { publish(frame); });

    connect(m_camera, &QCamera::errorOccurred, this,
            [this](QCamera::Error error, const QString &description) {
                if (error == QCamera::NoError)
                    return;
                setStatus(description.isEmpty()
                              ? QStringLiteral("ошибка камеры")
                              : description);
            });

    chooseCameraFormat(device);

    setStatus(QStringLiteral("включение камеры…"));
    m_camera->start();
}

void CameraSource::chooseCameraFormat(const QCameraDevice &device)
{
    // ЗАЧЕМ ОГРАНИЧИВАТЬ. Многие камеры запускаются в наибольшем доступном
    // разрешении — у Logitech BRIO это 4K. На панели размером в четверть
    // экрана такой кадр всё равно будет уменьшен, зато по дороге его придётся
    // принять с USB, разжать и отмасштабировать: работа впустую, и немалая.
    // Потолок 1280x720 сохраняет запас качества для будущего распознавания и
    // снимает основную часть нагрузки.
    constexpr int kMaxWidth = 1280;
    constexpr int kMaxHeight = 720;
    constexpr double kPreferredFps = 30.0;

    QCameraFormat best;
    for (const QCameraFormat &format : device.videoFormats()) {
        const QSize size = format.resolution();
        if (size.width() > kMaxWidth || size.height() > kMaxHeight)
            continue;

        if (best.isNull()) {
            best = format;
            continue;
        }

        // Сначала берём наибольшее разрешение в допустимых пределах, и лишь
        // при равном разрешении смотрим на частоту.
        const qint64 area = qint64(size.width()) * size.height();
        const qint64 bestArea = qint64(best.resolution().width()) * best.resolution().height();
        if (area > bestArea) {
            best = format;
        } else if (area == bestArea) {
            const double current = qMin(format.maxFrameRate(), kPreferredFps);
            const double chosen = qMin(best.maxFrameRate(), kPreferredFps);
            if (current > chosen)
                best = format;
        }
    }

    // Ни один формат не уложился в потолок — оставляем выбор за системой.
    // Своевольно ронять качество ниже возможностей камеры мы не станем.
    if (!best.isNull())
        m_camera->setCameraFormat(best);
}

void CameraSource::stop()
{
    if (!m_camera)
        return;

    // Порядок важен: сперва отцепляем приёмник кадров, иначе во время
    // остановки может прийти кадр уже разрушаемому объекту.
    if (m_sink)
        disconnect(m_sink, nullptr, this, nullptr);

    m_camera->stop();

    // Все три объекта созданы с родителем this, поэтому каждый удаляется
    // явно — иначе они дожили бы до разрушения самого источника и держали
    // устройство занятым, а его нужно освободить сразу: другая панель могла
    // уже выбрать эту же камеру.
    delete m_session;
    delete m_camera;
    delete m_sink;
    m_session = nullptr;
    m_camera = nullptr;
    m_sink = nullptr;
}

// ===========================================================================
//  Видеозапись
// ===========================================================================

MediaFileSource::MediaFileSource(const SourceDescriptor &descriptor, QObject *parent)
    : VideoSource(descriptor, parent)
{
}

MediaFileSource::~MediaFileSource()
{
    MediaFileSource::stop();
}

void MediaFileSource::start()
{
    if (m_player)
        return;

    if (!QFileInfo::exists(m_descriptor.target())) {
        setStatus(QStringLiteral("файл не найден"));
        return;
    }

    m_player = new QMediaPlayer(this);
    m_sink   = new QVideoSink(this);
    m_player->setVideoSink(m_sink);

    // Звук намеренно не подключается: это видеонаблюдение, звуковая дорожка
    // записи оператору не нужна и только мешала бы слышать сигнал тревоги.

    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) { publish(frame); });

    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &description) {
                if (error == QMediaPlayer::NoError)
                    return;
                setStatus(description.isEmpty()
                              ? QStringLiteral("файл не читается")
                              : description);
            });

    // Запись идёт по кругу без конца: заглушка должна вести себя как живой
    // поток с камеры, у которого «конца» не бывает.
    m_player->setLoops(QMediaPlayer::Infinite);
    m_player->setSource(QUrl::fromLocalFile(m_descriptor.target()));

    setStatus(QStringLiteral("открытие записи…"));
    m_player->play();
}

void MediaFileSource::stop()
{
    if (!m_player)
        return;

    if (m_sink)
        disconnect(m_sink, nullptr, this, nullptr);

    m_player->stop();
    delete m_player;
    delete m_sink;
    m_player = nullptr;
    m_sink = nullptr;
}

// ===========================================================================
//  Фотография
// ===========================================================================

StillImageSource::StillImageSource(const SourceDescriptor &descriptor, QObject *parent)
    : VideoSource(descriptor, parent)
{
}

void StillImageSource::start()
{
    QImageReader reader(m_descriptor.target());
    reader.setAutoTransform(true);
    const QImage image = reader.read();

    if (image.isNull()) {
        setStatus(QStringLiteral("изображение не читается"));
        return;
    }

    // Кадр выдаётся ровно один раз. Перерисовывать неподвижную картинку по
    // таймеру незачем — именно такие «холостые» циклы и грузили процессор в
    // прежней версии.
    publish(QVideoFrame(image.convertToFormat(QImage::Format_RGBA8888)));
    setStatus(QStringLiteral("фотография (неподвижный кадр)"));
}

void StillImageSource::stop()
{
}

// ===========================================================================

VideoSource *createSource(const SourceDescriptor &descriptor, QObject *parent)
{
    switch (descriptor.kind()) {
    case SourceKind::Camera:
        return new CameraSource(descriptor, parent);
    case SourceKind::File:
        if (descriptor.isStillImage())
            return new StillImageSource(descriptor, parent);
        return new MediaFileSource(descriptor, parent);
    case SourceKind::Network:
        return new NetworkSource(descriptor, parent);
    case SourceKind::None:
        break;
    }
    return nullptr;
}


// ===========================================================================
//  IP-камера в локальной сети
// ===========================================================================

NetworkSource::NetworkSource(const SourceDescriptor &descriptor, QObject *parent)
    : VideoSource(descriptor, parent)
{
}

NetworkSource::~NetworkSource()
{
    NetworkSource::stop();
}

void NetworkSource::start()
{
    if (m_player)
        return;

    m_attempts = 0;
    openStream();

    // Сторож обрыва.
    //
    // ЗАЧЕМ ОН НУЖЕН, ЕСЛИ ЕСТЬ СИГНАЛ ОБ ОШИБКЕ. Потому что самый частый вид
    // обрыва — молчаливый: соединение формально живо, ошибок нет, а кадры
    // перестали приходить. Так ведут себя камеры при потере питания на
    // коммутаторе и при перегрузке сети. Ошибку в этом случае никто не
    // сообщит, и наблюдение молча замрёт на последнем кадре — худшее, что
    // может случиться с постом охраны.
    m_watchdog = new QTimer(this);
    m_watchdog->setInterval(kStallCheckMs);
    connect(m_watchdog, &QTimer::timeout, this, &NetworkSource::reconnect);
    m_watchdog->start();
}

void NetworkSource::openStream()
{
    m_player = new QMediaPlayer(this);
    m_sink = new QVideoSink(this);
    m_player->setVideoSink(m_sink);

    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
                m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
                m_attempts = 0;
                publish(frame);
            });

    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &description) {
                if (error == QMediaPlayer::NoError)
                    return;
                setStatus(description.isEmpty()
                              ? QStringLiteral("камера не отвечает")
                              : description);
            });

    if (m_activeUrl.isEmpty())
        m_activeUrl = m_descriptor.target();

    m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
    m_player->setSource(QUrl(m_activeUrl));
    setStatus(m_attempts == 0 ? QStringLiteral("подключение к камере…")
                              : QStringLiteral("переподключение (попытка %1)…")
                                    .arg(m_attempts));
    m_player->play();
}

void NetworkSource::reconnect()
{
    const qint64 idle = QDateTime::currentMSecsSinceEpoch() - m_lastFrameMs;
    if (idle < kStallTimeoutMs)
        return;   // кадры идут, всё в порядке

    ++m_attempts;

    // ЕСЛИ ПОТОК МОЛЧИТ, ПРОБУЕМ ДРУГОЙ ПУТЬ К ТОЙ ЖЕ КАМЕРЕ.
    //
    // Соединение установилось, ошибок нет, а кадров не приходит — так ведёт
    // себя RTSP, когда его пакеты UDP не доходят: при включённой VPN, при
    // втором сетевом адресе в той же подсети, за строгим межсетевым экраном.
    // Молча переподключаться в такой обстановке можно вечно. Поэтому после
    // нескольких пустых попыток переходим на путь по HTTP, если он у этой
    // камеры есть, и говорим об этом оператору.
    if (!m_usingFallback && m_attempts >= kAttemptsBeforeFallback) {
        const QString fallback = CameraDiscovery::httpFallbackFor(m_descriptor.target());
        if (!fallback.isEmpty()) {
            m_usingFallback = true;
            m_activeUrl = fallback;
            setStatus(QStringLiteral(
                "поток RTSP не доходит — перешли на MJPEG"));
        }
    }

    // Полное пересоздание проигрывателя, а не просто повторный play():
    // после обрыва RTSP-сессии проигрыватель остаётся в негодном состоянии, и
    // «продолжить» его нельзя — только начать заново.
    if (m_player) {
        m_player->stop();
        delete m_player;
        m_player = nullptr;
        m_sink = nullptr;
    }

    setStatus(QStringLiteral("связь с камерой потеряна, переподключение…"));
    openStream();
}

void NetworkSource::stop()
{
    if (m_watchdog) {
        m_watchdog->stop();
        delete m_watchdog;
        m_watchdog = nullptr;
    }
    if (m_player) {
        m_player->stop();
        delete m_player;
        m_player = nullptr;
        m_sink = nullptr;
    }
    setStatus(QStringLiteral("отключено"));
}

} // namespace core
