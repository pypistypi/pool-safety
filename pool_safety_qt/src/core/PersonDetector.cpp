#include "core/PersonDetector.h"
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

/// Чем заливаются поля при вписывании кадра. То же значение, что при обучении.
constexpr float kBorder = 114.f;

/// Площадь пересечения, делённая на площадь объединения.
float intersectionOverUnion(const QRect &a, const QRect &b)
{
    const QRect common = a.intersected(b);
    if (common.isEmpty())
        return 0.f;
    const float overlap = float(common.width()) * common.height();
    const float total = float(a.width()) * a.height()
                        + float(b.width()) * b.height() - overlap;
    return total > 0.f ? overlap / total : 0.f;
}

/// Семейство модели. Определяется по числу строк выхода, а не по имени файла:
/// имя можно переименовать, а форма выхода — свойство самой модели.
///
///   YOLOX  при входе S даёт (S/8)² + (S/16)² + (S/32)² строк  (640 → 8400)
///   YOLOv5 при том же входе даёт втрое больше — по три якоря  (640 → 25200)
enum class Family { Unknown, YoloX, YoloV5 };

Family familyFor(int inputSize, int64_t rows)
{
    const int64_t cells = int64_t(inputSize / 8) * (inputSize / 8)
                          + int64_t(inputSize / 16) * (inputSize / 16)
                          + int64_t(inputSize / 32) * (inputSize / 32);
    if (rows == cells)
        return Family::YoloX;
    if (rows == cells * 3)
        return Family::YoloV5;
    return Family::Unknown;
}

/// Сетка и шаги для разложения выхода YOLOX. Считается один раз на размер:
/// пересчитывать её на каждый кадр — восемь тысяч операций впустую.
struct YoloxGrid {
    std::vector<float> cellX;
    std::vector<float> cellY;
    std::vector<float> stride;
};

YoloxGrid buildGrid(int size)
{
    YoloxGrid grid;
    for (int stride : {8, 16, 32}) {
        const int side = size / stride;
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                grid.cellX.push_back(float(x));
                grid.cellY.push_back(float(y));
                grid.stride.push_back(float(stride));
            }
        }
    }
    return grid;
}

} // namespace

// ===========================================================================

struct PersonDetector::Impl {
    Config config;
    QString error = QStringLiteral("модель не загружена");
    Family family = Family::Unknown;
    int inputSize = 640;
    YoloxGrid grid;

    OrtEnv *env = nullptr;
    OrtSessionOptions *options = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *memory = nullptr;

    std::string inputName;
    std::string outputName;

