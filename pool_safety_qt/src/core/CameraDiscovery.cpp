#include "core/CameraDiscovery.h"

#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QUuid>

namespace core {

namespace {

constexpr quint16 kOnvifPort = 3702;
constexpr int kOnvifWaitMs = 3000;
constexpr int kPortTimeoutMs = 400;

/// Сколько проб держим в работе одновременно.
///
/// Портов теперь шесть, адресов в подсети — до 254, то есть проб полторы
/// тысячи. Открыть столько сокетов разом нельзя: часть немедленно упадёт по
/// исчерпанию дескрипторов, и поиск станет врать — «камеры не найдены» там,
/// где они есть. Поэтому очередь с ограничением.
constexpr int kMaxParallel = 96;

/// Сколько ждём ответа на отпечаток. Отпечаток спрашивается у единиц адресов
/// (тех, где порт вообще открылся), так что здесь можно быть терпеливее.
constexpr int kFingerprintMs = 1500;

/// Сколько ждём ответа камеры на запрос RTSP. Настоящая камера отвечает сразу:
/// это первая строка её же протокола, до всякой проверки пароля.
constexpr int kRtspAnswerMs = 700;

/// Запрос WS-Discovery. Текст задан стандартом ONVIF; менять в нём нечего,
/// кроме идентификатора сообщения, который обязан быть новым каждый раз.
QByteArray onvifProbe()
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header>"
        "<w:MessageID>uuid:%1</w:MessageID>"
        "<w:To e:mustUnderstand=\"true\">"
        "urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action e:mustUnderstand=\"true\">"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
        "</e:Header>"
        "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types>"
        "</d:Probe></e:Body></e:Envelope>").arg(id).toUtf8();
}

/// Все адреса IPv4 этого компьютера в локальных сетях.
QVector<QPair<QHostAddress, int>> localNetworks()
{
    QVector<QPair<QHostAddress, int>> result;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)
            || !interface.flags().testFlag(QNetworkInterface::IsRunning)
            || interface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const auto entries = interface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (address.isLoopback())
                continue;
            result.append({address, entry.prefixLength()});
        }
    }
    return result;
}

/// Похож ли ответ на веб-сервер камеры.
///
/// Проверка нарочно узкая. Открытый порт 8080 — не признак камеры: так же
/// слушают роутеры, принтеры и любой ноутбук с отладочным сервером. Показать
/// их оператору как «найденные камеры» хуже, чем не показать ничего: он будет
/// перебирать их по одной и терять доверие к поиску.
bool looksLikeCamera(const QByteArray &server, const QByteArray &body,
                     QString *description)
{
    const QString serverText = QString::fromLatin1(server).toLower();

    // Приложение «IP Webcam» на смартфоне: отвечает на /status.json своим
    // состоянием. Ответ ни с чем не спутать, и по нему сразу известен путь
    // потока — гадать не придётся.
    if (body.contains("\"curvals\"") || body.contains("curvals")) {
        if (description)
            *description = QStringLiteral("Смартфон (IP Webcam)");
        return true;
    }

    static const QStringList kMarkers = {
        QStringLiteral("ip webcam"), QStringLiteral("ipcam"),
        QStringLiteral("webcam"),    QStringLiteral("camera"),
        QStringLiteral("hikvision"), QStringLiteral("dahua"),
        QStringLiteral("reolink"),   QStringLiteral("axis"),
        QStringLiteral("hipcam"),    QStringLiteral("dvrdvs"),
    };
    for (const QString &marker : kMarkers) {
        if (!serverText.contains(marker))
            continue;
        if (description)
            *description = QString::fromLatin1(server).trimmed();
        return true;
    }
    return false;
}

} // namespace

QString DiscoveredCamera::title() const
{
    QString text = address;
    if (!description.isEmpty())
        text += QStringLiteral(" — ") + description;

    QStringList marks;
    if (onvif)
        marks << QStringLiteral("ONVIF");
    if (!rtspPorts.isEmpty()) {
        QStringList ports;
        for (int port : rtspPorts)
            ports << QString::number(port);
        marks << QStringLiteral("RTSP ") + ports.join(QStringLiteral(", "));
    }
    if (!httpPorts.isEmpty()) {
        QStringList ports;
        for (int port : httpPorts)
            ports << QString::number(port);
        marks << QStringLiteral("HTTP ") + ports.join(QStringLiteral(", "));
    }
    if (!marks.isEmpty())
        text += QStringLiteral("  [") + marks.join(QStringLiteral("; ")) + QLatin1Char(']');
    return text;
}

