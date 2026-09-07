#include "core/EventServer.h"

#include "core/Version.h"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

namespace core {

namespace {

/// Слово, по которому телефон узнаёт наш ответ среди прочего сетевого шума.
const QByteArray kDiscoveryRequest = "POOLSAFETY-DISCOVER";

QByteArray toLine(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace

EventServer::EventServer(QObject *parent)
    : QObject(parent)
{
}

EventServer::~EventServer()
{
    stop();
}

bool EventServer::start(quint16 port)
{
    stop();
    m_port = port;

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &EventServer::acceptClient);

    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        m_error = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    // Ответчик на широковещательный запрос телефона.
    m_discovery = new QUdpSocket(this);
    if (m_discovery->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                          QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        connect(m_discovery, &QUdpSocket::readyRead, this, &EventServer::readDiscovery);
    } else {
        // Не беда: телефон сможет подключиться по адресу, введённому вручную.
        // Но сказать об этом надо — молча терять возможность нельзя.
        m_error = QStringLiteral("обнаружение недоступно: %1")
                      .arg(m_discovery->errorString());
        delete m_discovery;
        m_discovery = nullptr;
    }

    return true;
}

void EventServer::stop()
{
    for (QTcpSocket *client : std::as_const(m_clients)) {
        client->disconnectFromHost();
        client->deleteLater();
    }
    m_clients.clear();
    m_incoming.clear();

    if (m_discovery) {
        m_discovery->close();
        delete m_discovery;
        m_discovery = nullptr;
    }
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

bool EventServer::isRunning() const
{
    return m_server && m_server->isListening();
}

int EventServer::clientCount() const
{
    return int(m_clients.size());
}

QString EventServer::localAddress() const
{
    // Берём первый адрес IPv4 в локальной сети — именно его оператор впишет в
    // телефоне, если обнаружение не пройдёт.
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)
            || interface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const auto entries = interface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol
                && !address.isLoopback()) {
                return QStringLiteral("%1:%2").arg(address.toString()).arg(m_port);
            }
        }
    }
    return QStringLiteral("адрес не определён:%1").arg(m_port);
}

// ------------------------------------------------------------------ клиенты

void EventServer::acceptClient()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *client = m_server->nextPendingConnection();
        m_clients.append(client);

        connect(client, &QTcpSocket::disconnected, this, &EventServer::dropClient);
        connect(client, &QTcpSocket::readyRead, this, &EventServer::readFromClient);

        // Телефон, только что подключившийся, должен сразу узнать, что у нас
        // происходит: какие панели есть, где их камеры и что там сейчас. Иначе
        // до первого события он показывал бы пустой экран.
        send(client, panelsMessage());

        // ТЕЛЕФОН МОГ ПОДКЛЮЧИТЬСЯ ПОСРЕДИ ТРЕВОГИ. Состояние рассылается
        // при изменении, и тот, кто пришёл позже, о нём бы не узнал: экран
        // показывал бы спокойную обстановку, а на посту в это время воет
        // сирена. Поэтому новому клиенту состояние отправляется сразу.
        send(client, alarmStateMessage());

        QJsonObject hello;
        hello.insert(QStringLiteral("type"), QStringLiteral("hello"));
        hello.insert(QStringLiteral("app"), QStringLiteral("PoolSafety"));
        hello.insert(QStringLiteral("version"),
                     QString::fromLatin1(kAppVersion));
        hello.insert(QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate));
        send(client, toLine(hello));

        emit clientsChanged(clientCount());
    }
}

void EventServer::readFromClient()
{
    auto *client = qobject_cast<QTcpSocket *>(sender());
    if (!client)
        return;

    QByteArray &buffer = m_incoming[client];
    buffer += client->readAll();

    // Разбираем построчно: телефон может прислать несколько решений подряд,
    // а одна строка — прийти по частям.
    int newline = buffer.indexOf('\n');
    while (newline >= 0) {
        const QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        newline = buffer.indexOf('\n');

        const QJsonObject message = QJsonDocument::fromJson(line).object();
        if (message.value(QStringLiteral("type")).toString()
            != QStringLiteral("command")) {
            continue;
        }

        const QString action = message.value(QStringLiteral("action")).toString();
        if (action == QLatin1String("acknowledge"))
            emit commandReceived(Command::Acknowledge);
        else if (action == QLatin1String("false_alarm"))
            emit commandReceived(Command::FalseAlarm);
        else if (action == QLatin1String("confirm"))
            emit commandReceived(Command::Confirm);
        else if (action == QLatin1String("close"))
            emit commandReceived(Command::Close);
    }

    // Строка без перевода строки не может быть бесконечной: это либо сбой,
    // либо чужая программа, случайно подключившаяся к порту.
    if (buffer.size() > 8192)
        buffer.clear();
}

