#include "core/DeviceRegistry.h"

#include <QMediaDevices>

namespace core {

DeviceRegistry::DeviceRegistry(QObject *parent)
    : QObject(parent)
    , m_devices(new QMediaDevices(this))
{
    // Qt сам следит за подключением и отключением устройств. Опрос по
    // таймеру не нужен — именно он в прежней версии грузил процессор.
    connect(m_devices, &QMediaDevices::videoInputsChanged,
            this, &DeviceRegistry::devicesChanged);
}

QList<QCameraDevice> DeviceRegistry::cameras() const
{
    return QMediaDevices::videoInputs();
}

QCameraDevice DeviceRegistry::cameraById(const QString &id) const
{
    const QByteArray needle = id.toUtf8();
    const auto list = cameras();
    for (const QCameraDevice &device : list) {
        if (device.id() == needle)
            return device;
    }
    return {};
}

} // namespace core