CameraDiscovery::CameraDiscovery(QObject *parent)
    : QObject(parent)
{
}

CameraDiscovery::~CameraDiscovery()
{
    stop();
}

// ------------------------------------------------------------------- порты

QVector<int> CameraDiscovery::rtspPorts()
{
    // 554 — стандарт. 8554 — вторая по частоте привычка: так слушают
    // видеосерверы и камеры, у которых 554 занят прошивкой.
    return {554, 8554};
}

QVector<int> CameraDiscovery::httpPorts()
{
    // 8080 — смартфон в роли камеры и множество недорогих моделей.
    // 8000 — Hikvision (служебный порт), 88 и 81 — распространённые замены 80.
    //
    // Порт 80 сюда не входит намеренно: его слушает всё подряд, и опрос дал бы
    // список из роутера, телевизора и принтера. Камеру с веб-мордой на 80 мы и
    // так найдём — по её порту RTSP.
    return {8080, 8000, 88, 81};
}

int CameraDiscovery::defaultPort(const QString &scheme)
{
    const QString lower = scheme.toLower();
    if (lower == QLatin1String("rtsp"))
        return 554;
    if (lower == QLatin1String("rtsps"))
        return 322;
    if (lower == QLatin1String("https"))
        return 443;
    return 80;
}

QVector<CameraDiscovery::UrlTemplate> CameraDiscovery::urlTemplates()
{
    return {
        {QStringLiteral("Смартфон (IP Webcam), MJPEG"), QStringLiteral("http"), 8080,
         QStringLiteral("/video"),
         QStringLiteral("основной: доходит всегда, включается мгновенно")},
        {QStringLiteral("Смартфон (IP Webcam), H.264"), QStringLiteral("rtsp"), 8080,
         QStringLiteral("/h264_ulaw.sdp"),
         QStringLiteral("легче для сети, но пропадает при VPN и на двух сетях")},
        {QStringLiteral("Hikvision, HiWatch"), QStringLiteral("rtsp"), 554,
         QStringLiteral("/Streaming/Channels/102"),
         QStringLiteral("второй поток; главный — 101")},
        {QStringLiteral("Dahua, EZ-IP"), QStringLiteral("rtsp"), 554,
         QStringLiteral("/cam/realmonitor?channel=1&subtype=1"),
         QStringLiteral("subtype=1 — второй поток, 0 — главный")},
        {QStringLiteral("TP-Link (Tapo, VIGI)"), QStringLiteral("rtsp"), 554,
         QStringLiteral("/stream2"),
         QStringLiteral("stream1 — главный поток")},
        {QStringLiteral("Reolink"), QStringLiteral("rtsp"), 554,
         QStringLiteral("/h264Preview_01_sub"),
         QStringLiteral("_main — главный поток")},
        {QStringLiteral("Axis"), QStringLiteral("rtsp"), 554,
         QStringLiteral("/axis-media/media.amp?resolution=704x576"),
         QStringLiteral("разрешение задаётся в адресе")},
        {QStringLiteral("Общий ONVIF"), QStringLiteral("rtsp"), 554,
         QStringLiteral("/onvif1"),
         QStringLiteral("встречается у недорогих камер")},
        {QStringLiteral("Ввести вручную"), QString(), 0, QString(),
         QStringLiteral("путь смотрите в настройках самой камеры")},
    };
}

QString CameraDiscovery::httpFallbackFor(const QString &streamUrl)
{
    const QUrl url(streamUrl);
    if (url.scheme().compare(QLatin1String("rtsp"), Qt::CaseInsensitive) != 0)
        return {};

    // Пока запасной путь известен только для смартфона: он один отдаёт оба
    // вида потока по одному порту. Для обычных камер угадывать нельзя — у
    // каждого производителя свой путь, и промах дал бы ложную надежду.
    if (!url.path().contains(QLatin1String("h264_ulaw"), Qt::CaseInsensitive))
        return {};

    QUrl fallback(url);
    fallback.setScheme(QStringLiteral("http"));
    fallback.setPath(QStringLiteral("/video"));
    fallback.setQuery(QString());
    return fallback.toString();
}