    /// Буфер входного тензора. Переиспользуется от кадра к кадру: 640×640×3
    /// вещественных чисел — это 4,7 МБ, и выделять их заново по нескольку раз
    /// в секунду означало бы бессмысленно трясти память.
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

PersonDetector::PersonDetector()
    : d(std::make_unique<Impl>())
{
}

PersonDetector::~PersonDetector() = default;

QString PersonDetector::modelPath(Model model)
{
    const QString file = model == Model::Accurate ? QStringLiteral("models/yolox_s.onnx")
                       : model == Model::Fast     ? QStringLiteral("models/yolox_tiny.onnx")
                                                  : QStringLiteral("models/yolov5n.onnx");
    return QDir(QCoreApplication::applicationDirPath()).filePath(file);
}

QString PersonDetector::modelTitle(Model model)
{
    switch (model) {
    case Model::Accurate:
        return QStringLiteral("Точная (YOLOX-s) — меньше пропусков, ~150 мс на кадр");
    case Model::Fast:
        return QStringLiteral("Быстрая (YOLOX-tiny) — втрое быстрее, чуть больше пропусков");
    case Model::Legacy:
        return QStringLiteral("Прежняя (YOLOv5n) — лицензия AGPL-3.0, для объекта не годится");
    }
    return QString();
}

QString PersonDetector::defaultModelPath()
{
    return modelPath(Model::Accurate);
}

bool PersonDetector::isReady() const
{
    return d->session != nullptr && d->family != Family::Unknown;
}

QString PersonDetector::lastError() const
{
    return d->error;
}

QString PersonDetector::modelName() const
{
    const QString file = QFileInfo(d->config.modelPath).fileName();
    switch (d->family) {
    case Family::YoloX:  return QStringLiteral("%1 (YOLOX %2)").arg(file).arg(d->inputSize);
    case Family::YoloV5: return QStringLiteral("%1 (YOLOv5)").arg(file);
    case Family::Unknown: break;
    }
    return file;
}

bool PersonDetector::load(const Config &config)
{
    d->config = config;

    g_ort = ortApi();
    if (!g_ort) {
        d->error = ortRuntimeError();
        return false;
    }

    if (!QFileInfo::exists(config.modelPath)) {
        d->error = QStringLiteral("файл модели не найден: %1").arg(config.modelPath);
        return false;
    }

    QString message = takeStatus(g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR,
                                                  "pool_safety", &d->env));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }

    message = takeStatus(g_ort->CreateSessionOptions(&d->options));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }

    // Два потока на вычисления. Больше не ускоряет заметно, зато отнимает
    // ядра у отрисовки и декодирования видео.
    const int threads = std::max(1, config.cpuThreads);
    takeStatus(g_ort->SetIntraOpNumThreads(d->options, threads));
    takeStatus(g_ort->SetInterOpNumThreads(d->options, 1));
    takeStatus(g_ort->SetSessionGraphOptimizationLevel(d->options, ORT_ENABLE_ALL));

    // Путь передаётся широкими символами, поэтому кириллица в нём безопасна.
    // Это отдельно проверено: прежняя реализация на OpenCV на таком пути
    // спотыкалась, и модель приходилось грузить в обход.
    const std::wstring modelPath = QDir::toNativeSeparators(config.modelPath).toStdWString();
    message = takeStatus(g_ort->CreateSession(d->env, modelPath.c_str(),
                                              d->options, &d->session));
    if (!message.isEmpty()) {
        d->error = QStringLiteral("модель не открылась: %1").arg(message);
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

    // Имена входа и выхода спрашиваем у самой модели, а не задаём строкой:
    // при замене модели на другую они могут отличаться.
    char *name = nullptr;
    message = takeStatus(g_ort->SessionGetInputName(d->session, 0, allocator, &name));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }
    d->inputName = name;
    takeStatus(g_ort->AllocatorFree(allocator, name));

    name = nullptr;
    message = takeStatus(g_ort->SessionGetOutputName(d->session, 0, allocator, &name));
    if (!message.isEmpty()) {
        d->error = message;
        return false;
    }
    d->outputName = name;
    takeStatus(g_ort->AllocatorFree(allocator, name));

    // --- размер входа и семейство модели ---------------------------------
    //
    // Спрашиваем у самой модели: подменять файл модели администратор объекта
    // должен без правки настроек и уж точно без пересборки.
    OrtTypeInfo *inputInfo = nullptr;
    if (takeStatus(g_ort->SessionGetInputTypeInfo(d->session, 0, &inputInfo)).isEmpty()
        && inputInfo) {
        const OrtTensorTypeAndShapeInfo *shape = nullptr;
        if (takeStatus(g_ort->CastTypeInfoToTensorInfo(inputInfo, &shape)).isEmpty()
            && shape) {
            size_t count = 0;
            takeStatus(g_ort->GetDimensionsCount(shape, &count));
            std::vector<int64_t> dims(count, 0);
            takeStatus(g_ort->GetDimensions(shape, dims.data(), count));
            if (count == 4 && dims[2] > 0)
                d->inputSize = int(dims[2]);
        }
        g_ort->ReleaseTypeInfo(inputInfo);
    }

    OrtTypeInfo *outputInfo = nullptr;
    int64_t rows = 0;
    if (takeStatus(g_ort->SessionGetOutputTypeInfo(d->session, 0, &outputInfo)).isEmpty()
        && outputInfo) {
        const OrtTensorTypeAndShapeInfo *shape = nullptr;
        if (takeStatus(g_ort->CastTypeInfoToTensorInfo(outputInfo, &shape)).isEmpty()
            && shape) {
            size_t count = 0;
            takeStatus(g_ort->GetDimensionsCount(shape, &count));
            std::vector<int64_t> dims(count, 0);
            takeStatus(g_ort->GetDimensions(shape, dims.data(), count));
            if (count >= 2)
                rows = dims[count - 2];
        }
        g_ort->ReleaseTypeInfo(outputInfo);
    }

    d->family = familyFor(d->inputSize, rows);
    if (d->family == Family::Unknown) {
        d->error = QStringLiteral(
            "не удалось определить вид модели: вход %1, строк выхода %2. "
            "Ожидались YOLOX или YOLOv5.").arg(d->inputSize).arg(rows);
        return false;
    }

    if (d->family == Family::YoloX)
        d->grid = buildGrid(d->inputSize);

    d->input.assign(size_t(3) * d->inputSize * d->inputSize, 0.f);

    d->error.clear();
    return true;
}

