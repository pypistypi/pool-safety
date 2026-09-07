#pragma once

// ---------------------------------------------------------------------------
//  Описание источника изображения.
//
//  Источник — это то, откуда панель берёт картинку: камера компьютера, файл с
//  диска или пустая панель. Описание намеренно сделано лёгким значением, а не
//  объектом с состоянием: его можно свободно копировать, сравнивать и хранить
//  в настройках, не заботясь о владении и не рискуя случайно продлить жизнь
//  открытому устройству.
//
//  Ключ (key) — то, по чему источники считаются одним и тем же. Именно он
//  позволяет выбрать одну камеру сразу в нескольких клетках сетки: устройство
//  при этом открывается ровно один раз (см. SourceHub).
// ---------------------------------------------------------------------------

#include <QString>

namespace core {

enum class SourceKind {
    None,     ///< панель пуста — источник не выбран
    Camera,   ///< видеоустройство компьютера (встроенное или USB)
    File,     ///< файл с диска: видеозапись или фотография
    Network   ///< IP-камера в локальной сети (RTSP)
};

class SourceDescriptor
{
public:
    SourceDescriptor() = default;

    static SourceDescriptor none();
    static SourceDescriptor camera(const QString &deviceId, const QString &deviceName);
    static SourceDescriptor file(const QString &path);

    /// IP-камера. `url` — полный адрес потока (rtsp://…), `name` — как её
    /// назвать оператору.
    ///
    /// Логин и пароль входят в адрес — так требует RTSP, и так их понимает
    /// проигрыватель. Наружу, в подписи панели, адрес показывается без них:
    /// пароль на экране поста наблюдения ни к чему.
    static SourceDescriptor network(const QString &url, const QString &name);

    /// Адрес без логина и пароля — для показа оператору и для журнала.
    QString safeTarget() const;

    SourceKind kind() const { return m_kind; }
    bool isNone() const { return m_kind == SourceKind::None; }

    /// Идентификатор устройства Qt или путь к файлу.
    const QString &target() const { return m_target; }

    /// Человекочитаемое имя для выпадающего меню и подписи на панели.
    const QString &displayName() const { return m_displayName; }

    /// Уникальный ключ источника. Одинаковый ключ — один разделяемый поток.
    QString key() const;

    /// Является ли файл неподвижным изображением (по расширению).
    bool isStillImage() const;

    bool operator==(const SourceDescriptor &other) const;
    bool operator!=(const SourceDescriptor &other) const { return !(*this == other); }

private:
    SourceKind m_kind = SourceKind::None;
    QString m_target;
    QString m_displayName;
};

} // namespace core
