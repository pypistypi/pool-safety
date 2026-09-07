#include "ui/IpCameraDialog.h"

#include "core/CameraDiscovery.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>

namespace ui {

namespace {
constexpr int kTestTimeoutMs = 8000;
}

IpCameraDialog::IpCameraDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Подключение IP-камеры"));
    setMinimumSize(720, 660);

    auto *root = new QVBoxLayout(this);

    // --- поиск в сети -----------------------------------------------------
    auto *searchBox = new QGroupBox(QStringLiteral("Найти камеры в сети"), this);
    auto *searchLayout = new QVBoxLayout(searchBox);

    QStringList portList;
    for (int port : core::CameraDiscovery::rtspPorts())
        portList << QString::number(port);
    for (int port : core::CameraDiscovery::httpPorts())
        portList << QString::number(port);

    auto *hint = new QLabel(QStringLiteral(
        "Программа опросит локальную сеть: сначала по ONVIF, затем проверит "
        "порты %1. Поиск занимает около десяти секунд и никуда за пределы "
        "вашей сети не обращается.").arg(portList.join(QStringLiteral(", "))),
        searchBox);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #939fb0;"));
    searchLayout->addWidget(hint);

    auto *searchRow = new QHBoxLayout;
    m_search = new QPushButton(QStringLiteral("Начать поиск"), searchBox);
    connect(m_search, &QPushButton::clicked, this, &IpCameraDialog::startSearch);
    searchRow->addWidget(m_search);

    m_progress = new QProgressBar(searchBox);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_progress->setFormat(QStringLiteral("готов к поиску"));
    searchRow->addWidget(m_progress, 1);
    searchLayout->addLayout(searchRow);

