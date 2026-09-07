#include "core/PoseEstimator.h"
#include "core/OrtRuntime.h"

#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "ort_compat.h"

namespace core {

namespace {

const OrtApi *g_ort = nullptr;

QString takeStatus(OrtStatus *status) { return ortTakeStatus(status); }

// Вход модели: ширина 192, высота 256. Пропорция 3:4 — под фигуру человека.
constexpr int kInputWidth = 192;
constexpr int kInputHeight = 256;

// Нормализация ImageNet — та, с которой модель обучалась. Менять нельзя:
// другие числа дадут не «чуть хуже», а мусор на выходе.
constexpr float kMean[3] = {123.675f, 116.28f, 103.53f};
constexpr float kStd[3]  = {58.395f, 57.12f, 57.375f};

// Чем заполняются края, когда рамка выходит за кадр. Серый — то же значение,
// что в исходной реализации на Python.
constexpr float kBorder = 114.f;

/// Область вырезки: центр и размеры в координатах кадра.
struct CropBox {
    double centerX = 0.0;
    double centerY = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// Привести рамку к пропорции 3:4 и слегка расширить.
///
/// ЗАЧЕМ. Модель обучена на вырезках заданной пропорции. Подай ей
/// произвольный прямоугольник — фигура растянется, и все углы суставов
/// поедут, а именно углы мы и измеряем. Запас по краям нужен потому, что
/// детектор часто обрезает кисти и стопы, а они несут смысл: по кистям видно,
/// как человек бьёт по воде.
CropBox expandBox(const QRect &box)
{
    CropBox crop;
    crop.centerX = box.x() + box.width() / 2.0;
    crop.centerY = box.y() + box.height() / 2.0;

    double width = box.width() * 1.25;
    double height = box.height() * 1.25;

    constexpr double aspect = double(kInputWidth) / double(kInputHeight);
    if (width > height * aspect)
        height = width / aspect;
    else
        width = height * aspect;

    crop.width = std::max(1.0, width);
    crop.height = std::max(1.0, height);
    return crop;
}

/// Значение одного канала в точке кадра с билинейной интерполяцией.
/// За пределами кадра возвращается фон — так же, как это делал warpAffine.
inline float sampleChannel(const uchar *bits, int bytesPerLine,
                           int width, int height,
                           double x, double y, int channel)
{
    if (x < 0.0 || y < 0.0 || x > width - 1.0 || y > height - 1.0)
        return kBorder;

    const int x0 = int(x);
    const int y0 = int(y);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const double fx = x - x0;
    const double fy = y - y0;

    const uchar *row0 = bits + size_t(y0) * bytesPerLine;
    const uchar *row1 = bits + size_t(y1) * bytesPerLine;

    const double v00 = row0[x0 * 3 + channel];
    const double v01 = row0[x1 * 3 + channel];
    const double v10 = row1[x0 * 3 + channel];
    const double v11 = row1[x1 * 3 + channel];

    const double top = v00 + (v01 - v00) * fx;
    const double bottom = v10 + (v11 - v10) * fx;
    return float(top + (bottom - top) * fy);
}

} // namespace

// ===========================================================================

struct PoseEstimator::Impl {
    Config config;
    QString error = QStringLiteral("модель позы не загружена");

    OrtEnv *env = nullptr;
    OrtSessionOptions *options = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *memory = nullptr;

    std::string inputName;
    std::string outputXName;
    std::string outputYName;

    /// Буфер входа. Переиспользуется от кадра к кадру: восемь человек — это
    /// 4,7 МБ вещественных чисел, и выделять их заново по нескольку раз в
    /// секунду означало бы бессмысленно трясти память.
    std::vector<float> input;

    ~Impl()
    {
        if (!g_ort)
            return;
        if (session) g_ort->ReleaseSession(session);
        if (options) g_ort->ReleaseSessionOptions(options);
        if (memory)  g_ort->ReleaseMemoryInfo(memory);
        if (env)     g_ort->ReleaseEnv(env);
    }
};

PoseEstimator::PoseEstimator()
    : d(std::make_unique<Impl>())
{
}

PoseEstimator::~PoseEstimator() = default;

QString PoseEstimator::defaultModelPath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("models/rtmpose-m.onnx"));
}

bool PoseEstimator::isReady() const { return d->session != nullptr; }

QString PoseEstimator::lastError() const { return d->error; }

