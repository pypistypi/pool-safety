#pragma once

// ---------------------------------------------------------------------------
//  Оценка позы человека — RTMPose-m через ONNX Runtime, локально.
//
//  СХЕМА «СВЕРХУ ВНИЗ». Сначала работает детектор людей (PersonDetector,
//  YOLOv5n): он находит рамки. Затем каждая рамка вырезается и подаётся
//  модели позы. Так точнее, чем у моделей «всё сразу»: модель позы получает
//  человека крупным планом и не отвлекается на фон.
//
//  Плата — время растёт с числом людей: N человек = N вырезок. Для бассейна
//  это приемлемо (людей в кадре единицы), а при скоплении срабатывает предел
//  maxPeople: лучше разобрать восьмерых как следует, чем двадцатерых кое-как.
//
//  ПОЧЕМУ ИМЕННО RTMPose. Лицензия Apache 2.0 — модель можно поставить на
//  коммерческий объект. У YOLOv8-pose и YOLO11-pose лицензия AGPL-3.0: для
//  гостиницы это означало бы раскрытие исходных текстов всей системы либо
//  покупку отдельной лицензии.
//
//  ЛИЦА НЕ РАСПОЗНАЮТСЯ — см. пояснение в Skeleton.h.
// ---------------------------------------------------------------------------

#include "core/Skeleton.h"

#include <QString>
#include <QImage>
#include <QVector>
#include <QRect>

#include <memory>

namespace core {

class PoseEstimator
{
public:
    struct Config {
        QString modelPath;

        /// Четыре потока по тому же замеру: RTMPose на троих людях — 70 мс на
        /// двух потоках и 41 мс на четырёх.
        int cpuThreads = 4;
        int maxPeople = 8;
    };

    PoseEstimator();
    ~PoseEstimator();

    PoseEstimator(const PoseEstimator &) = delete;
    PoseEstimator &operator=(const PoseEstimator &) = delete;

    bool load(const Config &config);
    bool isReady() const;
    QString lastError() const;

    /// Посчитать позы для найденных людей. Вызывать только из потока
    /// разбора: метод занимает десятки миллисекунд на человека.
    QVector<Pose> estimate(const QImage &frame, const QVector<QRect> &boxes);

    static QString defaultModelPath();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace core