QVector<Detection> PersonDetector::detect(const QImage &frame)
{
    if (!isReady() || frame.isNull())
        return {};

    const int side = d->inputSize;
    const int sourceWidth = frame.width();
    const int sourceHeight = frame.height();

    // --- вписывание кадра с сохранением пропорций -------------------------
    //
    // Растянуть кадр в квадрат было бы проще, но 16:9 при этом становится
    // приземистым, и модель, обученная на неискажённых снимках, узнаёт
    // человека хуже. Поэтому кадр вписывается целиком, а поля заливаются
    // серым — тем же значением, что при обучении.
    const double scale = std::min(double(side) / sourceWidth,
                                  double(side) / sourceHeight);
    const int fittedWidth = std::max(1, int(std::round(sourceWidth * scale)));
    const int fittedHeight = std::max(1, int(std::round(sourceHeight * scale)));

    const QImage fitted = frame
                              .scaled(fittedWidth, fittedHeight, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB888);
    if (fitted.isNull())
        return {};

    const bool yolox = d->family == Family::YoloX;

    // Раскладка «каналами»: сперва весь первый канал, затем второй, затем
    // третий — этого ждут обе модели. Порядок самих каналов разный: YOLOX
    // обучался на BGR, YOLOv5 — на RGB.
    float *plane0 = d->input.data();
    float *plane1 = plane0 + size_t(side) * side;
    float *plane2 = plane1 + size_t(side) * side;

    std::fill(d->input.begin(), d->input.end(), yolox ? kBorder : kBorder / 255.f);

    const float divisor = yolox ? 1.f : 255.f;
    for (int y = 0; y < fittedHeight; ++y) {
        const uchar *line = fitted.constScanLine(y);
        const size_t offset = size_t(y) * side;
        for (int x = 0; x < fittedWidth; ++x) {
            const float red   = line[x * 3 + 0] / divisor;
            const float green = line[x * 3 + 1] / divisor;
            const float blue  = line[x * 3 + 2] / divisor;
            plane0[offset + x] = yolox ? blue : red;
            plane1[offset + x] = green;
            plane2[offset + x] = yolox ? red : blue;
        }
    }

    const std::array<int64_t, 4> shape = {1, 3, side, side};
    OrtValue *inputTensor = nullptr;
    QString message = takeStatus(g_ort->CreateTensorWithDataAsOrtValue(
        d->memory, d->input.data(), d->input.size() * sizeof(float),
        shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensor));
    if (!message.isEmpty()) {
        d->error = message;
        return {};
    }

    const char *inputNames[] = {d->inputName.c_str()};
    const char *outputNames[] = {d->outputName.c_str()};
    OrtValue *outputTensor = nullptr;

    message = takeStatus(g_ort->Run(d->session, nullptr, inputNames,
                                    &inputTensor, 1, outputNames, 1, &outputTensor));
    g_ort->ReleaseValue(inputTensor);

    if (!message.isEmpty()) {
        d->error = message;
        if (outputTensor)
            g_ort->ReleaseValue(outputTensor);
        return {};
    }

    float *rowsData = nullptr;
    message = takeStatus(g_ort->GetTensorMutableData(outputTensor,
                                                     reinterpret_cast<void **>(&rowsData)));
    if (!message.isEmpty() || !rowsData) {
        g_ort->ReleaseValue(outputTensor);
        return {};
    }

    OrtTensorTypeAndShapeInfo *info = nullptr;
    message = takeStatus(g_ort->GetTensorTypeAndShape(outputTensor, &info));
    if (!message.isEmpty()) {
        g_ort->ReleaseValue(outputTensor);
        return {};
    }

    size_t dimensions = 0;
    takeStatus(g_ort->GetDimensionsCount(info, &dimensions));
    std::vector<int64_t> dims(dimensions, 0);
    takeStatus(g_ort->GetDimensions(info, dims.data(), dimensions));
    g_ort->ReleaseTensorTypeAndShapeInfo(info);

    if (dims.size() < 2) {
        g_ort->ReleaseValue(outputTensor);
        return {};
    }

    const int64_t rowCount = dims[dims.size() - 2];
    const int64_t rowSize = dims[dims.size() - 1];
    if (rowSize < 6) {
        g_ort->ReleaseValue(outputTensor);
        return {};
    }

    QVector<Detection> candidates;
    for (int64_t i = 0; i < rowCount; ++i) {
        const float *row = rowsData + i * rowSize;
        const float objectness = row[4];

        // Дешёвая отсечка до перебора восьмидесяти классов: если объекта тут
        // вовсе нет, разбирать его класс незачем.
        if (objectness < d->config.confidenceThreshold * 0.5f)
            continue;

        // Класс с наибольшей оценкой. Нас интересует только нулевой —
        // «человек». Всё прочее (мебель, инвентарь, животные) отбрасывается.
        int bestClass = 0;
        float bestScore = row[5];
        for (int64_t c = 1; c < rowSize - 5; ++c) {
            if (row[5 + c] > bestScore) {
                bestScore = row[5 + c];
                bestClass = int(c);
            }
        }
        if (bestClass != 0)
            continue;

        const float confidence = objectness * bestScore;
        if (confidence < d->config.confidenceThreshold)
            continue;

        // Координаты в системе вписанного кадра.
        float centerX = row[0];
        float centerY = row[1];
        float boxWidth = row[2];
        float boxHeight = row[3];

        if (yolox) {
            // YOLOX отдаёт смещение внутри ячейки и логарифм размера —
            // сетку надо разложить вручную.
            const size_t cell = size_t(i);
            if (cell >= d->grid.stride.size())
                continue;
            const float stride = d->grid.stride[cell];
            centerX = (centerX + d->grid.cellX[cell]) * stride;
            centerY = (centerY + d->grid.cellY[cell]) * stride;
            boxWidth = std::exp(boxWidth) * stride;
            boxHeight = std::exp(boxHeight) * stride;
        }

        // Снимаем вписывание: делим на масштаб, поля были справа и снизу.
        int x = int((centerX - boxWidth / 2.f) / scale);
        int y = int((centerY - boxHeight / 2.f) / scale);
        int w = int(boxWidth / scale);
        int h = int(boxHeight / scale);

        x = std::max(0, std::min(x, sourceWidth - 1));
        y = std::max(0, std::min(y, sourceHeight - 1));
        w = std::max(1, std::min(w, sourceWidth - x));
        h = std::max(1, std::min(h, sourceHeight - y));

        candidates.append(Detection{QRect(x, y, w, h), confidence});
    }

    g_ort->ReleaseValue(outputTensor);

    // Подавление перекрывающихся рамок: модель находит одного человека
    // несколько раз подряд, и без этого шага оператор увидел бы вместо одной
    // цели пять, а счётчик людей стал бы бессмысленным.
    std::sort(candidates.begin(), candidates.end(),
              [](const Detection &a, const Detection &b) {
                  return a.confidence > b.confidence;
              });

    QVector<Detection> kept;
    for (const Detection &candidate : std::as_const(candidates)) {
        bool overlaps = false;
        for (const Detection &accepted : std::as_const(kept)) {
            if (intersectionOverUnion(candidate.box, accepted.box)
                > d->config.nmsThreshold) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps)
            kept.append(candidate);
    }

    return kept;
}

} // namespace core