void EventServer::dropClient()
{
    auto *client = qobject_cast<QTcpSocket *>(sender());
    if (!client)
        return;
    m_incoming.remove(client);
    m_clients.removeAll(client);
    client->deleteLater();
    emit clientsChanged(clientCount());
}

void EventServer::readDiscovery()
{
    while (m_discovery && m_discovery->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(int(m_discovery->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_discovery->readDatagram(data.data(), data.size(), &sender, &senderPort);

        if (!data.startsWith(kDiscoveryRequest))
            continue;

        QJsonObject reply;
        reply.insert(QStringLiteral("app"), QStringLiteral("PoolSafety"));
        reply.insert(QStringLiteral("port"), int(m_port));
        reply.insert(QStringLiteral("panels"), int(m_panels.size()));
        m_discovery->writeDatagram(QJsonDocument(reply).toJson(QJsonDocument::Compact),
                                   sender, senderPort);
    }
}

// ------------------------------------------------------------------ отправка

void EventServer::send(QTcpSocket *client, const QByteArray &line)
{
    if (!client || client->state() != QAbstractSocket::ConnectedState)
        return;
    client->write(line);
    client->flush();
}

void EventServer::broadcast(const QByteArray &line)
{
    for (QTcpSocket *client : std::as_const(m_clients))
        send(client, line);
}

QByteArray EventServer::panelsMessage() const
{
    QJsonArray array;
    for (const PanelInfo &panel : m_panels) {
        QJsonObject item;
        item.insert(QStringLiteral("id"), panel.panelId);
        item.insert(QStringLiteral("zone"), panel.zone);
        item.insert(QStringLiteral("stream"), panel.streamUrl);
        item.insert(QStringLiteral("people"), panel.peopleCount);
        item.insert(QStringLiteral("level"), int(panel.level));
        array.append(item);
    }

    QJsonObject message;
    message.insert(QStringLiteral("type"), QStringLiteral("panels"));
    message.insert(QStringLiteral("items"), array);
    return toLine(message);
}

void EventServer::setPanels(const QVector<PanelInfo> &panels)
{
    m_panels = panels;
    if (isRunning())
        broadcast(panelsMessage());
}

QByteArray EventServer::alarmStateMessage() const
{
    QJsonObject message;
    message.insert(QStringLiteral("type"), QStringLiteral("alarm_state"));
    message.insert(QStringLiteral("state"), m_alarmState);
    message.insert(QStringLiteral("panel"), m_alarmPanel);
    message.insert(QStringLiteral("time"),
                   QDateTime::currentDateTime().toString(Qt::ISODate));
    return toLine(message);
}

void EventServer::publishAlarmState(const QString &state, int panelId)
{
    // Запоминаем всегда, даже когда сервер выключен: телефон подключится
    // позже, и состояние должно быть верным с первой секунды.
    m_alarmState = state;
    m_alarmPanel = panelId;

    if (!isRunning())
        return;

    QJsonObject message;
    message.insert(QStringLiteral("type"), QStringLiteral("alarm_state"));
    message.insert(QStringLiteral("state"), state);   // idle | raised | acknowledged | confirmed
    message.insert(QStringLiteral("panel"), panelId);
    message.insert(QStringLiteral("time"),
                   QDateTime::currentDateTime().toString(Qt::ISODate));
    broadcast(toLine(message));
}

void EventServer::publishEvent(const QString &kind, int panelId, const QString &zone,
                               const QString &title, const QString &details, int level)
{
    if (!isRunning())
        return;

    QJsonObject message;
    message.insert(QStringLiteral("type"), QStringLiteral("event"));
    message.insert(QStringLiteral("kind"), kind);          // presence | attention | alarm | clear
    message.insert(QStringLiteral("panel"), panelId);
    message.insert(QStringLiteral("zone"), zone);
    message.insert(QStringLiteral("title"), title);
    message.insert(QStringLiteral("details"), details);
    message.insert(QStringLiteral("level"), level);        // 0 норма, 1 внимание, 2 тревога
    message.insert(QStringLiteral("time"),
                   QDateTime::currentDateTime().toString(Qt::ISODate));
    broadcast(toLine(message));
}

} // namespace core
