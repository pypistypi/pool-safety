#include "ui/VideoView.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QDateTime>
#include <QFontMetrics>

namespace ui {

namespace {

constexpr int kBarHeight = 26;    // высота полосы с подписями снизу
constexpr int kPadding   = 8;

QColor levelColor(core::Level level)
{
    switch (level) {
    case core::Level::Alarm:     return theme::alarm();
    case core::Level::Attention: return theme::attention();
    case core::Level::Normal:    break;
    }
    return theme::presence();
}

} // namespace

VideoView::VideoView(QWidget *parent)
    : QWidget(parent)
{
    // Фон рисуем сами, целиком и без прозрачности. Системная заливка перед
    // нашей отрисовкой — это лишний проход по всем точкам и источник мерцания.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setMinimumSize(160, 90);
    m_paceTimer.start();
}

void VideoView::setFrame(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return;

    // Панель не видна — вся работа с кадром бессмысленна. Проверка стоит
    // здесь, до преобразования: именно оно самое дорогое в этом классе.
    if (!isVisible())
        return;

    const qint64 now = m_paceTimer.elapsed();
    if ((now - m_lastShownMs) < m_minIntervalMs)
        return;
    m_lastShownMs = now;

    QImage image = frame.toImage();
    if (image.isNull())
        return;

    m_source = std::move(image);
    m_frameSize = m_source.size();
    rescale();
    update();
}

void VideoView::clear(const QString &placeholder)
{
    m_placeholder = placeholder;
    m_source = QImage();
    m_scaled = QImage();
    m_frameSize = QSize();
    m_fps = 0.0;
    m_analysis = core::PanelAnalysis();
    update();
}

void VideoView::setZoneLabel(const QString &text)
{
    if (m_zoneLabel == text) return;
    m_zoneLabel = text;
    update();
}

void VideoView::setSourceName(const QString &text)
{
    if (m_sourceName == text) return;
    m_sourceName = text;
    update();
}

void VideoView::setStatusText(const QString &text)
{
    if (m_statusText == text) return;
    m_statusText = text;
    update();
}

void VideoView::setStreamInfo(const QSize &frameSize, double fps)
{
    // Частоту сравниваем огрублённо: она всё время слегка плавает, и
    // перерисовывать панель из-за сотых долей — расточительство.
    if (m_frameSize == frameSize && qAbs(m_fps - fps) < 0.5)
        return;
    m_frameSize = frameSize;
    m_fps = fps;
    update();
}

void VideoView::setAnalysis(const core::PanelAnalysis &analysis)
{
    m_analysis = analysis;
    update();   // одна перерисовка на всё состояние кадра
}

void VideoView::clearAnalysis()
{
    m_analysis = core::PanelAnalysis();
    update();
}

void VideoView::setAlarmActive(bool active)
{
    if (m_alarmActive == active) return;
    m_alarmActive = active;
    update();
}

void VideoView::setBlinkPhase(bool bright)
{
    if (m_blinkBright == bright)
        return;
    m_blinkBright = bright;
    // Перерисовываем только когда есть чему мигать: иначе таймер окна дёргал
    // бы все четыре панели раз в полсекунды впустую.
    if (m_alarmActive || m_analysis.worstLevel != core::Level::Normal)
        update();
}

void VideoView::setSkeletonsVisible(bool visible)
{
    if (m_skeletons == visible) return;
    m_skeletons = visible;
    update();
}

void VideoView::setFpsLimit(int fps)
{
    // Отрицательный или нулевой темп не имеет смысла — оставляем прежний
    // предел, а не встаём в бесконечный показ или в деление на ноль.
    if (fps <= 0)
        return;
    m_minIntervalMs = qMax<qint64>(1, 1000 / fps);
}

void VideoView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    rescale();
}

