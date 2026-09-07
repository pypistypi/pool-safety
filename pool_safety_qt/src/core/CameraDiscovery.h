#pragma once

// ---------------------------------------------------------------------------
//  Поиск IP-камер в локальной сети.
//
//  ТРИ СПОСОБА СРАЗУ, потому что ни один по отдельности не находит всё.
//
//    1. WS-Discovery (ONVIF). Многоадресный запрос на 239.255.255.250:3702, на
//       который откликаются камеры, поддерживающие ONVIF. Способ быстрый и
//       вежливый, но отвечают не все: дешёвые камеры ONVIF не умеют, а в сетях
//       с разделением клиентов многоадресная рассылка не проходит вовсе.
//    2. Опрос портов RTSP по подсети (554, 8554). Прямая проверка: если на
//       адресе открыт порт RTSP, там почти наверняка камера. Медленнее и
//       грубее, зато находит то, что молчит на ONVIF.
//    3. Опрос портов HTTP (8080, 8000, 88, 81) с проверкой отпечатка.
//       Открытый порт 8080 сам по себе камерой не является — так же слушают
//       роутеры, принтеры и любой ноутбук с отладочным сервером. Поэтому по
//       такому адресу задаётся один короткий вопрос, и в список попадают
//       только те, кто ответил как камера.
//
//  ПОЧЕМУ ПОРТ 554 НЕ ЕДИНСТВЕННЫЙ. Смартфон в роли камеры (приложение
//  «IP Webcam» и подобные) слушает 8080 и отдаёт по нему сразу два потока:
//  MJPEG по HTTP и H.264 по RTSP. Пока опрашивался только 554, такая камера не
//  находилась никогда, хотя в браузере открывалась и работала.
//
//  ЧТО ЭТОТ КЛАСС НЕ ДЕЛАЕТ. Он не выясняет адрес видеопотока у произвольной
//  камеры: для этого нужен полноценный запрос ONVIF с подписью учётных данных,
//  а у каждого производителя свои особенности. Поэтому найденные камеры
//  показываются оператору, а путь потока подставляется по шаблону
//  производителя и проверяется пробным подключением — честнее, чем делать вид,
//  что угадали. Исключение одно: камеру, которая назвалась сама (отпечаток
//  совпал), сопровождаем готовым адресом — тут гадать не о чем.
//
//  ВСЁ ПРОИСХОДИТ ТОЛЬКО В ЛОКАЛЬНОЙ СЕТИ. Ни одного обращения наружу.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QHostAddress>
#include <QString>
#include <QVector>

class QUdpSocket;
class QTimer;

namespace core {

/// Найденная камера.
struct DiscoveredCamera {
    QString address;        ///< адрес в сети, например 192.168.1.64
    QString description;    ///< что удалось узнать: производитель, модель
    bool onvif = false;     ///< откликнулась на ONVIF
    QVector<int> rtspPorts; ///< открытые порты RTSP (554, 8554…)
    QVector<int> httpPorts; ///< порты HTTP, откликнувшиеся как камера

    /// Готовый адрес потока, если камера опозналась по отпечатку. Пусто, если
    /// путь неизвестен и его должен выбрать оператор.
    QString scheme;   ///< rtsp либо http
    int port = 0;
    QString path;

    bool hasRtsp() const { return !rtspPorts.isEmpty(); }
    bool knowsStream() const { return !scheme.isEmpty() && !path.isEmpty(); }

    QString title() const;
};

class CameraDiscovery : public QObject
{
    Q_OBJECT

public:
    explicit CameraDiscovery(QObject *parent = nullptr);
    ~CameraDiscovery() override;

    /// Начать поиск. Сигнал finished() придёт примерно через десять секунд.
    void start();
    void stop();
    bool isRunning() const { return m_running; }

    /// Порты, которые опрашиваются. Вынесены сюда, чтобы список был виден и
    /// проверяем, а не спрятан в глубине перебора.
    static QVector<int> rtspPorts();
    static QVector<int> httpPorts();