QString CameraDiscovery::buildUrl(const QString &scheme, const QString &host, int port,
                                  const QString &user, const QString &password,
                                  const QString &path)
{
    const QString useScheme = scheme.isEmpty() ? QStringLiteral("rtsp") : scheme.toLower();

    QUrl url;
    url.setScheme(useScheme);
    url.setHost(host);

    // Стандартный порт схемы в адресе не пишем: короче и привычнее глазу.
    if (port > 0 && port != defaultPort(useScheme))
        url.setPort(port);

    if (!user.isEmpty()) {
        url.setUserName(user);
        if (!password.isEmpty())
            url.setPassword(password);
    }

    QString cleanPath = path;
    if (!cleanPath.isEmpty() && !cleanPath.startsWith(QLatin1Char('/')))
        cleanPath.prepend(QLatin1Char('/'));

    // Путь может нести параметры запроса — их надо отделить, иначе QUrl
    // закодирует знак вопроса, и камера такой адрес не поймёт.
    const int question = cleanPath.indexOf(QLatin1Char('?'));
    if (question >= 0) {
        url.setPath(cleanPath.left(question));
        url.setQuery(cleanPath.mid(question + 1));
    } else {
        url.setPath(cleanPath);
    }

    return url.toString();
}

bool CameraDiscovery::parseUrl(const QString &text, UrlParts *parts)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;

    // Схема обязательна. Без неё это просто имя узла, и разбирать нечего:
    // «192.168.1.126» — не адрес потока, а адрес камеры, он и так лежит в
    // своём поле.
    static const QRegularExpression schemeRe(
        QStringLiteral("^(rtsp|rtsps|http|https)://"),
        QRegularExpression::CaseInsensitiveOption);
    if (!schemeRe.match(trimmed).hasMatch())
        return false;

    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return false;

    if (!parts)
        return true;

    parts->scheme = url.scheme().toLower();
    parts->host = url.host();
    parts->port = url.port() > 0 ? url.port() : defaultPort(parts->scheme);
    parts->user = url.userName();
    parts->password = url.password();

    QString path = url.path();
    const QString query = url.query();
    if (!query.isEmpty())
        path += QLatin1Char('?') + query;
    parts->path = path;

    return true;
}

// ------------------------------------------------------------------- поиск

void CameraDiscovery::start()
{
    if (m_running)
        return;

    m_running = true;
    m_found.clear();
    m_queue.clear();
    m_queueHead = 0;
    m_active = 0;
    m_completed = 0;
    m_identifying = 0;
    m_scanDone = false;

    emit progress(QStringLiteral("Опрос камер по ONVIF…"), 5);
    probeOnvif();
}

void CameraDiscovery::stop()
{
    if (m_onvifTimer) {
        m_onvifTimer->stop();
        m_onvifTimer->deleteLater();
        m_onvifTimer = nullptr;
    }
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_queue.clear();
    m_queueHead = 0;
    m_running = false;
}

void CameraDiscovery::probeOnvif()
{
    m_socket = new QUdpSocket(this);
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_socket->bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::ShareAddress);
    connect(m_socket, &QUdpSocket::readyRead, this, &CameraDiscovery::readOnvifReplies);

    const QByteArray probe = onvifProbe();

    // Рассылаем с каждого сетевого интерфейса: на компьютере их обычно
    // несколько (проводной, беспроводной, виртуальные), и камера видна только
    // с того, что смотрит в её сеть.
    const auto networks = localNetworks();
    for (const auto &network : networks) {
        m_socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
        m_socket->writeDatagram(probe, QHostAddress(QStringLiteral("239.255.255.250")),
                                kOnvifPort);
        // И широковещательно по подсети — на случай, если многоадресная
        // рассылка в этой сети не проходит.
        const QHostAddress broadcast(network.first.toIPv4Address()
                                     | (0xFFFFFFFFu >> network.second));
        m_socket->writeDatagram(probe, broadcast, kOnvifPort);
    }

    m_onvifTimer = new QTimer(this);
    m_onvifTimer->setSingleShot(true);
    m_onvifTimer->setInterval(kOnvifWaitMs);
    connect(m_onvifTimer, &QTimer::timeout, this, &CameraDiscovery::finishOnvif);
    m_onvifTimer->start();
}