bool PoseEstimator::load(const Config &config)
{
    d->config = config;

    g_ort = ortApi();
    if (!g_ort) {
        d->error = ortRuntimeError();
        return false;
    }

    if (!QFileInfo::exists(config.modelPath)) {
        d->error = QStringLiteral("файл модели позы не найден: %1").arg(config.modelPath);
        return false;
    }

    QString message = takeStatus(g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR,
                                                  "pool_safety_pose", &d->env));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }

    message = takeStatus(g_ort->CreateSessionOptions(&d->options));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }

    const int threads = std::max(1, config.cpuThreads);
    takeStatus(g_ort->SetIntraOpNumThreads(d->options, threads));
    takeStatus(g_ort->SetInterOpNumThreads(d->options, 1));
    takeStatus(g_ort->SetSessionGraphOptimizationLevel(d->options, ORT_ENABLE_ALL));

    // Путь передаётся широкими символами — кириллица в нём безопасна.
    const std::wstring modelPath =
        QDir::toNativeSeparators(config.modelPath).toStdWString();
    message = takeStatus(g_ort->CreateSession(d->env, modelPath.c_str(),
                                              d->options, &d->session));
    if (!message.isEmpty()) {
        d->error = QStringLiteral("модель позы не открылась: %1").arg(message);
        return false;
    }

    message = takeStatus(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                                    &d->memory));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }

    OrtAllocator *allocator = nullptr;
    message = takeStatus(g_ort->GetAllocatorWithDefaultOptions(&allocator));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }

    char *name = nullptr;
    message = takeStatus(g_ort->SessionGetInputName(d->session, 0, allocator, &name));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }
    d->inputName = name;
    takeStatus(g_ort->AllocatorFree(allocator, name));

    // Выходов ровно два: распределение вдоль X и вдоль Y. Если модель отдаёт
    // иное число, это другая модель — и молча делать вид, что всё в порядке,
    // нельзя.
    size_t outputs = 0;
    message = takeStatus(g_ort->SessionGetOutputCount(d->session, &outputs));
    if (!message.isEmpty() || outputs < 2) {
        d->error = QStringLiteral(
            "у модели позы %1 выходов вместо двух — файл не тот").arg(outputs);
        return false;
    }

    for (size_t i = 0; i < 2; ++i) {
        name = nullptr;
        message = takeStatus(g_ort->SessionGetOutputName(d->session, i, allocator, &name));
        if (!message.isEmpty()) {
            d->error = message;
            return false;
        }
        (i == 0 ? d->outputXName : d->outputYName) = name;
        takeStatus(g_ort->AllocatorFree(allocator, name));
    }

    d->error.clear();
    return true;
}

