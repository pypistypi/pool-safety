#pragma once

// ---------------------------------------------------------------------------
//  Вывод изображения одной панели вместе со всеми подписями поверх него.
//
//  ПОЧЕМУ НЕ ШТАТНЫЙ QVideoWidget. Он выводит картинку в отдельное системное
//  окно внутри нашего. Поверх такого окна виджеты Windows не рисует, а нам
//  нужны поверх видео название зоны, время, счётчик людей, скелеты и рамка
//  тревоги. Обходные приёмы здесь дают ровно тот класс мерцаний и артефактов,
//  от которого мы уходим.
//
//  ЧЕМ ПЛАТИМ И ЧТО ВЗАМЕН. Кадр переводится в картинку силами процессора, а
//  не видеокарты. Взамен получаем полный контроль над нагрузкой, а он на посту
//  наблюдения важнее пиковой скорости:
//
//    * ЧАСТОТА ПОКАЗА ОГРАНИЧЕНА. Камера отдаёт 30 кадров в секунду, глазу
//      оператора хватает 25 и меньше. Лишние кадры отбрасываются до всякой
//      обработки — самая дешёвая работа та, которая не делается.
//    * СВЁРНУТОЕ ОКНО НЕ СТОИТ НИЧЕГО. Пока панель не видна, кадры даже не
//      разворачиваются в картинку.
//    * МАСШТАБ СЧИТАЕТСЯ ОДИН РАЗ НА КАДР. Готовая картинка нужного размера
//      запоминается, поэтому перерисовка подписей (раз в секунду) не тянет за
//      собой пересчёт всего изображения.
//
//  ВЕРДИКТ ПИШЕТСЯ НА САМОМ КАДРЕ, КРУПНО. Это выяснилось при проверке: все
//  измерения жили в боковой панели, и проверяющий, лежавший на полу в роли
//  пострадавшего, прочитать их не мог. Подпись поверх видео читается с
//  нескольких метров — а именно так на неё и смотрят.
// ---------------------------------------------------------------------------

#include "core/AnalysisTypes.h"

#include <QWidget>
#include <QImage>
#include <QVideoFrame>
#include <QElapsedTimer>
#include <QString>
#include <QVector>
#include <QRect>

namespace ui {

class VideoView : public QWidget
{
    Q_OBJECT

public:
    explicit VideoView(QWidget *parent = nullptr);

    /// Показать кадр. Может вызываться сколь угодно часто: лишнее
    /// отбрасывается здесь же.
    void setFrame(const QVideoFrame &frame);

    /// Убрать изображение и показать заглушку с указанной причиной.
    void clear(const QString &placeholder);

    // --- подписи поверх видео --------------------------------------------
    void setZoneLabel(const QString &text);
    void setSourceName(const QString &text);
    void setStatusText(const QString &text);
    void setStreamInfo(const QSize &frameSize, double fps);

    /// Результат ОДНОГО разбора кадра: рамки, скелеты, счётчик и уровень.
    ///
    /// Одним вызовом, а не порознь: между двумя вызовами панель успевает
    /// перерисоваться, и оператор на мгновение видит три рамки при счётчике
    /// «2». Мелочь, но именно из таких мелочей складывается впечатление, что
    /// алгоритм считает неверно.
    void setAnalysis(const core::PanelAnalysis &analysis);

    /// Сбросить разбор — источник сменили, прежние люди к новой картинке
    /// отношения не имеют.
    void clearAnalysis();

    /// Сигнал 2: панель участвует в тревоге — обводится красной рамкой.
    void setAlarmActive(bool active);

    /// Фаза мигания рамки тревоги. Переключается общим таймером окна: свой
    /// таймер на каждой панели означал бы четыре пробуждения вместо одного.
    void setBlinkPhase(bool bright);

    /// Рисовать ли скелеты. Оператор может их отключить: на четырёх панелях
    /// сразу они утомляют, а рамок с подписями обычно достаточно.
    void setSkeletonsVisible(bool visible);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void rescale();
    void drawOverlay(QPainter &painter);
    void drawPlaceholder(QPainter &painter);
    void drawPeople(QPainter &painter);
    void drawVerdictBanner(QPainter &painter);
    QRect placeBox(const QRect &box, double scaleX, double scaleY) const;

    QImage m_source;      ///< кадр в исходном размере
    QImage m_scaled;      ///< он же, подогнанный под виджет
    QRect m_target;       ///< куда именно вписан кадр

    QString m_placeholder = QStringLiteral("Источник не выбран");
    QString m_zoneLabel;
    QString m_sourceName;
    QString m_statusText;
    QSize m_frameSize;
    double m_fps = 0.0;
    bool m_alarmActive = false;
    bool m_blinkBright = true;
    /// Скелеты по умолчанию не рисуются.
    ///
    /// Оператору они не нужны: ему нужна рамка вокруг человека и вердикт
    /// словами. Семнадцать точек с линиями — это отладочная картинка, по
    /// которой проверяли биомеханику; включается в меню, когда понадобится.
    bool m_skeletons = false;

    core::PanelAnalysis m_analysis;

    QElapsedTimer m_paceTimer;

    /// Предел частоты показа. Камера отдаёт 30 кадров в секунду, глазу
    /// оператора хватает 25: лишние отбрасываются до всякой обработки, потому
    /// что самая дешёвая работа — та, которой не было.
    static constexpr qint64 kMinIntervalMs = 40;
    qint64 m_lastShownMs = 0;
};

} // namespace ui