void CameraDiscovery::readOnvifReplies()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(int(m_socket->pendingDatagramSize()));
        QHostAddress sender;
        m_socket->readDatagram(data.data(), data.size(), &sender);

        const QString text = QString::fromUtf8(data);

        // Из ответа берём адрес службы устройства: он содержит настоящий
        // сетевой адрес камеры, который может отличаться от адреса
        // отправителя (например, за преобразованием адресов).
        static const QRegularExpression addressRe(
            QStringLiteral("https?://([0-9]{1,3}(?:\\.[0-9]{1,3}){3})"));
        const auto match = addressRe.match(text);
        const QString address = match.hasMatch()
                                    ? match.captured(1)
                                    : sender.toString().section(QLatin1Char(':'), -1);

        // Название модели, если камера его сообщила.
        QString description;
        static const QRegularExpression scopeRe(
            QStringLiteral("onvif://www\\.onvif\\.org/(?:name|hardware)/([^\\s<]+)"));
        auto scopes = scopeRe.globalMatch(text);
        QStringList parts;
        while (scopes.hasNext())
            parts << QUrl::fromPercentEncoding(scopes.next().captured(1).toUtf8());
        parts.removeDuplicates();
        description = parts.join(QLatin1Char(' '));

        remember(address, description, true, 0, 0);
    }
}

void CameraDiscovery::finishOnvif()
{
    emit progress(QStringLiteral("Проверка портов камер в сети…"), 35);
    beginPortScan();
}

// ------------------------------------------------------------- перебор портов

void CameraDiscovery::beginPortScan()
{
    // Опрашиваем подсеть текущего компьютера. Берём только сети /24 и уже:
    // перебирать шестьдесят пять тысяч адресов ради поиска камеры — не поиск,
    // а сканирование сети, и на объекте это заметит служба безопасности.
    QStringList targets;
    const auto networks = localNetworks();
    for (const auto &network : networks) {
        const int prefix = network.second;
        if (prefix < 24 || prefix > 32)
            continue;

        // Перебираем ровно ту сеть, в которой стоим.
        //
        // ЗДЕСЬ БЫЛА ОШИБКА, И ОНА ДОРОГО СТОИЛА. Маска бралась жёстко /24
        // независимо от настоящей длины префикса. Для сети /30 — а так выглядит
        // туннель VPN, который на компьютере оператора вполне может быть, —
        // это означало опрос 254 адресов вместо двух. Туннель отвечал
        // подтверждением на любой из них, и список находок наполнялся
        // двумя сотнями несуществующих камер.
        const quint32 span = 1u << (32 - prefix);
        const quint32 mask = prefix == 32 ? 0xFFFFFFFFu : ~(span - 1);
        const quint32 base = network.first.toIPv4Address() & mask;

        // В сетях /31 и /32 своих адресов сети и широковещания нет — там
        // перебираются все, какие есть.
        const quint32 first = span > 2 ? 1u : 0u;
        const quint32 last = span > 2 ? span - 1 : span;

        for (quint32 host = first; host < last; ++host) {
            const QHostAddress candidate(base | host);
            if (candidate == network.first)
                continue;
            targets.append(candidate.toString());
        }
    }
    targets.removeDuplicates();

    // Порядок проб выбран так, чтобы весь диапазон адресов проверялся по
    // одному порту, потом по следующему. Так самый вероятный порт (554)
    // отрабатывает целиком в начале, и первые находки появляются в списке
    // сразу, а не после полного перебора.
    const QVector<int> rtsp = rtspPorts();
    const QVector<int> http = httpPorts();
    m_queue.reserve(targets.size() * (rtsp.size() + http.size()));
    for (int port : rtsp) {
        for (const QString &address : targets)
            m_queue.append({address, port, false});
    }
    for (int port : http) {
        for (const QString &address : targets)
            m_queue.append({address, port, true});
    }

    if (m_queue.isEmpty()) {
        m_scanDone = true;
        finishScan();
        return;
    }

    pumpPortScan();
}

