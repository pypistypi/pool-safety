#include "core/BackgroundModel.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>

namespace core {

namespace {

/// Ниже какой изменчивости ячейка считается неподвижной.
///
/// Единицы — доли яркости от 0 до 255. Два деления это меньше, чем шум
/// матрицы на плохом свете, и заметно меньше, чем след прошедшего человека.
constexpr float kQuietLevel = 2.0f;

/// Насколько быстро модель забывает прошлое.
///
/// ЗДЕСЬ ГЛАВНЫЙ КОМПРОМИСС ВСЕЙ ЗАТЕИ. Забывать быстро — значит принять за
/// «постоянное окружение» человека, задремавшего на лежаке. Забывать медленно
/// — значит месяцами держаться за расстановку мебели, которую поменяли
/// вчера. Взято около часа непрерывной работы на половину забывания: лежак
/// переставляют реже, а спят на нём дольше.
constexpr float kSlowRate = 1.0f / 30000.0f;

/// Быстрая оценка — по последним минутам. Нужна не для модели, а для
/// сравнения с ней: разошлись — значит сцена уже не та.
constexpr float kFastRate = 1.0f / 300.0f;

} // namespace

QVector<float> BackgroundModel::reduce(const QImage &frame)
{
    if (frame.isNull())
        return {};

    // Уменьшаем грубо и быстро: сглаживание здесь только повредило бы —
    // нам нужна яркость области, а не красивая картинка.
    const QImage small = frame.scaled(kGridWidth, kGridHeight,
                                      Qt::IgnoreAspectRatio,
                                      Qt::FastTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    if (small.isNull())
        return {};

    QVector<float> cells(kCells, 0.0f);
    for (int y = 0; y < kGridHeight; ++y) {
        const uchar *line = small.constScanLine(y);
        for (int x = 0; x < kGridWidth; ++x)
            cells[y * kGridWidth + x] = float(line[x]);
    }
    return cells;
}

void BackgroundModel::observe(const QImage &frame)
{
    const QVector<float> now = reduce(frame);
    if (now.size() != kCells)
        return;

    if (m_previous.size() != kCells) {
        m_previous = now;
        if (m_startedAt.isEmpty())
            m_startedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
        return;
    }

    if (m_fast.size() != kCells)
        m_fast = QVector<float>(kCells, 0.0f);

    int quietInModel = 0;
    int agreeing = 0;

    for (int i = 0; i < kCells; ++i) {
        const float delta = qAbs(now.at(i) - m_previous.at(i));

        // Долгая память — сама модель.
        m_motion[i] += (delta - m_motion[i]) * kSlowRate;
        // Короткая — то, что происходит сейчас.
        m_fast[i] += (delta - m_fast[i]) * kFastRate;

        // Совпадение считаем по спокойным ячейкам: именно они — то ценное,
        // что модель знает. Подвижные и так подвижны везде.
        if (m_motion.at(i) < kQuietLevel) {
            ++quietInModel;
            if (m_fast.at(i) < kQuietLevel * 2.0f)
                ++agreeing;
        }
    }

    m_previous = now;
    ++m_samples;

    // Пока спокойных ячеек нет вовсе, сравнивать не с чем — считаем, что
    // расхождения нет, иначе новая модель сама себя объявила бы негодной.
    m_agreement = quietInModel > 0 ? double(agreeing) / quietInModel : 1.0;
}

double BackgroundModel::stillnessAt(const QPoint &point, const QSize &frameSize) const
{
    if (!isReady() || frameSize.isEmpty())
        return 0.0;

    const int x = qBound(0, point.x() * kGridWidth / frameSize.width(), kGridWidth - 1);
    const int y = qBound(0, point.y() * kGridHeight / frameSize.height(), kGridHeight - 1);
    const float motion = m_motion.at(y * kGridWidth + x);

    // Переводим изменчивость в «неподвижность» от 0 до 1.
    if (motion >= kQuietLevel)
        return 0.0;
    return double(1.0f - motion / kQuietLevel);
}

double BackgroundModel::stillnessAt(const QRect &box, const QSize &frameSize) const
{
    if (!isReady() || frameSize.isEmpty() || box.isEmpty())
        return 0.0;

    const int x0 = qBound(0, box.left() * kGridWidth / frameSize.width(), kGridWidth - 1);
    const int x1 = qBound(0, box.right() * kGridWidth / frameSize.width(), kGridWidth - 1);
    const int y0 = qBound(0, box.top() * kGridHeight / frameSize.height(), kGridHeight - 1);
    const int y1 = qBound(0, box.bottom() * kGridHeight / frameSize.height(), kGridHeight - 1);

    double sum = 0.0;
    int count = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float motion = m_motion.at(y * kGridWidth + x);
            sum += motion >= kQuietLevel ? 0.0 : double(1.0f - motion / kQuietLevel);
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

void BackgroundModel::reset()
{
    m_motion.fill(0.0f);
    m_fast.clear();
    m_previous.clear();
    m_samples = 0;
    m_agreement = 1.0;
    m_startedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
}

bool BackgroundModel::save(const QString &path) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonArray cells;
    for (float value : m_motion)
        cells.append(double(qRound(value * 100.0f)) / 100.0);

    QJsonObject root;
    root.insert(QStringLiteral("_комментарий"), QStringLiteral(
        "Карта постоянного окружения этой установки. Числа — насколько "
        "меняется каждая клетка кадра. Файл принадлежит конкретной "
        "расстановке камер: после переноса камеры обучение надо начать "
        "заново."));
    root.insert(QStringLiteral("сетка_ширина"), kGridWidth);
    root.insert(QStringLiteral("сетка_высота"), kGridHeight);
    root.insert(QStringLiteral("разборов_учтено"), m_samples);
    root.insert(QStringLiteral("обучение_начато"), m_startedAt);
    root.insert(QStringLiteral("изменчивость"), cells);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool BackgroundModel::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();

    // Чужая сетка — чужая карта. Такой файл читать нельзя: числа лягут не на
    // те места, и модель будет уверенно говорить неправду.
    if (root.value(QStringLiteral("сетка_ширина")).toInt() != kGridWidth
        || root.value(QStringLiteral("сетка_высота")).toInt() != kGridHeight) {
        return false;
    }

    const QJsonArray cells = root.value(QStringLiteral("изменчивость")).toArray();
    if (cells.size() != kCells)
        return false;

    for (int i = 0; i < kCells; ++i)
        m_motion[i] = float(cells.at(i).toDouble());

    m_samples = root.value(QStringLiteral("разборов_учтено")).toInt();
    m_startedAt = root.value(QStringLiteral("обучение_начато")).toString();
    m_fast.clear();
    m_previous.clear();
    m_agreement = 1.0;
    return true;
}

QString BackgroundModel::status() const
{
    if (m_samples < kMinSamples) {
        const int percent = kMinSamples > 0 ? m_samples * 100 / kMinSamples : 0;
        return QStringLiteral("идёт обучение: %1 %").arg(qMin(percent, 99));
    }
    if (m_agreement < kMinAgreement) {
        return QStringLiteral(
            "обучение не подходит этой сцене (совпадение %1 %) — "
            "начните заново").arg(int(m_agreement * 100));
    }
    return QStringLiteral("обучено, совпадение %1 %").arg(int(m_agreement * 100));
}

} // namespace core
