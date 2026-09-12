#pragma once

// ---------------------------------------------------------------------------
//  Одна клетка сетки наблюдения.
//
//  Панель отвечает ровно за две вещи: показать изображение выбранного
//  источника и дать сменить этот источник. Она не открывает устройства сама —
//  этим занимается реестр (SourceHub), — и не знает, что именно ей показывают.
//  Благодаря такому разделению одну и ту же камеру можно выбрать сразу в
//  нескольких клетках, а само устройство при этом откроется один раз.
//
//  Выбор источника — выпадающее меню в правом верхнем углу самой картинки, как
//  на пультах наблюдения: рядом с тем, что настраиваешь, а не в отдельном
//  разделе настроек.
// ---------------------------------------------------------------------------

#include "core/AnalysisTypes.h"
#include "core/SourceDescriptor.h"

#include <QFrame>
#include <QVector>
#include <QRect>
#include <QSize>

class QPushButton;

namespace core {
class SourceHub;
class DeviceRegistry;
class VideoSource;
class AnalysisWorker;
}

namespace ui {

class VideoView;

class CameraPanel : public QFrame
{
    Q_OBJECT

public:
    CameraPanel(int panelId,
                const QString &zoneLabel,
                core::SourceHub *hub,
                core::DeviceRegistry *devices,
                QWidget *parent = nullptr);
    ~CameraPanel() override;

    int panelId() const { return m_panelId; }
    const QString &zoneLabel() const { return m_zoneLabel; }
    const core::SourceDescriptor &source() const { return m_source; }

    /// Адрес потока, если источник — IP-камера. Телефон берёт видео прямо с
    /// камеры, поэтому адрес нужен ему целиком, вместе с учётными данными.
    QString streamUrl() const;

    /// Подключить панель к источнику. Прежний освобождается.
    void setSource(const core::SourceDescriptor &descriptor);

    /// Куда отдавать кадры на разбор. Панель ничего не знает о том, что с
    /// ними будет: её дело — показать картинку и передать её дальше.
    void setAnalysisWorker(core::AnalysisWorker *worker);

    /// Обновить показания, меняющиеся во времени: часы, частоту кадров.
    /// Вызывается общим таймером окна — свой таймер на каждой панели означал
    /// бы четыре пробуждения вместо одного при той же пользе.
    void refreshReadouts();

    /// Результат одного разбора кадра: рамки, счётчик и уровень.
    void setAnalysis(const core::PanelAnalysis &analysis);

    /// Забыть разбор: источник сменили.
    void clearAnalysis();

    /// Сигнал 2: панель участвует в текущей тревоге.
    void setAlarmActive(bool active);

    /// Фаза мигания — приходит от общего таймера окна.
    void setBlinkPhase(bool bright);

    /// Предел частоты показа — общий для всех панелей, задаётся в настройках.
    void setFpsLimit(int fps);

signals:
    void sourceChanged(int panelId, const core::SourceDescriptor &descriptor);
    /// Оператор объявил тревогу с этой панели.
    void alarmRequested(int panelId);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void showSourceMenu();

private:
    void detachSource();
    void chooseFile();
    void chooseIpCamera();
    void updateSourceButton();
    void placeSourceButton();

    int m_panelId;
    QString m_zoneLabel;

    core::SourceHub *m_hub = nullptr;
    core::DeviceRegistry *m_devices = nullptr;
    core::VideoSource *m_active = nullptr;
    core::AnalysisWorker *m_analysis = nullptr;
    core::SourceDescriptor m_source;

    VideoView *m_view = nullptr;
    QPushButton *m_sourceButton = nullptr;
    QPushButton *m_alarmButton = nullptr;
};

} // namespace ui