void CameraDiscovery::pumpPortScan()
{
    // Защита от повторного входа. Сокет обычно сообщает об ошибке через
    // событийный цикл, но не обязан: недостижимый адрес в своей же подсети
    // может отлуплаться сразу, прямо внутри connectToHost. Тогда проба
    // закрывается, не успев начаться, и подкачка вызывает саму себя — на
    // полутора тысячах проб это полторы тысячи вложенных вызовов.
    if (m_pumping)
        return;
    m_pumping = true;

    while (m_running && m_active < kMaxParallel && m_queueHead < m_queue.size()) {
        const Probe probe = m_queue.at(m_queueHead++);
        ++m_active;
        probeOnePort(probe);
    }

    m_pumping = false;

    if (m_queueHead >= m_queue.size() && m_active == 0 && !m_scanDone) {
        m_scanDone = true;
        finishScan();
    }
}

void CameraDiscovery::probeOnePort(const Probe &probe)
{
    auto *socket = new QTcpSocket(this);
    // Мимо любого прокси: адрес локальный, а на компьютере может быть поднят
    // туннель. Через него опрос своей же сети либо не пройдёт, либо уйдёт
    // наружу — чего в этой программе быть не должно вовсе.
    socket->setProxy(QNetworkProxy::NoProxy);

    auto *timeout = new QTimer(socket);
    timeout->setSingleShot(true);
    timeout->setInterval(kPortTimeoutMs);

    // Общий на оба исхода признак «уже посчитали»: сокет умеет сообщить и об
    // ошибке, и о срабатывании сторожа, а закрыть пробу надо ровно один раз.
    // Владение общее — признак переживает все три копии этой лямбды и умирает
    // вместе с последней.
    auto closed = QSharedPointer<bool>::create(false);

    const QString address = probe.address;
    const int port = probe.port;
    const bool http = probe.http;

    auto done = [this, socket, closed, address, port, http](bool open) {
        if (*closed)
            return;
        *closed = true;

        socket->deleteLater();

        if (open) {
            if (http) {
                // Открытый порт HTTP сам по себе ничего не значит — спросим,
                // камера ли это.
                ++m_identifying;
                identifyHttpCamera(address, port);
            } else {
                remember(address, QString(), false, port, 0);
            }
        }
        probeDone();
    };

    // Ответ камеры может прийти по частям, поэтому копим его.
    auto answer = QSharedPointer<QByteArray>::create();

    connect(socket, &QTcpSocket::connected, this, [socket, timeout, done, http, address, port] {
        if (http) {
            // По HTTP спрашиваем отдельно и позже: там нужен разбор ответа,
            // а не одна строка.
            timeout->stop();
            done(true);
            return;
        }

        // ОТКРЫТЫЙ ПОРТ — ЕЩЁ НЕ КАМЕРА, и это не придирка. Туннель VPN, а на
        // объекте и обычный посредник, подтверждает соединение на любой адрес,
        // после чего в списке оказываются сотни «камер», которых нет. Поэтому
        // спрашиваем то, на что умеет ответить только RTSP-сервер. Запрос
        // OPTIONS выбран потому, что на него отвечают до проверки пароля:
        // камера с неизвестным нам паролем всё равно назовётся.
        const QByteArray request = QStringLiteral(
            "OPTIONS rtsp://%1:%2/ RTSP/1.0\r\nCSeq: 1\r\n"
            "User-Agent: PoolSafety\r\n\r\n").arg(address).arg(port).toLatin1();
        socket->write(request);
        timeout->start(kRtspAnswerMs);
    });

    connect(socket, &QTcpSocket::readyRead, this, [socket, timeout, done, answer] {
        answer->append(socket->readAll());
        if (answer->size() < 5)
            return;   // ждём остаток строки, таймер ещё идёт
        timeout->stop();
        done(answer->startsWith("RTSP/"));
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [done, timeout](QAbstractSocket::SocketError) {
                timeout->stop();
                done(false);
            });
    connect(timeout, &QTimer::timeout, this, [socket, done] {
        socket->abort();
        done(false);
    });

    socket->connectToHost(address, quint16(port));
    timeout->start();
}

void CameraDiscovery::probeDone()
{
    --m_active;
    ++m_completed;

    if (m_completed % 50 == 0 && !m_queue.isEmpty()) {
        emit progress(QStringLiteral("Проверка портов камер в сети…"),
                      35 + 60 * m_completed / qMax(1, m_queue.size()));
    }

    pumpPortScan();
}

void CameraDiscovery::identifyHttpCamera(const QString &address, int port)
{
    auto *manager = findChild<QNetworkAccessManager *>(
        QStringLiteral("discoveryHttp"), Qt::FindDirectChildrenOnly);
    if (!manager) {
        manager = new QNetworkAccessManager(this);
        manager->setObjectName(QStringLiteral("discoveryHttp"));
        manager->setProxy(QNetworkProxy::NoProxy);
    }

    // Один короткий вопрос на адрес. «IP Webcam» отвечает на него своим
    // состоянием, обычная камера — хотя бы своим веб-сервером в заголовке,
    // а роутер не отвечает ни тем ни другим и в список не попадёт.
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(address);
    url.setPort(port);
    url.setPath(QStringLiteral("/status.json"));

    QNetworkRequest request(url);
    request.setTransferTimeout(kFingerprintMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));

    QNetworkReply *reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, address, port] {
        const QByteArray body = reply->readAll().left(4096);
        const QByteArray server = reply->rawHeader("Server");
        reply->deleteLater();

        QString description;
        if (looksLikeCamera(server, body, &description)) {
            if (description == QStringLiteral("Смартфон (IP Webcam)")) {
                // Путь известен точно — подставим готовый адрес.
                //
                // БЕРЁМ MJPEG, А НЕ H.264, хотя H.264 втрое легче для сети.
                // H.264 отдаётся по RTSP, а тот шлёт видео отдельными пакетами
                // UDP: при включённой VPN или втором сетевом адресе в той же
                // подсети они не доходят, и камера, которая только что
                // нашлась, показывает пустоту. MJPEG идёт по тому же
                // соединению, что и страница камеры, и доходит всегда.
                rememberStream(address, description, QStringLiteral("http"), port,
                               QStringLiteral("/video"));
            } else {
                remember(address, description, false, 0, port);
            }
        }

        --m_identifying;
        finishScan();
    });
}

