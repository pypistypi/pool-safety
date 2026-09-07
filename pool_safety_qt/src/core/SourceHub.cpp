#include "core/SourceHub.h"
#include "core/VideoSource.h"

namespace core {

SourceHub::SourceHub(QObject *parent)
    : QObject(parent)
{
}

SourceHub::~SourceHub()
{
    // Явная остановка перед разрушением: деструктор QObject-родителя удалил бы
    // источники и сам, но не гарантирует, что камера успеет корректно
    // закрыться до выхода из программы. Занятое устройство пережило бы наш
    // процесс, и следующий запуск не нашёл бы камеру.
    for (Entry &entry : m_entries) {
        if (entry.source) {
            entry.source->stop();
            delete entry.source;
        }
    }
    m_entries.clear();
}

VideoSource *SourceHub::acquire(const SourceDescriptor &descriptor)
{
    if (descriptor.isNone())
        return nullptr;

    const QString key = descriptor.key();

    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        ++it->refCount;
        return it->source;
    }

    VideoSource *source = createSource(descriptor, this);
    if (!source)
        return nullptr;

    Entry entry;
    entry.source = source;
    entry.refCount = 1;
    m_entries.insert(key, entry);

    source->start();
    emit activeSourcesChanged();
    return source;
}

void SourceHub::release(const SourceDescriptor &descriptor)
{
    if (descriptor.isNone())
        return;

    auto it = m_entries.find(descriptor.key());
    if (it == m_entries.end())
        return;

    if (--it->refCount > 0)
        return;   // источником пользуется другая панель — не трогаем

    VideoSource *source = it->source;
    m_entries.erase(it);

    if (source) {
        source->stop();
        // deleteLater, а не delete: отпустить источник могли изнутри его же
        // сигнала, и немедленное разрушение оборвало бы стек под собственными
        // ногами. Отложенное удаление снимает этот класс сбоев целиком.
        source->deleteLater();
    }

    emit activeSourcesChanged();
}

} // namespace core
