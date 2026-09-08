#include "core/MjpegWorker.h"

#include <QAuthenticator>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace core {

namespace {

/// Не копим байты бесконечно, если поток окажется не тем, что мы ждём.
constexpr qint64 kMaxBufferBytes = 32 * 1024 * 1024;

} // namespace

MjpegWorker::MjpegWorker(QObject *parent)
    : QObject(parent)
{
}

MjpegWorker::~MjpegWorker()
{
    stop();
}

void MjpegWorker::start(const QUrl &url)
{
    stop();
    m_stopped = false;
    m_url = url;
    m_liveFrameSeen = false;

    // Создаётся здесь, а не в конструкторе: объект переезжает в свой поток
    // через moveToThread ДО первого start(), и сетевые объекты Qt должны
    // рождаться в том потоке, где будут жить.
    if (!m_manager) {
        m_manager = new QNetworkAccessManager(this);
        connect(m_manager, &QNetworkAccessManager::authenticationRequired,
                this, &MjpegWorker::onAuthenticationRequired);
    }

    emit statusChanged(QStringLiteral("подключение к камере…"));
    requestFrame();
}

void MjpegWorker::requestFrame()
{
    m_buffer.clear();
    m_contentTypeChecked = false;
    m_authSupplied = false;

    QNetworkRequest request(m_url);
    // Камера не ответила восемь секунд — значит не ответит: не молчим до
    // системного таймаута в минуту с лишним.
    request.setTransferTimeout(8000);

    m_reply = m_manager->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &MjpegWorker::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &MjpegWorker::onFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &MjpegWorker::onErrorOccurred);
}

void MjpegWorker::stop()
{
    m_stopped = true;
    if (m_reply) {
        // Сначала отключаемся от сигналов — abort() сам по себе может дёрнуть
        // errorOccurred(), а разбираться с чужим отказом нам уже незачем.
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_buffer.clear();
}

void MjpegWorker::onAuthenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator)
{
    if (reply != m_reply || m_authSupplied)
        return;
    m_authSupplied = true;

    // Логин и пароль лежат прямо в адресе (CameraDiscovery::buildUrl их туда
    // кладёт). Qt не подставляет их сам — без этого ответа камера с паролем
    // просто откажет запросом ещё раз, и разбор тела никогда не получит ни
    // байта, будто камера не MJPEG вовсе.
    if (m_url.userName().isEmpty())
        return;
    authenticator->setUser(m_url.userName());
    authenticator->setPassword(m_url.password());
}

void MjpegWorker::onReadyRead()
{
    if (!m_reply)
        return;

    if (!m_contentTypeChecked) {
        m_contentTypeChecked = true;
        // Только явное "image/…" переводит в режим снимков. Отсутствующий
        // или неоднозначный заголовок трактуется как обычный поток — иначе
        // камера, которая просто не подписала Content-Type, замолчала бы
        // навсегда: onFinished() для настоящего потока не приходит вовсе,
        // пока камера не разорвёт соединение сама.
        const QByteArray contentType = m_reply->rawHeader("Content-Type").toLower();
        m_singleShotMode = contentType.startsWith("image/");
    }

    m_buffer += m_reply->readAll();

    if (m_buffer.size() > kMaxBufferBytes) {
        emit streamError(QStringLiteral("поток не похож на MJPEG"));
        stop();
        return;
    }

    if (m_singleShotMode)
        return; // тело одного снимка копится целиком, разбор — в onFinished()

    processBuffer();
}