void CameraDiscovery::finishScan()
{
    if (!m_running || !m_scanDone || m_identifying > 0)
        return;

    emit progress(QStringLiteral("Поиск завершён"), 100);
    m_running = false;
    emit finished(m_found.size());
}

// -------------------------------------------------------------- накопление

void CameraDiscovery::remember(const QString &address, const QString &description,
                               bool onvif, int rtspPort, int httpPort)
{
    if (address.isEmpty())
        return;

    for (DiscoveredCamera &camera : m_found) {
        if (camera.address != address)
            continue;
        // Уже находили другим способом — дополняем сведения.
        bool changed = false;
        if (onvif && !camera.onvif) {
            camera.onvif = true;
            changed = true;
        }
        if (rtspPort > 0 && !camera.rtspPorts.contains(rtspPort)) {
            camera.rtspPorts.append(rtspPort);
            changed = true;
        }
        if (httpPort > 0 && !camera.httpPorts.contains(httpPort)) {
            camera.httpPorts.append(httpPort);
            changed = true;
        }
        if (camera.description.isEmpty() && !description.isEmpty()) {
            camera.description = description;
            changed = true;
        }
        if (changed)
            emit cameraFound(camera);
        return;
    }

    DiscoveredCamera camera;
    camera.address = address;
    camera.description = description;
    camera.onvif = onvif;
    if (rtspPort > 0)
        camera.rtspPorts.append(rtspPort);
    if (httpPort > 0)
        camera.httpPorts.append(httpPort);
    m_found.append(camera);
    emit cameraFound(camera);
}

void CameraDiscovery::rememberStream(const QString &address, const QString &description,
                                     const QString &scheme, int port, const QString &path)
{
    remember(address, description, false, 0, port);

    for (DiscoveredCamera &camera : m_found) {
        if (camera.address != address)
            continue;
        camera.scheme = scheme;
        camera.port = port;
        camera.path = path;
        emit cameraFound(camera);
        return;
    }
}

} // namespace core
