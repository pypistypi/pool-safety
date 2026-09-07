#pragma once

// ---------------------------------------------------------------------------
//  Скелет человека: какие точки даёт модель позы и как они связаны.
//
//  RTMPose выдаёт 17 точек в разметке COCO. Здесь они названы, потому что
//  дальше по коду обращения идут не к «точке 11», а к «левому бедру» — иначе
//  в биомеханических формулах невозможно разобраться и невозможно найти
//  ошибку.
//
//  ЛИЦО НЕ РАСПОЗНАЁТСЯ. Точки глаз, ушей и носа нужны ровно для двух вещей:
//  понять наклон головы и видна ли голова над водой. По пяти координатам
//  опознать человека нельзя и связать два его посещения нельзя — персональных
//  данных здесь не возникает.
// ---------------------------------------------------------------------------

#include <QPointF>
#include <QRect>

#include <array>

namespace core {

namespace kp {

enum Index {
    Nose = 0,
    LeftEye = 1,  RightEye = 2,
    LeftEar = 3,  RightEar = 4,
    LeftShoulder = 5, RightShoulder = 6,
    LeftElbow = 7,    RightElbow = 8,
    LeftWrist = 9,    RightWrist = 10,
    LeftHip = 11,     RightHip = 12,
    LeftKnee = 13,    RightKnee = 14,
    LeftAnkle = 15,   RightAnkle = 16,
    Count = 17
};

/// Точки головы. Если не видно ни одной — голова под водой или отвёрнута,
/// и это само по себе важный признак.
inline constexpr std::array<int, 5> head = {Nose, LeftEye, RightEye, LeftEar, RightEar};

/// Точки ниже пояса. В воде они почти всегда невидимы — по этому и
/// определяется, что человек в воде, пока линия воды не размечена.
inline constexpr std::array<int, 4> lowerBody = {LeftKnee, RightKnee, LeftAnkle, RightAnkle};

inline constexpr std::array<int, 4> torso = {LeftShoulder, RightShoulder, LeftHip, RightHip};

/// Связи для отрисовки скелета.
inline constexpr std::array<std::array<int, 2>, 18> limbs = {{
    {LeftShoulder, RightShoulder}, {LeftShoulder, LeftHip},
    {RightShoulder, RightHip},     {LeftHip, RightHip},
    {LeftShoulder, LeftElbow},     {LeftElbow, LeftWrist},
    {RightShoulder, RightElbow},   {RightElbow, RightWrist},
    {LeftHip, LeftKnee},           {LeftKnee, LeftAnkle},
    {RightHip, RightKnee},         {RightKnee, RightAnkle},
    {Nose, LeftEye},               {Nose, RightEye},
    {LeftEye, LeftEar},            {RightEye, RightEar},
    {LeftEar, LeftShoulder},       {RightEar, RightShoulder},
}};

} // namespace kp

/// Поза одного человека на одном кадре, в координатах ИСХОДНОГО кадра.
struct Pose {
    std::array<QPointF, kp::Count> points {};
    std::array<float, kp::Count> scores {};
    QRect box;

    /// Видна ли точка достаточно уверенно.
    ///
    /// Порог не строгий: под водой и в брызгах уверенность падает у всех
    /// точек сразу, и высокий порог просто ослепил бы систему.
    bool visible(int index, float threshold = 0.35f) const
    {
        return index >= 0 && index < kp::Count && scores[size_t(index)] >= threshold;
    }

    const QPointF &point(int index) const { return points[size_t(index)]; }

    /// Середина между двумя точками. Возвращает false, если хотя бы одна из
    /// них не видна: додумывать за модель нельзя.
    bool midpoint(int first, int second, QPointF *out) const
    {
        if (!visible(first) || !visible(second))
            return false;
        *out = (point(first) + point(second)) / 2.0;
        return true;
    }

    template <size_t N>
    int visibleCount(const std::array<int, N> &group) const
    {
        int count = 0;
        for (int index : group) {
            if (visible(index))
                ++count;
        }
        return count;
    }
};

} // namespace core