QVector<Pose> PoseEstimator::estimate(const QImage &frame, const QVector<QRect> &boxes)
{
    if (!isReady() || frame.isNull() || boxes.isEmpty())
        return {};

    const int people = std::min(int(boxes.size()), std::max(1, d->config.maxPeople));

    const QImage source = frame.format() == QImage::Format_RGB888
                              ? frame
                              : frame.convertToFormat(QImage::Format_RGB888);
    if (source.isNull())
        return {};

    const uchar *bits = source.constBits();
    const int bytesPerLine = source.bytesPerLine();
    const int width = source.width();
    const int height = source.height();

    const size_t planeSize = size_t(kInputWidth) * kInputHeight;
    d->input.assign(size_t(people) * 3 * planeSize, 0.f);

    std::vector<CropBox> crops;
    crops.reserve(size_t(people));

    for (int index = 0; index < people; ++index) {
        const CropBox crop = expandBox(boxes.at(index));
        crops.push_back(crop);

        const double scaleX = crop.width / kInputWidth;
        const double scaleY = crop.height / kInputHeight;
        const double originX = crop.centerX - crop.width / 2.0;
        const double originY = crop.centerY - crop.height / 2.0;

        float *base = d->input.data() + size_t(index) * 3 * planeSize;
        float *red = base;
        float *green = base + planeSize;
        float *blue = base + 2 * planeSize;

        for (int y = 0; y < kInputHeight; ++y) {
            const double sourceY = originY + (y + 0.5) * scaleY;
            const size_t offset = size_t(y) * kInputWidth;
            for (int x = 0; x < kInputWidth; ++x) {
                const double sourceX = originX + (x + 0.5) * scaleX;
                red[offset + x] =
                    (sampleChannel(bits, bytesPerLine, width, height, sourceX, sourceY, 0)
                     - kMean[0]) / kStd[0];
                green[offset + x] =
                    (sampleChannel(bits, bytesPerLine, width, height, sourceX, sourceY, 1)
                     - kMean[1]) / kStd[1];
                blue[offset + x] =
                    (sampleChannel(bits, bytesPerLine, width, height, sourceX, sourceY, 2)
                     - kMean[2]) / kStd[2];
            }
        }
    }

    // Все люди прогоняются одной пачкой: накладные расходы на вызов модели
    // заметно больше, чем разница между одним человеком и восемью.
    const std::array<int64_t, 4> shape = {people, 3, kInputHeight, kInputWidth};
    OrtValue *inputTensor = nullptr;
    QString message = takeStatus(g_ort->CreateTensorWithDataAsOrtValue(
        d->memory, d->input.data(), d->input.size() * sizeof(float),
        shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensor));
    if (!message.isEmpty()) {
        d->error = message;
        return {};
    }

    const char *inputNames[] = {d->inputName.c_str()};
    const char *outputNames[] = {d->outputXName.c_str(), d->outputYName.c_str()};
    OrtValue *outputs[2] = {nullptr, nullptr};

    message = takeStatus(g_ort->Run(d->session, nullptr, inputNames, &inputTensor, 1,
                                    outputNames, 2, outputs));
    g_ort->ReleaseValue(inputTensor);

    if (!message.isEmpty()) {
        d->error = message;
        for (OrtValue *value : outputs) {
            if (value)
                g_ort->ReleaseValue(value);
        }
        return {};
    }

    // Разбор выхода. RTMPose предсказывает не «тепловую карту», а два
    // одномерных распределения — вдоль X и вдоль Y, каждое с двойным
    // разрешением. Координата точки — положение максимума, делённое на два;
    // уверенность — меньшая из двух высот.
    auto readTensor = [](OrtValue *value, const float **data,
                         std::vector<int64_t> *dims) -> bool {
        if (!value)
            return false;
        float *raw = nullptr;
        if (!ortTakeStatus(g_ort->GetTensorMutableData(
                value, reinterpret_cast<void **>(&raw))).isEmpty() || !raw)
            return false;

        OrtTensorTypeAndShapeInfo *info = nullptr;
        if (!ortTakeStatus(g_ort->GetTensorTypeAndShape(value, &info)).isEmpty())
            return false;
        size_t count = 0;
        ortTakeStatus(g_ort->GetDimensionsCount(info, &count));
        dims->assign(count, 0);
        ortTakeStatus(g_ort->GetDimensions(info, dims->data(), count));
        g_ort->ReleaseTensorTypeAndShapeInfo(info);

        *data = raw;
        return count == 3;
    };

    const float *simccX = nullptr;
    const float *simccY = nullptr;
    std::vector<int64_t> dimsX;
    std::vector<int64_t> dimsY;

    QVector<Pose> poses;

    if (readTensor(outputs[0], &simccX, &dimsX)
        && readTensor(outputs[1], &simccY, &dimsY)
        && dimsX[1] >= kp::Count && dimsY[1] >= kp::Count) {

        const int64_t binsX = dimsX[2];
        const int64_t binsY = dimsY[2];

        poses.reserve(people);
        for (int index = 0; index < people; ++index) {
            Pose pose;
            pose.box = boxes.at(index);

            const CropBox &crop = crops[size_t(index)];
            const double originX = crop.centerX - crop.width / 2.0;
            const double originY = crop.centerY - crop.height / 2.0;

            for (int joint = 0; joint < kp::Count; ++joint) {
                const float *rowX = simccX + (size_t(index) * dimsX[1] + joint) * binsX;
                const float *rowY = simccY + (size_t(index) * dimsY[1] + joint) * binsY;

                int64_t bestX = 0;
                float valueX = rowX[0];
                for (int64_t b = 1; b < binsX; ++b) {
                    if (rowX[b] > valueX) { valueX = rowX[b]; bestX = b; }
                }
                int64_t bestY = 0;
                float valueY = rowY[0];
                for (int64_t b = 1; b < binsY; ++b) {
                    if (rowY[b] > valueY) { valueY = rowY[b]; bestY = b; }
                }

                const float score = std::min(valueX, valueY);
                pose.scores[size_t(joint)] = score;

                if (score <= 0.f) {
                    // Точка, в которой модель не уверена вовсе, помечается
                    // нулём — пусть дальше по коду она честно считается
                    // невидимой, а не «найденной в углу кадра».
                    pose.points[size_t(joint)] = QPointF();
                    continue;
                }

                const double localX = bestX / 2.0;
                const double localY = bestY / 2.0;
                pose.points[size_t(joint)] = QPointF(
                    localX / kInputWidth * crop.width + originX,
                    localY / kInputHeight * crop.height + originY);
            }

            poses.append(pose);
        }
    }

    for (OrtValue *value : outputs) {
        if (value)
            g_ort->ReleaseValue(value);
    }

    return poses;
}

} // namespace core