    /// Готовые шаблоны адреса потока для распространённых источников.
    ///
    /// Список не претендует на полноту и не должен: у камеры всегда можно
    /// посмотреть путь в её собственных настройках, а сюда собрано то, что
    /// покрывает большинство случаев.
    ///
    /// Схема и порт входят в шаблон, а не подразумеваются: смартфон отдаёт
    /// поток по HTTP на 8080, и «протокол всегда rtsp, порт всегда 554» — то
    /// самое допущение, из-за которого такая камера не подключалась.
    struct UrlTemplate {
        QString vendor;
        QString scheme;   ///< rtsp либо http
        int port = 0;     ///< 0 — оставить текущий
        QString path;
        QString note;
    };
    static QVector<UrlTemplate> urlTemplates();

    /// Стандартный порт схемы: 554 для rtsp, 80 для http. Такой порт в адресе
    /// не пишется.
    static int defaultPort(const QString &scheme);

    /// Запасной адрес того же потока по HTTP, если он известен.
    ///
    /// ЗАЧЕМ ЭТО ВООБЩЕ НУЖНО. RTSP передаёт видео отдельными пакетами UDP, а
    /// не по тому соединению, через которое договаривался. Стоит между
    /// программой и камерой оказаться чему-нибудь чуть сложнее прямого
    /// провода — включённой VPN, второму сетевому адресу в той же подсети,
    /// строгому межсетевому экрану, — и получается худшее из положений:
    /// камера найдена, соединение установлено, а кадры не приходят. Со
    /// стороны это выглядит как «программа не видит камеру», хотя видит.
    ///
    /// MJPEG по HTTP идёт целиком по одному соединению TCP и доходит везде,
    /// куда доходит сама страница камеры. Он тяжелее для сети, зато
    /// включается мгновенно и не теряется.
    ///
    /// Пустая строка — запасного пути для этого адреса нет.
    static QString httpFallbackFor(const QString &streamUrl);

    /// Собрать адрес потока из частей.
    static QString buildUrl(const QString &scheme, const QString &host, int port,
                            const QString &user, const QString &password,
                            const QString &path);

    /// Разобрать полный адрес на части. Нужно, чтобы оператор мог вставить
    /// готовую ссылку из браузера или из переписки целиком, а не разносить её
    /// по полям вручную, ошибаясь в порту и в пути.
    ///
    /// Возвращает false, если строка на адрес потока не похожа.
    struct UrlParts {
        QString scheme;
        QString host;
        int port = 0;
        QString user;
        QString password;
        QString path;
    };
    static bool parseUrl(const QString &text, UrlParts *parts);

signals:
    void cameraFound(const core::DiscoveredCamera &camera);
    void progress(const QString &stage, int percent);
    void finished(int found);

private slots:
    void readOnvifReplies();
    void finishOnvif();

private:
    /// Одна проба: адрес, порт и то, чем этот порт считается.
    struct Probe {
        QString address;
        int port = 0;
        bool http = false;
    };

    void probeOnvif();
    void beginPortScan();
    void pumpPortScan();
    void probeOnePort(const Probe &probe);
    void identifyHttpCamera(const QString &address, int port);
    void probeDone();
    void finishScan();

    void remember(const QString &address, const QString &description, bool onvif,
                  int rtspPort, int httpPort);
    void rememberStream(const QString &address, const QString &description,
                        const QString &scheme, int port, const QString &path);

    QUdpSocket *m_socket = nullptr;
    QTimer *m_onvifTimer = nullptr;
    QVector<DiscoveredCamera> m_found;
    bool m_running = false;

    QVector<Probe> m_queue;      ///< что осталось проверить
    int m_queueHead = 0;         ///< докуда дошли
    int m_active = 0;            ///< сколько проб в работе прямо сейчас
    int m_completed = 0;         ///< сколько закончено — для полосы хода
    int m_identifying = 0;       ///< сколько отпечатков ещё выясняется
    bool m_scanDone = false;     ///< перебор портов закончен
    bool m_pumping = false;      ///< подкачка очереди уже идёт
};

} // namespace core

Q_DECLARE_METATYPE(core::DiscoveredCamera)
