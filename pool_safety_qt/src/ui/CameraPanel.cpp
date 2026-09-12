#include "ui/CameraPanel.h"
#include "ui/VideoView.h"
#include "ui/IpCameraDialog.h"
#include "ui/Theme.h"

#include "core/SourceHub.h"
#include "core/VideoSource.h"
#include "core/DeviceRegistry.h"
#include "core/AnalysisWorker.h"

#include <QVBoxLayout>
#include <QPushButton>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QCameraDevice>

namespace ui {

CameraPanel::CameraPanel(int panelId,
                         const QString &zoneLabel,
                         core::SourceHub *hub,
                         core::DeviceRegistry *devices,
                         QWidget *parent)
    : QFrame(parent)
    , m_panelId(panelId)
    , m_zoneLabel(zoneLabel)
    , m_hub(hub)
    , m_devices(devices)
    , m_source(core::SourceDescriptor::none())
{
    setObjectName(QStringLiteral("Card"));
    setFrameShape(QFrame::NoFrame);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    m_view = new VideoView(this);
    m_view->setZoneLabel(zoneLabel);
    layout->addWidget(m_view);

    // Кнопка живёт поверх картинки и потому создаётся дочерней к ней, а не
    // кладётся в раскладку: раскладка отняла бы у видео полосу по высоте.
    m_sourceButton = new QPushButton(m_view);
    m_sourceButton->setObjectName(QStringLiteral("SourceButton"));
    m_sourceButton->setCursor(Qt::PointingHandCursor);
    connect(m_sourceButton, &QPushButton::clicked, this, &CameraPanel::showSourceMenu);

    // Тревогу объявляют с той панели, где оператор увидел беду: так в событие
    // сразу попадает нужная зона, и красной рамкой обводится именно она.
    m_alarmButton = new QPushButton(QStringLiteral("Тревога"), m_view);
    m_alarmButton->setObjectName(QStringLiteral("PanelAlarmButton"));
    m_alarmButton->setCursor(Qt::PointingHandCursor);
    m_alarmButton->setToolTip(
        QStringLiteral("Объявить тревогу по зоне «%1»").arg(zoneLabel));
    connect(m_alarmButton, &QPushButton::clicked, this,
            [this] { emit alarmRequested(m_panelId); });

    updateSourceButton();
    m_view->clear(QStringLiteral("Источник не выбран\nвыберите в меню справа вверху"));
}

CameraPanel::~CameraPanel()
{
    detachSource();
}

// --------------------------------------------------------------- источник

void CameraPanel::detachSource()
{
    if (m_active) {
        // Отключаемся от сигналов ДО освобождения: источник может быть занят
        // другой панелью и продолжит слать кадры — уже разрушаемому виджету.
        //
        // ОТСОЕДИНЯТЬ НАДО ОБА ПОЛУЧАТЕЛЯ. Здесь была ошибка: рвалась только
        // связь с самой панелью, а кадры и состояние идут ещё и напрямую в
        // вид. После «Отключить источник» пункт из меню исчезал, панель
        // считала себя пустой — а картинка продолжала идти, потому что вид
        // остался подписан. Для оператора это выглядит как живая камера там,
        // где наблюдения уже нет.
        disconnect(m_active, nullptr, this, nullptr);
        if (m_view)
            disconnect(m_active, nullptr, m_view, nullptr);
        m_active = nullptr;
    }
    if (!m_source.isNone() && m_hub)
        m_hub->release(m_source);
}

void CameraPanel::setSource(const core::SourceDescriptor &descriptor)
{
    if (descriptor == m_source && m_active)
        return;

    detachSource();
    m_source = descriptor;
    m_view->clearAnalysis();

    if (descriptor.isNone()) {
        m_view->clear(QStringLiteral("Источник не выбран\nвыберите в меню справа вверху"));
        m_view->setSourceName(QString());
        m_view->setStatusText(QString());
        updateSourceButton();
        emit sourceChanged(m_panelId, m_source);
        return;
    }

    m_active = m_hub ? m_hub->acquire(descriptor) : nullptr;
    if (!m_active) {
        m_view->clear(QStringLiteral("Источник недоступен"));
        updateSourceButton();
        emit sourceChanged(m_panelId, m_source);
        return;
    }

    connect(m_active, &core::VideoSource::frameReady,
            m_view, &VideoView::setFrame);
    connect(m_active, &core::VideoSource::statusChanged,
            m_view, &VideoView::setStatusText);

    // Тот же кадр уходит на разбор в вычислительный поток. Пока разбор
    // выключен, вызов стоит одну проверку признака и сразу возвращается —
    // каркас за него не платит.
    connect(m_active, &core::VideoSource::frameReady, this,
            [this](const QVideoFrame &frame) {
                if (m_analysis)
                    m_analysis->submitFrame(m_panelId, frame);
            });

    m_view->setSourceName(descriptor.displayName());
    m_view->setStatusText(m_active->statusText());
    m_view->clear(QStringLiteral("Ожидание изображения…"));

    // Если источник уже работает (его выбрала другая панель), картинка есть
    // прямо сейчас — показываем её, не дожидаясь следующего кадра. Для
    // фотографии следующего кадра не будет вовсе.
    const QVideoFrame existing = m_active->lastFrame();
    if (existing.isValid())
        m_view->setFrame(existing);

    updateSourceButton();
    emit sourceChanged(m_panelId, m_source);
}

QString CameraPanel::streamUrl() const
{
    return m_source.kind() == core::SourceKind::Network ? m_source.target() : QString();
}

void CameraPanel::setAnalysisWorker(core::AnalysisWorker *worker)
{
    m_analysis = worker;
}

// ------------------------------------------------------------------- меню

void CameraPanel::showSourceMenu()
{
    QMenu menu(this);

    QAction *fileAction = menu.addAction(QStringLiteral("Открыть файл с компьютера…"));
    QAction *ipAction = menu.addAction(QStringLiteral("Подключить IP-камеру…"));

    menu.addSeparator();

    const auto cameras = m_devices ? m_devices->cameras() : QList<QCameraDevice>();
    if (cameras.isEmpty()) {
        QAction *empty = menu.addAction(QStringLiteral("Камеры не найдены"));
        empty->setEnabled(false);
    } else {
        for (const QCameraDevice &device : cameras) {
            const QString id = QString::fromUtf8(device.id());
            QString title = device.description();
            if (device.isDefault())
                title += QStringLiteral("  (по умолчанию)");

            QAction *action = menu.addAction(title);
            action->setCheckable(true);
            action->setChecked(m_source.kind() == core::SourceKind::Camera
                               && m_source.target() == id);
            action->setData(id);
        }
    }

    QAction *noneAction = nullptr;
    if (!m_source.isNone()) {
        menu.addSeparator();
        noneAction = menu.addAction(QStringLiteral("Отключить источник"));
    }

    QAction *chosen = menu.exec(m_sourceButton->mapToGlobal(
        QPoint(0, m_sourceButton->height() + 2)));
    if (!chosen)
        return;

    if (chosen == fileAction) {
        chooseFile();
        return;
    }
    if (chosen == ipAction) {
        chooseIpCamera();
        return;
    }
    if (noneAction && chosen == noneAction) {
        setSource(core::SourceDescriptor::none());
        return;
    }

    const QString id = chosen->data().toString();
    if (id.isEmpty())
        return;

    QString name = chosen->text();
    name.remove(QStringLiteral("  (по умолчанию)"));
    setSource(core::SourceDescriptor::camera(id, name));
}

void CameraPanel::chooseIpCamera()
{
    IpCameraDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const core::SourceDescriptor descriptor = dialog.source();
    if (!descriptor.isNone())
        setSource(descriptor);
}

void CameraPanel::chooseFile()
{
    static const QString filter = QStringLiteral(
        "Видео и изображения (*.mp4 *.avi *.mov *.mkv *.m4v *.wmv *.mpg *.mpeg "
        "*.webm *.jpg *.jpeg *.png *.bmp *.webp);;"
        "Видеозаписи (*.mp4 *.avi *.mov *.mkv *.m4v *.wmv *.mpg *.mpeg *.webm);;"
        "Изображения (*.jpg *.jpeg *.png *.bmp *.webp);;"
        "Все файлы (*)");

    const QString start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Выберите видео или фотографию для панели «%1»").arg(m_zoneLabel),
        start, filter);

