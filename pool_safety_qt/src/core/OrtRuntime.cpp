#include "core/OrtRuntime.h"

#include <QLibrary>

#include "ort_compat.h"

namespace core {

namespace {

const OrtApi *g_ort = nullptr;
QString g_error = QStringLiteral("библиотека распознавания ещё не подключалась");
bool g_attempted = false;

QString loadRuntime()
{
    // Ищем рядом с программой — там же, где остальные библиотеки. Если не
    // найдётся, QLibrary поищет по системным путям.
    QLibrary library(QStringLiteral("onnxruntime"));
    if (!library.load()) {
        return QStringLiteral("не удалось загрузить onnxruntime.dll (%1)")
            .arg(library.errorString());
    }

    using GetApiBaseFn = const OrtApiBase *(ORT_API_CALL *)();
    auto getApiBase = reinterpret_cast<GetApiBaseFn>(library.resolve("OrtGetApiBase"));
    if (!getApiBase)
        return QStringLiteral("в onnxruntime.dll нет OrtGetApiBase — файл повреждён");

    const OrtApiBase *base = getApiBase();
    if (!base)
        return QStringLiteral("onnxruntime.dll не отдала интерфейс");

    g_ort = base->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        return QStringLiteral(
            "версия onnxruntime.dll старее требуемой (нужна не ниже 1.19)");
    }
    return QString();
}

} // namespace

const OrtApi *ortApi()
{
    if (!g_attempted) {
        g_attempted = true;
        g_error = loadRuntime();
    }
    return g_ort;
}

QString ortRuntimeError()
{
    ortApi();
    return g_error;
}

QString ortTakeStatus(OrtStatus *status)
{
    if (!status || !g_ort)
        return QString();
    const QString message = QString::fromUtf8(g_ort->GetErrorMessage(status));
    g_ort->ReleaseStatus(status);
    return message.isEmpty() ? QStringLiteral("неизвестная ошибка") : message;
}

} // namespace core