namespace {

/// Чем закончилась попытка найти кадр по заголовку Content-Length.
enum class HeaderScanResult {
    NoHeaderFound,    ///< заголовка нет в буфере вовсе — пробовать разбор по маркерам JPEG
    WaitingForMore,   ///< заголовок либо кадр ещё не дочитаны целиком — буфер не тронут, ждём сеть
    GarbageSkipped,   ///< совпадение оказалось случайным, один мусорный байт снят — искать ещё раз
    FrameExtracted,   ///< кадр целиком собран и вырезан из буфера
};

/// Найти в буфере один целый кадр по заголовку Content-Length.
HeaderScanResult tryExtractByContentLength(QByteArray &buffer, QByteArray *frame)
{
    static const QByteArray kTitleCase = QByteArrayLiteral("Content-Length:");
    static const QByteArray kLowerCase = QByteArrayLiteral("content-length:");

    // Два написания заголовка покрывают практически все встречающиеся
    // серверы: Title-Case — общепринятое соглашение HTTP, строчными изредка
    // встречается у самодельных серверов. У IP Webcam — ровно "Content-Length"
    // (проверено побайтово).
    int lenPos = buffer.indexOf(kTitleCase);
    QByteArray header = kTitleCase;
    if (lenPos < 0) {
        lenPos = buffer.indexOf(kLowerCase);
        header = kLowerCase;
    }
    if (lenPos < 0)
        return HeaderScanResult::NoHeaderFound;

    const int lineEnd = buffer.indexOf("\r\n", lenPos);
    if (lineEnd < 0)
        return HeaderScanResult::WaitingForMore; // строка с длиной ещё не дочитана целиком

    bool ok = false;
    const qint64 length =
        buffer.mid(lenPos + header.size(), lineEnd - lenPos - header.size())
            .trimmed()
            .toLongLong(&ok);

    if (!ok || length <= 0 || length > kMaxBufferBytes) {
        // Не похоже на настоящий заголовок длины — совпадение случайное или
        // данные повреждены. Отступаем на одну букву: следующий заход того же
        // цикла поищет заголовок заново с этого места, а не встанет намертво
        // на одном и том же совпадении.
        buffer.remove(0, lenPos + 1);
        return HeaderScanResult::GarbageSkipped;
    }

    const int headerEnd = buffer.indexOf("\r\n\r\n", lineEnd);
    if (headerEnd < 0)
        return HeaderScanResult::WaitingForMore; // заголовки этой части ещё не дочитаны целиком

    const int dataStart = headerEnd + 4;
    if (buffer.size() < dataStart + length)
        return HeaderScanResult::WaitingForMore; // тело кадра ещё не пришло целиком

    *frame = buffer.mid(dataStart, int(length));
    buffer.remove(0, dataStart + int(length));
    return HeaderScanResult::FrameExtracted;
}

/// Найти один целый кадр по самим байтам JPEG — маркеру начала FF D8 (SOI) и
/// конца FF D9 (EOI). Единственный способ, который работает независимо от
/// того, как (и есть ли вообще) камера оформляет multipart-заголовки: сам
/// формат JPEG от этого не зависит. Внутри сжатых данных JPEG байт FF всегда
/// сопровождается стаффинг-байтом 00 или другим маркером (это гарантирует
/// сам формат) — то есть FF D9 внутри тела кадра случайно возникнуть не
/// может, а значит поиск EOI после SOI безопасен.
QByteArray tryExtractByJpegMarkers(QByteArray &buffer)
{
    static const QByteArray kSoi = QByteArrayLiteral("\xFF\xD8");
    static const QByteArray kEoi = QByteArrayLiteral("\xFF\xD9");

    const int soi = buffer.indexOf(kSoi);
    if (soi < 0)
        return {}; // ни один кадр ещё не начался — то, что накопилось, мусор

    const int eoi = buffer.indexOf(kEoi, soi + kSoi.size());
    if (eoi < 0)
        return {}; // кадр начался, но не закончился — ждём остаток

    const int frameEnd = eoi + kEoi.size();
    const QByteArray frame = buffer.mid(soi, frameEnd - soi);
    buffer.remove(0, frameEnd);
    return frame;
}

} // namespace

QByteArray MjpegWorker::extractLatestFrame(QByteArray &buffer)
{
    QByteArray lastFrame;

    while (true) {
        QByteArray frame;
        const HeaderScanResult result = tryExtractByContentLength(buffer, &frame);

        if (result == HeaderScanResult::FrameExtracted) {
            lastFrame = frame;
            continue;
        }
        if (result == HeaderScanResult::GarbageSkipped)
            continue; // буфер уже сдвинут — искать заголовок заново с того же места
        if (result == HeaderScanResult::WaitingForMore)
            break; // буфер не тронут — ждём, пока сеть довезёт остаток

        // NoHeaderFound: заголовка Content-Length в буфере нет вовсе — камера
        // либо не указывает длину части, либо вообще не оформляет multipart и
        // просто шлёт JPEG за JPEG. Оба случая читаются одинаково — по маркерам.
        frame = tryExtractByJpegMarkers(buffer);
        if (frame.isEmpty())
            break;
        lastFrame = frame;
    }

    return lastFrame;
}

void MjpegWorker::processBuffer()
{
    const QByteArray frame = extractLatestFrame(m_buffer);
    if (frame.isEmpty())
        return;

    const QImage image = QImage::fromData(frame, "JPEG");
    if (image.isNull())
        return;

    if (!m_liveFrameSeen) {
        m_liveFrameSeen = true;
        emit statusChanged(QStringLiteral("поток идёт"));
    }
    emit frameReady(image);
}

void MjpegWorker::processSingleShot()
{
    const QImage image = QImage::fromData(m_buffer, "JPEG");
    m_buffer.clear();

    if (!image.isNull()) {
        if (!m_liveFrameSeen) {
            m_liveFrameSeen = true;
            emit statusChanged(QStringLiteral("поток идёт (снимками)"));
        }
        emit frameReady(image);
    }

    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    // Это не обрыв связи — камера ответила ровно так, как умеет (одним
    // снимком на запрос), поэтому не ждём паузу переподключения, как после
    // настоящего сбоя, а сразу просимся на следующий кадр.
    QTimer::singleShot(kSnapshotPollMs, this, [this] {
        if (!m_stopped)
            requestFrame();
    });
}

void MjpegWorker::onFinished()
{
    if (!m_reply)
        return;

    if (m_reply->error() != QNetworkReply::NoError)
        return; // обработано в onErrorOccurred()

    if (m_singleShotMode) {
        processSingleShot();
        return;
    }

    // Живой поток MJPEG сам собой не заканчивается. Если соединение закрылось
    // без ошибки — камера всё равно пропала, и это тоже повод переподключиться.
    emit streamError(QStringLiteral("соединение закрыто"));
}

void MjpegWorker::onErrorOccurred()
{
    if (!m_reply)
        return;
    emit streamError(m_reply->errorString());
}

} // namespace core