void VideoView::rescale()
{
    if (m_source.isNull() || width() <= 0 || height() <= 0) {
        m_scaled = QImage();
        m_target = QRect();
        return;
    }

    const QSize area(width(), qMax(1, height() - kBarHeight));
    const QSize fitted = m_source.size().scaled(area, Qt::KeepAspectRatio);
    if (fitted.isEmpty()) {
        m_scaled = QImage();
        m_target = QRect();
        return;
    }

    // Гладкое масштабирование включается только при уменьшении: увеличивать с
    // ним заметно дороже, а разницы на видео с камеры глазом не видно.
    const bool downscale = fitted.width() < m_source.width();
    m_scaled = m_source.scaled(fitted, Qt::IgnoreAspectRatio,
                               downscale ? Qt::SmoothTransformation
                                         : Qt::FastTransformation);
    m_target = QRect(QPoint((area.width() - fitted.width()) / 2,
                            (area.height() - fitted.height()) / 2),
                     fitted);
}

void VideoView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), QColor(0x0d, 0x10, 0x14));

    if (!m_scaled.isNull()) {
        painter.drawImage(m_target, m_scaled);
        drawPeople(painter);
    } else {
        drawPlaceholder(painter);
    }

    drawOverlay(painter);
    drawVerdictBanner(painter);
}

void VideoView::drawPlaceholder(QPainter &painter)
{
    painter.setPen(theme::textMuted());
    QFont font = painter.font();
    font.setPointSize(11);
    painter.setFont(font);
    const QRect area(0, 0, width(), qMax(1, height() - kBarHeight));
    painter.drawText(area, Qt::AlignCenter, m_placeholder);
}

QRect VideoView::placeBox(const QRect &box, double scaleX, double scaleY) const
{
    return QRect(m_target.x() + int(box.x() * scaleX),
                 m_target.y() + int(box.y() * scaleY),
                 int(box.width() * scaleX),
                 int(box.height() * scaleY));
}

void VideoView::drawPeople(QPainter &painter)
{
    if (m_analysis.people.isEmpty() || m_analysis.frameSize.isEmpty() || m_target.isEmpty())
        return;

    // Рамки приходят в координатах исходного кадра, а показан он уменьшенным и
    // сдвинутым к центру панели. Пересчитываем каждый раз при отрисовке —
    // тогда рамки остаются на людях и при изменении размера окна.
    const double scaleX = double(m_target.width()) / m_analysis.frameSize.width();
    const double scaleY = double(m_target.height()) / m_analysis.frameSize.height();

    QFont font = painter.font();
    font.setPointSize(9);
    font.setBold(true);
    const QFontMetrics metrics(font);

    for (const core::PersonView &person : std::as_const(m_analysis.people)) {
        const QColor color = levelColor(person.level);
        const QRect placed = placeBox(person.box, scaleX, scaleY);

        // --- скелет -------------------------------------------------------
        //
        // Рисуется бледнее рамки: он поясняет, ПОЧЕМУ система решила так, а не
        // притягивает взгляд. Внимание должно доставаться рамке и подписи.
        if (m_skeletons && person.points.size() == core::kp::Count) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            QColor limbColor = color;
            limbColor.setAlpha(170);
            QPen limbPen(limbColor);
            limbPen.setWidth(2);
            painter.setPen(limbPen);

            auto place = [&](int index) {
                const QPointF &point = person.points.at(index);
                return QPointF(m_target.x() + point.x() * scaleX,
                               m_target.y() + point.y() * scaleY);
            };
            auto visible = [&](int index) {
                return person.scores.value(index, 0.f) >= 0.35f;
            };

            for (const auto &limb : core::kp::limbs) {
                if (!visible(limb[0]) || !visible(limb[1]))
                    continue;
                painter.drawLine(place(limb[0]), place(limb[1]));
            }

            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            for (int index = 0; index < core::kp::Count; ++index) {
                if (!visible(index))
                    continue;
                painter.drawEllipse(place(index), 2.5, 2.5);
            }
        }

        // --- рамка --------------------------------------------------------
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setBrush(Qt::NoBrush);
        QPen pen(color);
        // Тревожная рамка толще и мигает: её задача — выдернуть взгляд.
        pen.setWidth(person.level == core::Level::Alarm ? 4 : 2);
        if (person.level == core::Level::Alarm && !m_blinkBright)
            pen.setColor(color.darker(180));
        painter.setPen(pen);
        painter.drawRect(placed);

        // --- подпись над рамкой -------------------------------------------
        if (person.level == core::Level::Normal || person.label.isEmpty())
            continue;

        const QString text = QStringLiteral("%1: %2")
                                 .arg(core::levelText(person.level), person.label);
        painter.setFont(font);
        const int textWidth = metrics.horizontalAdvance(text) + 2 * kPadding;
        const int textHeight = metrics.height() + 4;

        int labelY = placed.top() - textHeight - 2;
        if (labelY < m_target.top())
            labelY = placed.top() + 2;   // человек у верхнего края — подпись внутри

        const QRect plate(qMax(m_target.left(), placed.left()), labelY,
                          qMin(textWidth, m_target.width()), textHeight);

        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRect(plate);
        painter.setPen(person.level == core::Level::Alarm ? QColor(Qt::white)
                                                          : QColor(0x14, 0x14, 0x14));
        painter.drawText(plate, Qt::AlignCenter,
                         metrics.elidedText(text, Qt::ElideRight, plate.width() - 4));
    }
}

