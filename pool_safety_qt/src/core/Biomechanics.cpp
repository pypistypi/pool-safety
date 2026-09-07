#include "core/Biomechanics.h"

#include <QtMath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace core {

namespace {

double length(const QPointF &vector)
{
    return std::sqrt(vector.x() * vector.x() + vector.y() * vector.y());
}

/// Угол в суставе `vertex` между звеньями к `first` и `second`.
/// 180° — звено выпрямлено, 90° — согнуто под прямым углом.
/// Пусто — какая-то из трёх точек не видна, и врать нельзя.
std::optional<double> angleAt(const Pose &pose, int first, int vertex, int second)
{
    if (!pose.visible(first) || !pose.visible(vertex) || !pose.visible(second))
        return std::nullopt;

    const QPointF a = pose.point(first) - pose.point(vertex);
    const QPointF b = pose.point(second) - pose.point(vertex);
    const double na = length(a);
    const double nb = length(b);
    if (na < 1e-6 || nb < 1e-6)
        return std::nullopt;

    const double dot = a.x() * b.x() + a.y() * b.y();
    const double cosine = std::clamp(dot / (na * nb), -1.0, 1.0);
    return qRadiansToDegrees(std::acos(cosine));
}

/// Отклонение вектора от вертикали в градусах: 0 — стоит, 90 — лежит.
///
/// Ось экрана Y направлена вниз, поэтому вертикаль «вверх» — это (0, -1).
/// Знак отклонения не важен: наклон влево и вправо одинаково опасен.
double tiltFromVertical(const QPointF &vector)
{
    const double len = length(vector);
    if (len < 1e-6)
        return 0.0;
    const double cosine = std::clamp(-vector.y() / len, -1.0, 1.0);
    return qRadiansToDegrees(std::acos(cosine));
}

} // namespace

PoseFeatures extractFeatures(const Pose &pose)
{
    PoseFeatures features;

    QPointF shoulders;
    QPointF hips;
    const bool hasShoulders = pose.midpoint(kp::LeftShoulder, kp::RightShoulder, &shoulders);
    const bool hasHips = pose.midpoint(kp::LeftHip, kp::RightHip, &hips);

    // --- ось туловища: основа всех измерений ------------------------------
    if (hasShoulders && hasHips) {
        const QPointF axis = shoulders - hips;
        const double torso = length(axis);
        features.torsoLength = torso;
        if (torso > 1e-6)
            features.torsoTilt = tiltFromVertical(axis);
    }

    if (pose.visible(kp::LeftShoulder) && pose.visible(kp::RightShoulder)) {
        features.shoulderWidth =
            length(pose.point(kp::LeftShoulder) - pose.point(kp::RightShoulder));
    }

    const double scale = features.torsoLength.value_or(0.0);
    const bool hasScale = scale > 1e-6;

    // --- голова -----------------------------------------------------------
    QPointF headCenter;
    int headPoints = 0;
    for (int index : kp::head) {
        if (!pose.visible(index))
            continue;
        headCenter += pose.point(index);
        ++headPoints;
    }
    features.headVisible = headPoints > 0;

    if (headPoints > 0 && hasHips && hasScale) {
        headCenter /= headPoints;
        // Ось Y растёт вниз, поэтому «выше» — это меньшее значение.
        features.headAboveHips = (hips.y() - headCenter.y()) / scale;
    }

    // Размер головы: расстояние между ушами либо между глазами с поправкой.
    // Нужен, чтобы отличить ребёнка: у детей голова относительно тела больше.
    if (hasScale) {
        std::optional<double> headSize;
        if (pose.visible(kp::LeftEar) && pose.visible(kp::RightEar)) {
            headSize = length(pose.point(kp::LeftEar) - pose.point(kp::RightEar));
        } else if (pose.visible(kp::LeftEye) && pose.visible(kp::RightEye)) {
            // Между глазами примерно вдвое меньше, чем между ушами.
            headSize = 2.0 * length(pose.point(kp::LeftEye) - pose.point(kp::RightEye));
        }
        if (headSize && *headSize > 1e-6)
            features.headToTorso = *headSize / scale;
    }

    // --- углы в суставах --------------------------------------------------
    features.kneeLeft   = angleAt(pose, kp::LeftHip, kp::LeftKnee, kp::LeftAnkle);
    features.kneeRight  = angleAt(pose, kp::RightHip, kp::RightKnee, kp::RightAnkle);
    features.elbowLeft  = angleAt(pose, kp::LeftShoulder, kp::LeftElbow, kp::LeftWrist);
    features.elbowRight = angleAt(pose, kp::RightShoulder, kp::RightElbow, kp::RightWrist);
    features.hipLeft    = angleAt(pose, kp::LeftShoulder, kp::LeftHip, kp::LeftKnee);
    features.hipRight   = angleAt(pose, kp::RightShoulder, kp::RightHip, kp::RightKnee);

    // --- что видно --------------------------------------------------------
    features.lowerBodyRatio =
        double(pose.visibleCount(kp::lowerBody)) / double(kp::lowerBody.size());

    double sum = 0.0;
    int counted = 0;
    for (int index : kp::torso) { sum += pose.scores[size_t(index)]; ++counted; }
    for (int index : kp::head)  { sum += pose.scores[size_t(index)]; ++counted; }
    features.quality = counted > 0 ? sum / counted : 0.0;

    // --- руки -------------------------------------------------------------
    if (hasShoulders) {
        int raised = 0;
        for (int wrist : {kp::LeftWrist, kp::RightWrist}) {
            if (pose.visible(wrist) && pose.point(wrist).y() < shoulders.y())
                ++raised;
        }
        features.wristsAboveShoulders = raised;
    }

    if (hasScale && pose.visible(kp::LeftWrist) && pose.visible(kp::RightWrist)) {
        features.wristSpread =
            length(pose.point(kp::LeftWrist) - pose.point(kp::RightWrist)) / scale;
    }

    // --- центр масс и опора -----------------------------------------------
    QPointF center;
    int visible = 0;
    double minY = 0.0;
    double maxY = 0.0;
    for (int index = 0; index < kp::Count; ++index) {
        if (!pose.visible(index))
            continue;
        const QPointF &point = pose.point(index);
        center += point;
        if (visible == 0) {
            minY = maxY = point.y();
        } else {
            minY = std::min(minY, point.y());
            maxY = std::max(maxY, point.y());
        }
        ++visible;
    }
    if (visible > 0) {
        features.center = center / visible;
        features.bodyHeight = maxY - minY;
    }

    if (hasScale && pose.visible(kp::LeftAnkle) && pose.visible(kp::RightAnkle)) {
        features.supportWidth =
            std::abs(pose.point(kp::LeftAnkle).x() - pose.point(kp::RightAnkle).x()) / scale;
    }

    return features;
}

bool isHorizontal(const PoseFeatures &features, double threshold)
{
    return features.torsoTilt && *features.torsoTilt >= threshold;
}

bool isUpright(const PoseFeatures &features, double threshold)
{
    return features.torsoTilt && *features.torsoTilt <= threshold;
}

bool looksSubmerged(const PoseFeatures &features)
{
    return features.lowerBodyRatio <= 0.25;
}

} // namespace core
