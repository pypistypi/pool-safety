#pragma once

// ---------------------------------------------------------------------------
//  Связь с телефонами в локальной сети.
//
//  ЧТО ЗДЕСЬ ХОДИТ ПО СЕТИ, А ЧТО НЕТ. По этому каналу передаются ТОЛЬКО
//  события и сведения о камерах: «на панели 2 появились люди», «на панели 3
//  тревога», «камера панели 1 живёт по такому адресу». Изображение здесь не
//  ходит НИКОГДА — телефон берёт видео прямо с камеры.
//
//  Так сделано по прямому требованию заказчика, и оно правильное: гнать
//  видеопоток сначала на компьютер, а оттуда на телефоны означало бы
//  пропустить его через сеть дважды и нагрузить компьютер перекодированием.
//  Камера и так раздаёт поток всем желающим — пусть телефон берёт его сам.
//
//  КАНАЛ ДВУСТОРОННИЙ. Сначала он только вещал, и это была ошибка замысла:
//  спасатель с телефоном в руках у самой воды видел тревогу, но выключить её
//  мог только тот, кто стоит у компьютера. Сирена при этом продолжает выть.
//  Теперь телефон присылает те же решения, что доступны оператору на посту:
//  «принял», «ложная», «подтверждаю».
//
//  ПРОТОКОЛ НАМЕРЕННО ПРОСТОЙ: строки JSON, разделённые переводом строки.
//  Такой разбирается на любой стороне без библиотек и читается человеком при
//  отладке — достаточно подключиться telnet-ом и посмотреть глазами.
//
//  ОБНАРУЖЕНИЕ. Телефону не нужно спрашивать адрес компьютера: он шлёт
//  широковещательный запрос, компьютер отвечает. Настройка сети со стороны
//  пользователя не требуется вовсе.
//
//  БЕЗ ВЫХОДА В ИНТЕРНЕТ. Сервер слушает только локальную сеть; никаких
//  обращений наружу, никаких облаков.
// ---------------------------------------------------------------------------

#include "core/AnalysisTypes.h"

#include <QObject>
#include <QVector>
#include <QHash>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

namespace core {

/// Что известно о панели: как называется зона и откуда телефону брать видео.
struct PanelInfo {
    int panelId = -1;
    QString zone;
    QString streamUrl;   ///< адрес потока камеры, пустой — источник не сетевой
    int peopleCount = 0;
    Level level = Level::Normal;
};

class EventServer : public QObject
{
    Q_OBJECT

public:
    /// Порт для подключения телефонов.
    static constexpr quint16 kDefaultPort = 8765;

    /// Порт, на котором сервер откликается на широковещательный запрос.
    static constexpr quint16 kDiscoveryPort = 8766;

    explicit EventServer(QObject *parent = nullptr);
    ~EventServer() override;

    /// Запустить. false — порт занят, причина в lastError().
    bool start(quint16 port = kDefaultPort);
    void stop();
    bool isRunning() const;

    QString lastError() const { return m_error; }
    int clientCount() const;

    /// Адрес, который надо вписать в телефоне вручную, если обнаружение не
    /// сработало (например, телефон в другой подсети).
    QString localAddress() const;

    /// Что телефон может сделать с тревогой. Ровно то же, что и оператор на
    /// посту: других решений в системе нет, и придумывать их для телефона
    /// нельзя — оператор и спасатель должны понимать друг друга.
    enum class Command {
        Acknowledge,   ///< принял, выключить сирену
        FalseAlarm,    ///< тревога ложная
        Confirm,       ///< тревога настоящая, помощь вызвана
        Close          ///< происшествие завершено, можно возвращаться к наблюдению
    };

public slots:
    /// Сообщить телефонам, в каком состоянии тревога.
    ///
    /// БЕЗ ЭТОГО УВЕДОМЛЕНИЕ НЕ СНИМАЕТСЯ. Оператор погасил тревогу на посту,
    /// а на телефоне она продолжала висеть красным до следующего события —
    /// то есть спасатель считал, что беда ещё идёт.
    void publishAlarmState(const QString &state, int panelId);

    /// Обновить сведения о панелях. Рассылается подключённым телефонам.
    void setPanels(const QVector<core::PanelInfo> &panels);

    /// Событие: люди в зоне, признаки опасности, тревога.
    void publishEvent(const QString &kind, int panelId, const QString &zone,
                      const QString &title, const QString &details, int level);

signals:
    void clientsChanged(int count);

    /// Телефон принял решение по тревоге.
    void commandReceived(core::EventServer::Command command);

private slots:
    void acceptClient();
    void dropClient();
    void readDiscovery();
    void readFromClient();

private:
    void send(QTcpSocket *client, const QByteArray &line);
    QByteArray alarmStateMessage() const;
    void broadcast(const QByteArray &line);
    QByteArray panelsMessage() const;

    QTcpServer *m_server = nullptr;
    QUdpSocket *m_discovery = nullptr;
    QVector<QTcpSocket *> m_clients;
    QVector<PanelInfo> m_panels;

    /// Последнее состояние тревоги — чтобы рассказать о нём телефону, который
    /// подключился уже посреди происшествия.
    QString m_alarmState = QStringLiteral("idle");
    int m_alarmPanel = -1;

    QHash<QTcpSocket *, QByteArray> m_incoming;   ///< недочитанные строки
    QString m_error;
    quint16 m_port = kDefaultPort;
};

} // namespace core

Q_DECLARE_METATYPE(core::EventServer::Command)
