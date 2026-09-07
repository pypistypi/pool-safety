#include "core/SourceDescriptor.h"

#include <QFileInfo>
#include <QUrl>

namespace core {

namespace {

// Расширения, которые считаем фотографией. Всё остальное отдаётся
// проигрывателю: он сам разберётся с форматом, а если не сможет — честно
// сообщит об ошибке, что лучше нашей самодеятельности со списком кодеков.
bool looksLikeImage(const QString &path)
{
    static const QStringList kImageSuffixes = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("bmp"),  QStringLiteral("webp"), QStringLiteral("tif"),
        QStringLiteral("tiff")
    };
    return kImageSuffixes.contains(QFileInfo(path).suffix().toLower());
}

} // namespace

SourceDescriptor SourceDescriptor::none()
{
    SourceDescriptor d;
    d.m_kind = SourceKind::None;
    d.m_displayName = QStringLiteral("Источник не выбран");
    return d;
}

SourceDescriptor SourceDescriptor::camera(const QString &deviceId, const QString &deviceName)
{
    SourceDescriptor d;
    d.m_kind = SourceKind::Camera;
    d.m_target = deviceId;
    d.m_displayName = deviceName;
    return d;
}

SourceDescriptor SourceDescriptor::network(const QString &url, const QString &name)
{
    SourceDescriptor descriptor;
    descriptor.m_kind = SourceKind::Network;
    descriptor.m_target = url;
    descriptor.m_displayName = name.isEmpty() ? url : name;
    return descriptor;
}

QString SourceDescriptor::safeTarget() const
{
    if (m_kind != SourceKind::Network)
        return m_target;

    QUrl url(m_target);
    if (url.userName().isEmpty())
        return m_target;

    // Пароль не должен попадать ни на экран, ни в журнал.
    url.setUserName(QString());
    url.setPassword(QString());
    return url.toString();
}

SourceDescriptor SourceDescriptor::file(const QString &path)
{
    SourceDescriptor d;
    d.m_kind = SourceKind::File;
    d.m_target = path;
    d.m_displayName = QFileInfo(path).fileName();
    return d;
}

QString SourceDescriptor::key() const
{
    switch (m_kind) {
    case SourceKind::None:   return QStringLiteral("none");
    case SourceKind::Camera: return QStringLiteral("cam:") + m_target;
    case SourceKind::File:   return QStringLiteral("file:") + m_target;
    case SourceKind::Network: return QStringLiteral("net:") + m_target;
    }
    return QStringLiteral("none");
}

bool SourceDescriptor::isStillImage() const
{
    return m_kind == SourceKind::File && looksLikeImage(m_target);
}

bool SourceDescriptor::operator==(const SourceDescriptor &other) const
{
    return m_kind == other.m_kind && m_target == other.m_target;
}

} // namespace core