    if (path.isEmpty())
        return;   // оператор передумал — прежний источник не трогаем

    setSource(core::SourceDescriptor::file(path));
}

void CameraPanel::updateSourceButton()
{
    const QString name = m_source.isNone()
                             ? QStringLiteral("Источник")
                             : m_source.displayName();

    // Длинное имя файла целиком не влезет и растянет кнопку на пол-панели.
    const QString shortened = name.size() > 22 ? name.left(20) + QStringLiteral("…") : name;
    m_sourceButton->setText(shortened + QStringLiteral("  ▾"));
    m_sourceButton->setToolTip(m_source.isNone()
                                   ? QStringLiteral("Выбрать источник изображения")
                                   : m_source.target());
    m_sourceButton->adjustSize();
    placeSourceButton();
}

void CameraPanel::placeSourceButton()
{
    if (!m_sourceButton || !m_view || !m_alarmButton)
        return;

    const int margin = 8;
    const int gap = 6;

    m_alarmButton->adjustSize();
    const int alarmX = qMax(margin, m_view->width() - m_alarmButton->width() - margin);
    m_alarmButton->move(alarmX, margin);
    m_alarmButton->raise();

    const int sourceX = qMax(margin, alarmX - gap - m_sourceButton->width());
    m_sourceButton->move(sourceX, margin);
    m_sourceButton->raise();
}

void CameraPanel::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    placeSourceButton();
}

// --------------------------------------------------------------- показания

void CameraPanel::refreshReadouts()
{
    if (m_active)
        m_view->setStreamInfo(m_active->frameSize(), m_active->fps());

    // Часы в нижней полосе идут секундами, поэтому панель нужно перерисовать
    // даже когда нового кадра не пришло (фотография, замерший поток).
    m_view->update();
}

void CameraPanel::setAnalysis(const core::PanelAnalysis &analysis)
{
    // ПАНЕЛЬ БЕЗ ИСТОЧНИКА НЕ ПОКАЗЫВАЕТ НИЧЬИХ ЛЮДЕЙ.
    //
    // Разбор идёт в отдельном потоке и отстаёт на кадр-другой. Когда оператор
    // отключает источник, в пути может оказаться уже посчитанный итог — и
    // тогда на пустой панели остаётся висеть «В зоне: 7». Для поста наблюдения
    // это ложь того же рода, что и картинка с отключённой камеры.
    if (m_source.isNone()) {
        m_view->clearAnalysis();
        return;
    }
    m_view->setAnalysis(analysis);
}

void CameraPanel::clearAnalysis()
{
    m_view->clearAnalysis();
}

void CameraPanel::setAlarmActive(bool active)
{
    m_view->setAlarmActive(active);
}

void CameraPanel::setBlinkPhase(bool bright)
{
    m_view->setBlinkPhase(bright);
}

void CameraPanel::setFpsLimit(int fps)
{
    m_view->setFpsLimit(fps);
}

} // namespace ui
