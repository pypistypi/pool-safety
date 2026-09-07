# ---------------------------------------------------------------------------
#  Система видеонаблюдения за бассейном — настольное приложение (C++/Qt 6)
#
#  Переписано с Python-прототипа. Причина отказа от прежней реализации:
#  связка Python + OpenCV + веб-интерфейс требовала на каждый кадр
#  распаковать JPEG, обработать его интерпретатором и снова сжать, а картинку
#  показывала через встроенный браузер. Даже без распознавания это загружало
#  процессор до предела и давало рывки. Здесь декодированием и выводом
#  занимается сам Qt (аппаратное декодирование Windows), а интерпретатора нет
#  вовсе.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)

project(PoolSafety
    VERSION 0.4.0
    DESCRIPTION "Система видеонаблюдения за бассейном"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Автоматическая обработка Q_OBJECT (сигналы и слоты) и ресурсов.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Исходники содержат русский текст. Без явного указания кодировок MinGW
# считает литералы записанными в системной кодовой странице (для русской
# Windows это 1251) и на выходе получаются битые символы — ровно та беда,
# что была в прежней версии.
if(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# MultimediaWidgets не нужен: вывод изображения делает собственный виджет
# (см. пояснение в ui/VideoView.h), а лишняя библиотека — это лишние
# мегабайты в поставке.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Network Test)

qt_standard_project_setup()

# Значок программы и сведения о файле (название, версия, издатель).
#
# Без них программа выглядит как поделка: стандартный значок в панели задач и
# пустые свойства файла. Для системы, которую ставят на объект, это первое, на
# что смотрит чужой администратор.
#
# ОТДЕЛЬНОЙ ЦЕЛЬЮ, А НЕ В ОБЩЕМ СПИСКЕ ИСХОДНИКОВ — вынужденно. Компилятор
# ресурсов windres передаёт пути включения препроцессору без кавычек, и путь
# «ИИ наблюдение» разваливается по пробелу: «fatal error: наблюдение/...: No
# such file or directory». У отдельной цели путей включения нет вовсе, и
# проблема исчезает.
#
# Ключ -c 65001 говорит, что файл в UTF-8: иначе русские строки попадут в
# свойства файла битыми.
if(WIN32)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001")
    add_library(PoolSafetyResources OBJECT deploy/PoolSafety.rc)
endif()

qt_add_executable(PoolSafety WIN32
    src/main.cpp

    src/core/AppPaths.h
    src/core/AppPaths.cpp
    src/core/SourceDescriptor.h
    src/core/SourceDescriptor.cpp
    src/core/VideoSource.h
    src/core/VideoSource.cpp
    src/core/SourceHub.h
    src/core/SourceHub.cpp
    src/core/DeviceRegistry.h
    src/core/DeviceRegistry.cpp
    src/core/CameraDiscovery.h
    src/core/CameraDiscovery.cpp
    src/core/EventServer.h
    src/core/EventServer.cpp
    src/core/AlarmTypes.h
    src/core/AlarmTypes.cpp
    src/core/SiteInfo.h
    src/core/SiteInfo.cpp
    src/core/Settings.h
    src/core/Settings.cpp
    src/core/EventLog.h
    src/core/EventLog.cpp
    src/core/OrtRuntime.h
    src/core/OrtRuntime.cpp
    src/core/PersonDetector.h
    src/core/PersonDetector.cpp
    src/core/Skeleton.h
    src/core/PoseEstimator.h
    src/core/PoseEstimator.cpp
    src/core/Biomechanics.h
    src/core/Biomechanics.cpp
    src/core/PersonTracker.h
    src/core/PersonTracker.cpp
    src/core/SituationRules.h
    src/core/SituationRules.cpp
    src/core/AnalysisTypes.h
    src/core/AnalysisTypes.cpp
    src/core/AlarmController.h
    src/core/AlarmController.cpp
    src/core/AnalysisWorker.h
    src/core/AnalysisWorker.cpp
    src/core/BackgroundModel.h
    src/core/BackgroundModel.cpp

    src/ui/Theme.h
    src/ui/Theme.cpp
    src/ui/VideoView.h
    src/ui/VideoView.cpp
    src/ui/CameraPanel.h
    src/ui/CameraPanel.cpp
    src/ui/AlarmCard.h
    src/ui/AlarmCard.cpp
    src/ui/AlarmDialog.h
    src/ui/AlarmDialog.cpp
    src/ui/IpCameraDialog.h
    src/ui/IpCameraDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/SettingsDialog.cpp
    src/ui/EventLogDialog.h
    src/ui/EventLogDialog.cpp
    src/ui/Chime.h
    src/ui/Chime.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
)

# third_party — только заголовок ONNX Runtime. Сама библиотека подключается
# во время работы, по имени файла, и на этапе сборки не нужна (см.
# core/PersonDetector.cpp).
target_include_directories(PoolSafety PRIVATE src third_party/onnxruntime)

target_link_libraries(PoolSafety PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network
)

if(WIN32)
    target_sources(PoolSafety PRIVATE $<TARGET_OBJECTS:PoolSafetyResources>)
endif()

# Строгие предупреждения: на каркасе их ноль, и это стоит удерживать —
# именно мелкие недосмотры позже превращаются в зависания.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(PoolSafety PRIVATE -Wall -Wextra)
endif()

install(TARGETS PoolSafety RUNTIME DESTINATION .)

# ---------------------------------------------------------------------------
#  Проверки ядра. В поставку не входят: собираются и запускаются при
#  разработке. Интерфейс в них не участвует — проверяются правила, а не
#  расположение кнопок.
# ---------------------------------------------------------------------------
enable_testing()

qt_add_executable(PoolSafetyTests
    tests/test_core.cpp

    src/core/AppPaths.h
    src/core/AppPaths.cpp
    src/core/SourceDescriptor.h
    src/core/SourceDescriptor.cpp
    src/core/VideoSource.h
    src/core/VideoSource.cpp
    src/core/SourceHub.h
    src/core/SourceHub.cpp
    src/core/AlarmTypes.h
    src/core/AlarmTypes.cpp
    src/core/SiteInfo.h
    src/core/SiteInfo.cpp
    src/core/Settings.h
    src/core/Settings.cpp
    src/core/EventLog.h
    src/core/EventLog.cpp
    src/core/OrtRuntime.h
    src/core/OrtRuntime.cpp
    src/core/PersonDetector.h
    src/core/PersonDetector.cpp
    src/core/Skeleton.h
    src/core/PoseEstimator.h
    src/core/PoseEstimator.cpp
    src/core/Biomechanics.h
    src/core/Biomechanics.cpp
    src/core/PersonTracker.h
    src/core/PersonTracker.cpp
    src/core/SituationRules.h
    src/core/SituationRules.cpp
    src/core/AnalysisTypes.h
    src/core/AnalysisTypes.cpp
    src/core/AlarmController.h
    src/core/AlarmController.cpp
    src/core/AnalysisWorker.h
    src/core/AnalysisWorker.cpp
    src/core/BackgroundModel.h
    src/core/BackgroundModel.cpp
    src/core/CameraDiscovery.h
    src/core/CameraDiscovery.cpp
    src/core/EventServer.h
    src/core/EventServer.cpp
)

# Тесты должны писать в консоль, иначе их вывод пропадает и запускать их
# бессмысленно.
set_target_properties(PoolSafetyTests PROPERTIES WIN32_EXECUTABLE FALSE)

target_include_directories(PoolSafetyTests PRIVATE src third_party/onnxruntime)

# Тестам нужны настоящая модель и настоящий кадр с людьми: проверять
# распознавание на нарисованных фигурах бессмысленно — модель обучена на
# фотографиях и на рисунок не отзовётся.
target_compile_definitions(PoolSafetyTests PRIVATE
    TEST_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/models/yolov5n.onnx"
    TEST_POSE_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/models/rtmpose-m.onnx"
    TEST_ACCURATE_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/models/yolox_s.onnx"
    TEST_FAST_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/models/yolox_tiny.onnx"
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

target_link_libraries(PoolSafetyTests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Test)

add_test(NAME core COMMAND PoolSafetyTests)
