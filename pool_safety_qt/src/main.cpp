// ---------------------------------------------------------------------------
//  Точка входа.
//
//  Здесь намеренно почти ничего нет. Вся настройка живёт в тех классах,
//  которых она касается: разбросанная по main логика — первое, что превращает
//  программу в неподдерживаемую.
// ---------------------------------------------------------------------------

#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "core/AlarmTypes.h"
#include "core/AnalysisTypes.h"
#include "core/EventLog.h"
#include "core/SingleInstance.h"
#include "core/Version.h"
#include "core/SituationRules.h"

#include <QApplication>
#include <QMessageBox>
#include <QVector>
#include <QRect>

#ifdef Q_OS_WIN
#include <windows.h>

namespace {

/// Не дать запустить вторую копию.
///
/// ЗАЧЕМ. Две копии поделили бы между собой камеры: Windows не отдаёт
/// устройство дважды, и вторая программа показывала бы пустые панели, а
/// оператор считал бы, что наблюдение идёт. Плюс обе писали бы в один журнал.
///
/// Имя мьютекса знает и установщик: по нему он понимает, что программа
/// запущена, и предлагает её закрыть, а не подменяет файлы на ходу.
bool claimSingleInstance()
{
    static HANDLE handle = CreateMutexW(nullptr, TRUE, L"PoolSafetyRunning");
    return handle != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
}

/// Разбудить уже работающую копию вместо тупикового сообщения.
///
/// true — окно первой копии найдено и разбужено, вторую можно тихо закрыть.
/// false — окна не нашли (редкий случай: висящий процесс без окна), тогда
/// стоит хотя бы объяснить, в чём дело, а не молча выйти.
bool wakeRunningInstance()
{
    const HWND window = FindWindowW(nullptr, core::singleInstance::kMainWindowTitle);
    if (!window)
        return false;

    // Окно могло быть свёрнуто в трей (скрыто) — обычный ShowWindow тут не
    // поможет, само окно ждёт именно это сообщение и знает, что с ним делать
    // (те же три вызова, что у пункта «Показать окно» в значке у часов).
    PostMessageW(window, core::singleInstance::restoreMessage(), 0, 0);
    return true;
}

} // namespace
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

#ifdef Q_OS_WIN
    if (!claimSingleInstance()) {
        // Первым делом пробуем разбудить работающую копию — это и есть то,
        // чего оператор ждёт, нажимая ярлык второй раз. Диалог ниже — только
        // если будить оказалось некого (окна не нашли).
        if (wakeRunningInstance())
            return 0;

        QMessageBox::information(
            nullptr, QStringLiteral("Программа уже запущена"),
            QStringLiteral(
                "«Наблюдение за бассейном» уже работает на этом компьютере, "
                "но её окно найти не удалось.\n\n"
                "Проверьте значок у часов — вероятно, программа свёрнута "
                "туда. Если и там её нет, завершите процесс PoolSafety.exe "
                "в диспетчере задач и запустите программу заново."));
        return 0;
    }
#endif

    QApplication::setApplicationName(QStringLiteral("Наблюдение за бассейном"));
    QApplication::setApplicationVersion(
        QString::fromLatin1(core::kAppVersion));
    QApplication::setOrganizationName(QStringLiteral("Pool Safety"));

    // Событие тревоги ходит между потоками, поэтому его тип нужно объявить
    // системе типов Qt — иначе соединение через очередь молча не сработает.
    qRegisterMetaType<core::AlarmEvent>("core::AlarmEvent");
    qRegisterMetaType<core::AlarmState>("core::AlarmState");
    // Рамки найденных людей едут из потока распознавания в поток окна.
    qRegisterMetaType<QVector<QRect>>("QVector<QRect>");
    // Итог разбора кадра и найденные опасные положения — тоже через очередь.
    qRegisterMetaType<core::PanelAnalysis>("core::PanelAnalysis");
    qRegisterMetaType<core::DangerReport>("core::DangerReport");
    qRegisterMetaType<core::Thresholds>("core::Thresholds");
    qRegisterMetaType<core::EventLog::Entry>("core::EventLog::Entry");

    app.setStyleSheet(ui::theme::applicationStyleSheet());

    ui::MainWindow window;
    window.show();

    return app.exec();
}
