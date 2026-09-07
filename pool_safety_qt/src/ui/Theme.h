#pragma once

// ---------------------------------------------------------------------------
//  Оформление окна.
//
//  Тёмное — не ради моды: пост наблюдения работает круглосуточно, и светлый
//  фон на четырёх видеопанелях ночью слепит. Тёмный фон вокруг видео ещё и не
//  спорит с самим изображением, а цветом здесь выделяется только то, что
//  требует внимания: присутствие людей и тревога.
// ---------------------------------------------------------------------------

#include <QColor>
#include <QString>

namespace ui {

namespace theme {

inline const QColor background()   { return QColor(0x15, 0x18, 0x1d); }
inline const QColor surface()      { return QColor(0x1e, 0x23, 0x2b); }
inline const QColor border()       { return QColor(0x2f, 0x37, 0x42); }
inline const QColor textPrimary()  { return QColor(0xe6, 0xea, 0xf0); }
inline const QColor textMuted()    { return QColor(0x93, 0x9f, 0xb0); }

/// Спокойное состояние.
inline const QColor calm()         { return QColor(0x4a, 0x9e, 0xff); }
/// Сигнал 1 — в зоне есть люди. Зелёный: это не опасность, а сопровождение.
inline const QColor presence()     { return QColor(0x35, 0xc7, 0x59); }
/// Признаки опасности, до тревоги не дотягивающие. Оранжевый — промежуточное
/// состояние: «взгляните», но ещё не «бегите».
inline const QColor attention()    { return QColor(0xff, 0x9f, 0x1c); }
/// Сигнал 2 — тревога. Красный используется ТОЛЬКО здесь и больше нигде,
/// чтобы он ни с чем не смешивался.
inline const QColor alarm()        { return QColor(0xff, 0x45, 0x45); }
inline const QColor alarmMuted()   { return QColor(0xb3, 0x2d, 0x2d); }

QString applicationStyleSheet();

} // namespace theme

} // namespace ui