    m_list = new QListWidget(searchBox);
    m_list->setMinimumHeight(110);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &IpCameraDialog::useSelected);
    searchLayout->addWidget(m_list);

    root->addWidget(searchBox);

    // --- параметры подключения -------------------------------------------
    auto *manual = new QGroupBox(QStringLiteral("Параметры подключения"), this);
    auto *form = new QFormLayout(manual);

    m_host = new QLineEdit(manual);
    m_host->setPlaceholderText(
        QStringLiteral("192.168.1.64 — или вставьте сюда готовый адрес целиком"));
    // Разбор ссылки — по завершении ввода, а не на каждой букве: иначе на
    // третьем символе строки «http://192…» поле схлопнулось бы в «1».
    connect(m_host, &QLineEdit::textChanged, this, &IpCameraDialog::updatePreviewUrl);
    connect(m_host, &QLineEdit::editingFinished, this, &IpCameraDialog::hostTextChanged);
    form->addRow(QStringLiteral("Адрес камеры:"), m_host);

    // Протокол. Порядок не случаен: RTSP стоит первым, потому что он легче для
    // сети и его же понимает телефон, а MJPEG хорош как запасной — включается
    // мгновенно, но гонит несжатый поток.
    m_scheme = new QComboBox(manual);
    m_scheme->addItem(QStringLiteral("RTSP — H.264, основной"), QStringLiteral("rtsp"));
    m_scheme->addItem(QStringLiteral("HTTP — MJPEG, запасной"), QStringLiteral("http"));
    form->addRow(QStringLiteral("Протокол:"), m_scheme);

    m_port = new QSpinBox(manual);
    m_port->setRange(1, 65535);
    m_port->setValue(554);
    form->addRow(QStringLiteral("Порт:"), m_port);

    m_user = new QLineEdit(manual);
    m_user->setPlaceholderText(QStringLiteral("admin"));
    form->addRow(QStringLiteral("Пользователь:"), m_user);

    m_password = new QLineEdit(manual);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Пароль:"), m_password);

    m_vendor = new QComboBox(manual);
    const auto templates = core::CameraDiscovery::urlTemplates();
    for (const auto &item : templates) {
        m_vendor->addItem(item.note.isEmpty()
                              ? item.vendor
                              : QStringLiteral("%1 — %2").arg(item.vendor, item.note),
                          item.path);
    }
    connect(m_vendor, &QComboBox::currentIndexChanged, this, &IpCameraDialog::applyTemplate);
    form->addRow(QStringLiteral("Производитель:"), m_vendor);

    m_path = new QLineEdit(manual);
    m_path->setPlaceholderText(QStringLiteral("/Streaming/Channels/102"));
    form->addRow(QStringLiteral("Путь потока:"), m_path);

    m_name = new QLineEdit(manual);
    m_name->setPlaceholderText(QStringLiteral("Например: чаша, вид с востока"));
    form->addRow(QStringLiteral("Название:"), m_name);

    root->addWidget(manual);

    // --- итоговый адрес и проверка ---------------------------------------
    m_urlPreview = new QLabel(this);
    m_urlPreview->setWordWrap(true);
    m_urlPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_urlPreview->setStyleSheet(QStringLiteral("color: #939fb0;"));
    root->addWidget(m_urlPreview);

    auto *testRow = new QHBoxLayout;
    m_test = new QPushButton(QStringLiteral("Проверить связь"), this);
    connect(m_test, &QPushButton::clicked, this, &IpCameraDialog::testConnection);
    testRow->addWidget(m_test);

    m_testResult = new QLabel(this);
    m_testResult->setWordWrap(true);
    testRow->addWidget(m_testResult, 1);
    root->addLayout(testRow);

    // --- кнопки -----------------------------------------------------------
    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(QStringLiteral("Подключить"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    for (QLineEdit *field : {m_user, m_password, m_path})
        connect(field, &QLineEdit::textChanged, this, &IpCameraDialog::updatePreviewUrl);
    connect(m_port, &QSpinBox::valueChanged, this, &IpCameraDialog::updatePreviewUrl);
    connect(m_scheme, &QComboBox::currentIndexChanged, this,
            &IpCameraDialog::updatePreviewUrl);

    applyTemplate(0);
    updatePreviewUrl();
}

IpCameraDialog::~IpCameraDialog()
{
    if (m_discovery)
        m_discovery->stop();
    stopTest(QString(), false);
}

// ------------------------------------------------------------------- поиск

void IpCameraDialog::startSearch()
{
    if (!m_discovery) {
        m_discovery = new core::CameraDiscovery(this);
        connect(m_discovery, &core::CameraDiscovery::cameraFound,
                this, &IpCameraDialog::onCameraFound);
        connect(m_discovery, &core::CameraDiscovery::finished,
                this, &IpCameraDialog::onSearchFinished);
        connect(m_discovery, &core::CameraDiscovery::progress, this,
                [this](const QString &stage, int percent) {
                    m_progress->setValue(percent);
                    m_progress->setFormat(stage);
                });
    }
    if (m_discovery->isRunning())
        return;

    m_list->clear();
    m_search->setEnabled(false);
    m_discovery->start();
}

void IpCameraDialog::onCameraFound(const core::DiscoveredCamera &camera)
{
    // Список пополняется по мере находок, а не в конце: оператор видит, что
    // поиск идёт, и может выбрать первую же камеру, не дожидаясь остальных.
    QListWidgetItem *item = nullptr;
    for (int row = 0; row < m_list->count(); ++row) {
        if (m_list->item(row)->data(Qt::UserRole).toString() == camera.address) {
            item = m_list->item(row);
            break;
        }
    }
    if (!item) {
        item = new QListWidgetItem(camera.title(), m_list);
        item->setData(Qt::UserRole, camera.address);
    }
    item->setText(camera.title());

    // Всё, что камера сообщила о себе сама, храним рядом со строкой: при
    // выборе оператору не придётся ни гадать про порт, ни искать путь.
    item->setData(Qt::UserRole + 1, camera.scheme);
    item->setData(Qt::UserRole + 2, camera.port);
    item->setData(Qt::UserRole + 3, camera.path);
    item->setData(Qt::UserRole + 4, camera.description);
    item->setData(Qt::UserRole + 5,
                  camera.rtspPorts.isEmpty() ? 0 : camera.rtspPorts.first());
    item->setData(Qt::UserRole + 6,
                  camera.httpPorts.isEmpty() ? 0 : camera.httpPorts.first());
}

void IpCameraDialog::onSearchFinished(int found)
{
    m_search->setEnabled(true);
    m_progress->setValue(100);
    m_progress->setFormat(found > 0
                              ? QStringLiteral("найдено камер: %1").arg(found)
                              : QStringLiteral("камеры не найдены — введите адрес вручную"));
}

void IpCameraDialog::useSelected()
{
    const auto items = m_list->selectedItems();
    if (items.isEmpty())
        return;
    const QListWidgetItem *item = items.first();

    const QString address = item->data(Qt::UserRole).toString();
    const QString scheme = item->data(Qt::UserRole + 1).toString();
    const int streamPort = item->data(Qt::UserRole + 2).toInt();
    const QString path = item->data(Qt::UserRole + 3).toString();
    const QString description = item->data(Qt::UserRole + 4).toString();
    const int rtspPort = item->data(Qt::UserRole + 5).toInt();
    const int httpPort = item->data(Qt::UserRole + 6).toInt();

    if (!scheme.isEmpty() && !path.isEmpty()) {
        // Камера назвала себя сама — берём её адрес целиком.
        setFields(scheme, address, streamPort, m_user->text(), m_password->text(), path);
    } else {
        // Путь неизвестен. Подставляем адрес и тот порт, что откликнулся, а
        // путь оператор выберет по производителю.
        const int port = rtspPort > 0 ? rtspPort : (httpPort > 0 ? httpPort : m_port->value());
        setFields(rtspPort > 0 ? QStringLiteral("rtsp") : currentScheme(), address, port,
                  m_user->text(), m_password->text(), m_path->text());
    }

    if (m_name->text().isEmpty())
        m_name->setText(description.isEmpty() ? address : description);

    updatePreviewUrl();
}

// --------------------------------------------------------------- параметры

QString IpCameraDialog::currentScheme() const
{
    const QString scheme = m_scheme->currentData().toString();
    return scheme.isEmpty() ? QStringLiteral("rtsp") : scheme;
}

void IpCameraDialog::setFields(const QString &scheme, const QString &host, int port,
                               const QString &user, const QString &password,
                               const QString &path)
{
    m_filling = true;

    const QSignalBlocker blockHost(m_host);
    const QSignalBlocker blockScheme(m_scheme);
    const QSignalBlocker blockPort(m_port);
    const QSignalBlocker blockUser(m_user);
    const QSignalBlocker blockPassword(m_password);
    const QSignalBlocker blockPath(m_path);

    m_host->setText(host);
    if (!scheme.isEmpty()) {
        const int index = m_scheme->findData(scheme);
        m_scheme->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (port > 0)
        m_port->setValue(port);
    m_user->setText(user);
    m_password->setText(password);
    m_path->setText(path);

    m_filling = false;
}

void IpCameraDialog::hostTextChanged()
{
    if (m_filling) {
        updatePreviewUrl();
        return;
    }

    // Оператор мог вставить готовую ссылку целиком. Разбираем её по полям —
    // это самый частый способ подключить камеру: адрес уже есть в браузере
    // или в переписке, и переносить его руками незачем.
    core::CameraDiscovery::UrlParts parts;
    if (core::CameraDiscovery::parseUrl(m_host->text(), &parts)) {
        setFields(parts.scheme, parts.host, parts.port, parts.user, parts.password,
                  parts.path);
        // Путь пришёл из ссылки, а не из шаблона — покажем это списком.
        const QSignalBlocker blockVendor(m_vendor);
        m_vendor->setCurrentIndex(m_vendor->count() - 1);   // «Ввести вручную»
    }

    updatePreviewUrl();
}

void IpCameraDialog::applyTemplate(int index)
{
    const auto templates = core::CameraDiscovery::urlTemplates();
    if (index < 0 || index >= templates.size())
        return;
    const auto &item = templates.at(index);

    // «Ввести вручную» ничего не навязывает: у пункта пуст и путь, и схема.
    if (item.path.isEmpty() && item.scheme.isEmpty()) {
        updatePreviewUrl();
        return;
    }

    setFields(item.scheme, m_host->text(), item.port, m_user->text(),
              m_password->text(), item.path);
    updatePreviewUrl();
}

QString IpCameraDialog::composeUrl() const
{
    core::CameraDiscovery::UrlParts parts;
    if (core::CameraDiscovery::parseUrl(m_host->text(), &parts)) {
        return core::CameraDiscovery::buildUrl(
            parts.scheme, parts.host, parts.port,
            parts.user.isEmpty() ? m_user->text().trimmed() : parts.user,
            parts.password.isEmpty() ? m_password->text() : parts.password,
            parts.path);
    }
    return core::CameraDiscovery::buildUrl(
        currentScheme(), m_host->text().trimmed(), m_port->value(),
        m_user->text().trimmed(), m_password->text(), m_path->text().trimmed());
}

void IpCameraDialog::updatePreviewUrl()
{
    const QString url = composeUrl();

    // Пароль в подписи скрывается: окно настройки могут заполнять при
    // посторонних, и светить им на экране незачем.
    QString shown = url;
    if (!m_password->text().isEmpty())
        shown.replace(m_password->text(), QStringLiteral("******"));

    m_urlPreview->setText(m_host->text().trimmed().isEmpty()
                              ? QStringLiteral("Укажите адрес камеры.")
                              : QStringLiteral("Адрес потока: %1").arg(shown));
}

// ---------------------------------------------------------------- проверка

void IpCameraDialog::testConnection()
{
    stopTest(QString(), false);

    const core::SourceDescriptor descriptor = source();
    if (descriptor.isNone()) {
        m_testResult->setText(QStringLiteral("Сначала укажите адрес камеры."));
        m_testResult->setStyleSheet(QStringLiteral("color: #ffd23f;"));
        return;
    }

    m_test->setEnabled(false);
    m_testResult->setText(QStringLiteral("Подключение…"));
    m_testResult->setStyleSheet(QStringLiteral("color: #939fb0;"));

    m_player = new QMediaPlayer(this);
    m_sink = new QVideoSink(this);
    m_player->setVideoSink(m_sink);

    connect(m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
                if (!frame.isValid())
                    return;
                stopTest(QStringLiteral("Связь есть: кадр %1×%2 получен.")
                             .arg(frame.width()).arg(frame.height()), true);
            });

    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &description) {
                if (error == QMediaPlayer::NoError)
                    return;
                stopTest(QStringLiteral("Не удалось: %1").arg(
                             description.isEmpty() ? QStringLiteral("камера не отвечает")
                                                   : description), false);
            });

    m_testTimeout = new QTimer(this);
    m_testTimeout->setSingleShot(true);
    m_testTimeout->setInterval(kTestTimeoutMs);
    connect(m_testTimeout, &QTimer::timeout, this, [this] {
        stopTest(QStringLiteral(
                     "Кадр не пришёл за %1 секунд. Проверьте протокол, путь потока, "
                     "имя пользователя и пароль.").arg(kTestTimeoutMs / 1000), false);
    });
    m_testTimeout->start();

    m_player->setSource(QUrl(descriptor.target()));
    m_player->play();
}

void IpCameraDialog::stopTest(const QString &message, bool success)
{
    if (m_testTimeout) {
        m_testTimeout->stop();
        m_testTimeout->deleteLater();
        m_testTimeout = nullptr;
    }
    if (m_player) {
        m_player->stop();
        m_player->deleteLater();
        m_player = nullptr;
        m_sink = nullptr;
    }

    if (m_test)
        m_test->setEnabled(true);

    if (message.isEmpty() || !m_testResult)
        return;

    m_testResult->setText(message);
    m_testResult->setStyleSheet(success ? QStringLiteral("color: #35c759;")
                                        : QStringLiteral("color: #ff6b6b;"));
}

// ------------------------------------------------------------------- итог

core::SourceDescriptor IpCameraDialog::source() const
{
    const QString host = m_host->text().trimmed();
    if (host.isEmpty())
        return core::SourceDescriptor::none();

    const QString url = composeUrl();

    QString name = m_name->text().trimmed();
    if (name.isEmpty())
        name = QStringLiteral("Камера %1").arg(host);

    return core::SourceDescriptor::network(url, name);
}

} // namespace ui
