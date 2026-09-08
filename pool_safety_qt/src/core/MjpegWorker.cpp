#include "core/MjpegWorker.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

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
    m_buffer.clear();
    m_liveFrameSeen = false;

    // Создаётся здесь, а не в конструкторе: объект переезжает в свой поток
    // через moveToThread ДО первого start(), и сетевые объекты Qt должны
    // рождаться в том потоке, где будут жить.
    if (!m_manager)
        m_manager = new QNetworkAccessManager(this);

    QNetworkRequest request(url);
    // Камера не ответила восемь секунд — значит не ответит: не молчим до
    // системного таймаута в минуту с лишним.
    request.setTransferTimeout(8000);

    m_reply = m_manager->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &MjpegWorker::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &MjpegWorker::onFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &MjpegWorker::onErrorOccurred);

    emit statusChanged(QStringLiteral("подключение к камере…"));
}

void MjpegWorker::stop()
{
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

void MjpegWorker::onReadyRead()
{
    if (!m_reply)
        return;

    m_buffer += m_reply->readAll();

    if (m_buffer.size() > kMaxBufferBytes) {
        emit streamError(QStringLiteral("поток не похож на MJPEG"));
        stop();
        return;
    }

    processBuffer();
}

QByteArray MjpegWorker::extractLatestFrame(QByteArray &buffer)
{
    // Два написания заголовка покрывают практически все встречающиеся
    // серверы: Title-Case — общепринятое соглашение HTTP, строчными изредка
    // встречается у самодельных серверов. У IP Webcam — ровно "Content-Length"
    // (проверено побайтово).
    static const QByteArray kTitleCase = QByteArrayLiteral("Content-Length:");
    static const QByteArray kLowerCase = QByteArrayLiteral("content-length:");

    QByteArray lastFrame;

    while (true) {
        int lenPos = buffer.indexOf(kTitleCase);
        QByteArray header = kTitleCase;
        if (lenPos < 0) {
            lenPos = buffer.indexOf(kLowerCase);
            header = kLowerCase;
        }
        if (lenPos < 0)
            break; // заголовка ещё нет в накопленном — ждём следующих байтов

        const int lineEnd = buffer.indexOf("\r\n", lenPos);
        if (lineEnd < 0)
            break; // строка с длиной ещё не дочитана целиком

        bool ok = false;
        const qint64 length =
            buffer.mid(lenPos + header.size(),
                      lineEnd - lenPos - header.size())
                .trimmed()
                .toLongLong(&ok);

        if (!ok || length <= 0 || length > kMaxBufferBytes) {
            // Не похоже на настоящий заголовок длины — совпадение случайное
            // или данные повреждены. Отступаем на одну букву и ищем дальше,
            // а не встаём намертво на одном и том же месте.
            buffer.remove(0, lenPos + 1);
            continue;
        }

        const int headerEnd = buffer.indexOf("\r\n\r\n", lineEnd);
        if (headerEnd < 0)
            break; // заголовки этой части ещё не дочитаны целиком

        const int dataStart = headerEnd + 4;
        if (buffer.size() < dataStart + length)
            break; // тело кадра ещё не пришло целиком — ждём

        // Кадр целиком собран. Продолжаем цикл, не декодируя: если в буфере
        // уже есть и СЛЕДУЮЩИЙ целый кадр, этот — устаревший, и достаточно
        // запомнить его байты, не более того. Декодирование — дело вызывающей
        // стороны, и оно случится только для того, что переживёт весь разбор.
        lastFrame = buffer.mid(dataStart, int(length));
        buffer.remove(0, dataStart + int(length));
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

void MjpegWorker::onFinished()
{
    if (!m_reply)
        return;

    // Живой поток MJPEG сам собой не заканчивается. Если соединение закрылось
    // без ошибки — камера всё равно пропала, и это тоже повод переподключиться.
    if (m_reply->error() == QNetworkReply::NoError)
        emit streamError(QStringLiteral("соединение закрыто"));
}

void MjpegWorker::onErrorOccurred()
{
    if (!m_reply)
        return;
    emit streamError(m_reply->errorString());
}

} // namespace core