void VideoView::drawVerdictBanner(QPainter &painter)
{
    if (m_analysis.worstLevel == core::Level::Normal || m_analysis.worstLabel.isEmpty())
        return;

    // Крупная подпись поверх кадра. Именно она читается издалека — боковая
    // панель с числами полезна при настройке, но в зале на неё никто не
    // смотрит.
    const bool alarm = m_analysis.worstLevel == core::Level::Alarm;
    const QColor color = levelColor(m_analysis.worstLevel);

    QFont font = painter.font();
    font.setPointSize(alarm ? 15 : 12);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics(font);

    const QString headline = QStringLiteral("%1 — %2")
                                 .arg(core::levelText(m_analysis.worstLevel),
                                      m_analysis.worstLabel);
    const QString reason = m_analysis.worstReasons.value(0);

    QFont small = font;
    small.setPointSize(alarm ? 10 : 9);
    small.setBold(false);
    const QFontMetrics smallMetrics(small);

    const int lineHeight = metrics.height();
    const int reasonHeight = reason.isEmpty() ? 0 : smallMetrics.height();
    const int bannerHeight = lineHeight + reasonHeight + 12;

    const QRect banner(0, 0, width(), bannerHeight);

    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    QColor background = color;
    // Мигание — только для тревоги: «внимание» не должно дёргать глаз.
    background.setAlpha(alarm ? (m_blinkBright ? 235 : 150) : 205);
    painter.setBrush(background);
    painter.drawRect(banner);

    painter.setPen(alarm ? QColor(Qt::white) : QColor(0x14, 0x14, 0x14));
    painter.setFont(font);
    painter.drawText(QRect(kPadding, 4, width() - 2 * kPadding, lineHeight),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     metrics.elidedText(headline, Qt::ElideRight, width() - 2 * kPadding));

    if (!reason.isEmpty()) {
        painter.setFont(small);
        painter.drawText(QRect(kPadding, 4 + lineHeight, width() - 2 * kPadding, reasonHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         smallMetrics.elidedText(reason, Qt::ElideRight,
                                                 width() - 2 * kPadding));
    }
}

void VideoView::drawOverlay(QPainter &painter)
{
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Название зоны сдвигается вниз, если сверху лежит подпись вердикта.
    int topOffset = kPadding;
    if (m_analysis.worstLevel != core::Level::Normal && !m_analysis.worstLabel.isEmpty())
        topOffset += 44;

    // --- название зоны, левый верхний угол ------------------------------
    if (!m_zoneLabel.isEmpty()) {
        QFont font = painter.font();
        font.setPointSize(10);
        font.setBold(true);
        painter.setFont(font);

        const QFontMetrics metrics(font);
        const int textWidth = metrics.horizontalAdvance(m_zoneLabel);
        const QRect plate(kPadding, topOffset,
                          textWidth + 2 * kPadding, metrics.height() + 6);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(13, 16, 20, 190));
        painter.drawRoundedRect(plate, 4, 4);
        painter.setPen(theme::textPrimary());
        painter.drawText(plate, Qt::AlignCenter, m_zoneLabel);

        // --- сигнал 1: плашка присутствия, сразу под названием ----------
        if (m_analysis.peopleCount > 0) {
            const QString text = QStringLiteral("В зоне: %1").arg(m_analysis.peopleCount);
            const int badgeWidth = metrics.horizontalAdvance(text);
            const QRect badge(kPadding, plate.bottom() + 5,
                              badgeWidth + 2 * kPadding, metrics.height() + 6);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme::presence());
            painter.drawRoundedRect(badge, 4, 4);
            painter.setPen(QColor(0x0d, 0x14, 0x0d));
            painter.drawText(badge, Qt::AlignCenter, text);
        }
    }

    // --- нижняя полоса: время, разрешение, частота, состояние -----------
    const QRect bar(0, height() - kBarHeight, width(), kBarHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x16, 0x1a, 0x20));
    painter.drawRect(bar);

    QFont font = painter.font();
    font.setPointSize(9);
    font.setBold(false);
    painter.setFont(font);
    const QFontMetrics metrics(font);

    // Время — справа, и место под него резервируется до отрисовки остального.
    // В прежней версии подпись слева наезжала на время и уродовала его.
    const QString clock = QDateTime::currentDateTime().toString(
        QStringLiteral("dd.MM.yyyy  HH:mm:ss"));
    const int clockWidth = metrics.horizontalAdvance(clock) + kPadding;

    painter.setPen(theme::textPrimary());
    painter.drawText(bar.adjusted(0, 0, -kPadding, 0),
                     Qt::AlignRight | Qt::AlignVCenter, clock);

    QStringList parts;
    if (!m_sourceName.isEmpty())
        parts << m_sourceName;
    if (m_frameSize.isValid() && !m_frameSize.isEmpty()) {
        parts << QStringLiteral("%1 x %2").arg(m_frameSize.width()).arg(m_frameSize.height());
        if (m_fps > 0.1)
            parts << QStringLiteral("%1 к/с").arg(m_fps, 0, 'f', 0);
    }
    if (!m_statusText.isEmpty())
        parts << m_statusText;

    const int available = bar.width() - clockWidth - 2 * kPadding;
    if (available > 40) {
        const QString left = metrics.elidedText(parts.join(QStringLiteral("  ·  ")),
                                                Qt::ElideRight, available);
        painter.setPen(theme::textMuted());
        painter.drawText(bar.adjusted(kPadding, 0, 0, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, left);
    }

    // --- рамка вокруг всей панели ---------------------------------------
    //
    // Красная — тревога, оранжевая — есть признаки, зелёная — в зоне есть
    // люди. Порядок строгий: тревога перекрывает всё. Если человек тонет,
    // оператору не важно, сколько людей на панели, и два цвета сразу его
    // только собьют.
    const bool alarm = m_alarmActive || m_analysis.worstLevel == core::Level::Alarm;
    const bool attention = m_analysis.worstLevel == core::Level::Attention;

    if (alarm || attention || m_analysis.peopleCount > 0) {
        QColor color = alarm ? theme::alarm()
                             : (attention ? theme::attention() : theme::presence());
        if (alarm && !m_blinkBright)
            color = color.darker(200);

        QPen pen(color);
        pen.setWidth(alarm ? 5 : 3);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const int inset = alarm ? 3 : 2;
        painter.drawRect(rect().adjusted(inset, inset, -inset, -inset));
    }
}

} // namespace ui
