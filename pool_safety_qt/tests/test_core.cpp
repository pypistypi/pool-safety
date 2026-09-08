// ---------------------------------------------------------------------------
//  Проверки ядра: разделение источников, порядок обработки тревоги, приём
//  кадров вычислительным потоком.
//
//  Интерфейс здесь не участвует сознательно. Проверять надо правила, а не
//  расположение кнопок: правила — это то, что ломается незаметно и всплывает
//  на объекте. Тесты консольные и идут за доли секунды, поэтому их не лень
//  запускать после каждой правки.
// ---------------------------------------------------------------------------

#include <QtTest>
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QImage>
#include <QPainter>
#include <QFileInfo>

#include "core/SourceDescriptor.h"
#include "core/SourceHub.h"
#include "core/VideoSource.h"
#include "core/MjpegWorker.h"
#include "core/AlarmController.h"
#include "core/AnalysisWorker.h"
#include "core/PersonDetector.h"
#include "core/PoseEstimator.h"
#include "core/Biomechanics.h"
#include "core/PersonTracker.h"
#include "core/SituationRules.h"
#include "core/Settings.h"
#include "core/EventLog.h"
#include "core/SiteInfo.h"
#include "core/CameraDiscovery.h"
#include "core/EventServer.h"
#include "core/AnalysisTypes.h"
#include "core/Version.h"
#include "core/BackgroundModel.h"

#include <QtMath>

class CoreTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // --- источники ---------------------------------------------------------
    void descriptorKeysDistinguishSources();
    void hubSharesOneSourceBetweenPanels();
    void hubClosesSourceWhenLastPanelLeaves();
    void stillImageIsPublishedOnce();

    // --- сигнал 1 ----------------------------------------------------------
    void presenceCountsAcrossPanels();
    void occupancyReportsOnlyRealChanges();
    void forgettingPanelClearsItsPeople();

    // --- сигнал 2 ----------------------------------------------------------
    void alarmFollowsOperatorSequence();
    void alarmCanBeDismissedWithoutAcknowledge();
    void secondAlarmDoesNotOverrideFirst();
    void presenceNeverRaisesAlarm();

    // --- вычислительный поток ---------------------------------------------
    void workerIgnoresFramesWhileDisabled();
    void workerKeepsOnlyNewestFramePerPanel();

    // --- распознавание человека -------------------------------------------
    void detectorFindsPeopleOnRealFrame();
    void detectorFindsNobodyOnEmptyPool();
    void detectorExplainsMissingModel();
    void detectorRecognisesBothModelFamilies();
    void everyModelFindsPeopleAndIgnoresEmptyPool();

    // --- устойчивость дорожек ---------------------------------------------
    void walkingPersonKeepsOneTrack();
    void disabledRuleStaysSilent();

    // --- данные для звонка ------------------------------------------------
    void siteTemplateMarksUnfilledAddress();
    void siteSurvivesSaveAndLoad();
    void callBriefStatesWhatAndWhere();
    void callBriefWarnsAboutMissingAddress();
    void alarmRemembersPeopleAndDetails();

    // --- биомеханика -------------------------------------------------------
    void tiltIsMeasuredFromVertical();
    void poseModelReturnsSkeletonOnRealFrame();

    // --- опасные положения: что ОБЯЗАНО сработать -------------------------
    void lyingStillInWaterRaisesAlarm();
    void motionlessInWaterRaisesAlarm();

    // --- спорные случаи ----------------------------------------------------
    void sunbatherOnLoungerIsNotAnAlarm();
    void sleepingAdultIsNotSupervision();

    // --- опасные положения: что обязано НЕ сработать ----------------------
    void standingPersonIsCalm();
    void swimmerIsNotDrowning();
    void playingInWaterIsNotDrowning();
    void walkingPersonDidNotFall();
    void oneBadFrameIsNotAFall();
    void unsteadyWalkNeverBecomesAlarm();
    void distantPasserbyIsNotJudgedByGait();
    void closeUpAdultIsNotMistakenForChild();
    void childRuleNeverRaisesAlarmUntilCalibrated();
    void distantAdultIsNotMistakenForChild();

    // --- защита от прежних ошибок -----------------------------------------
    void durationsUseFullHistoryNotWindow();
    void worstVerdictWinsBySeverity();
    void fallAlarmSurvivesBeyondHistoryWindow();
    void slowLieDownIsNotMistakenForAFall();

    // --- настройки и журнал ------------------------------------------------
    // --- IP-камеры ---------------------------------------------------------
    void streamUrlKeepsSchemeAndPort();
    void standardPortIsOmittedFromUrl();
    void wholeUrlIsParsedIntoParts();
    void smartphoneTemplateMatchesLiveCamera();
    void discoveryLooksBeyondPort554();

    // --- связь с телефоном --------------------------------------------------
    void phoneLearnsPanelsOnConnect();
    void phoneReceivesAlarmWithReadableRussian();
    void serverAnnouncesRealVersion();
    void zoneCountTakesTheBusiestCamera();
    void phoneCanAcknowledgeAlarm();
    void phoneCanCloseIncident();
    void alarmStateReachesPhone();
    void lateJoinerLearnsAlarmIsRunning();

    // --- постоянное окружение -----------------------------------------------
    void backgroundStaysSilentUntilTrained();
    void backgroundTellsStillFromBusy();
    void backgroundSurvivesSaveAndLoad();
    void backgroundRefusesForeignGrid();
    void rtspFallsBackToHttpForPhoneCamera();
    void reconnectBackoffGrowsAndCaps();
    void staleSettingsAreRewrittenAfterMigration();
    void newDefaultModelIsFast();

    // --- собственный приёмник MJPEG -------------------------------------
    void mjpegExtractsSingleCompleteFrame();
    void mjpegWaitsForIncompletePayload();
    void mjpegSkipsStaleFramesUnderBacklog();
    void mjpegRecoversFromGarbageBeforeHeader();
    void mjpegAcceptsLowercaseHeader();
    void mjpegReadsRawJpegWithoutAnyHeaders();
    void mjpegSkipsStaleRawFramesUnderBacklog();
    void mjpegWaitsForIncompleteRawFrame();

    // --- темп разбора -------------------------------------------------------
    void verdictSurvivesBetweenPoseRuns();

    void settingsSurviveSaveAndLoad();
    void invalidFpsCapFallsBackToThirty();
    void eventLogWritesReadableFile();

private:
    QTemporaryDir m_dir;
    QString m_imagePath;
};

void CoreTests::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_imagePath = m_dir.filePath(QStringLiteral("frame.png"));

    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QVERIFY(image.save(m_imagePath));
}

// =========================================================== источники

void CoreTests::descriptorKeysDistinguishSources()
{
    const auto camA = core::SourceDescriptor::camera(QStringLiteral("dev-1"),
                                                     QStringLiteral("Камера 1"));
    const auto camB = core::SourceDescriptor::camera(QStringLiteral("dev-2"),
                                                     QStringLiteral("Камера 2"));
    const auto same = core::SourceDescriptor::camera(QStringLiteral("dev-1"),
                                                     QStringLiteral("Другое имя"));

    // Совпадение определяет устройство, а не подпись: одна и та же камера,
    // названная по-разному, обязана считаться одним источником — иначе она
    // откроется дважды и вторая панель останется без картинки.
    QCOMPARE(camA.key(), same.key());
    QVERIFY(camA.key() != camB.key());
    QVERIFY(camA == same);

    QVERIFY(core::SourceDescriptor::file(QStringLiteral("C:/видео/x.mp4")).isStillImage() == false);
    QVERIFY(core::SourceDescriptor::file(QStringLiteral("C:/фото/x.JPG")).isStillImage());
    QVERIFY(core::SourceDescriptor::none().isNone());
}

void CoreTests::hubSharesOneSourceBetweenPanels()
{
    core::SourceHub hub;
    const auto descriptor = core::SourceDescriptor::file(m_imagePath);

    core::VideoSource *first = hub.acquire(descriptor);
    core::VideoSource *second = hub.acquire(descriptor);

    QVERIFY(first != nullptr);
    QCOMPARE(first, second);              // один объект на обе панели
    QCOMPARE(hub.activeSourceCount(), 1); // и одно открытое устройство
}

void CoreTests::hubClosesSourceWhenLastPanelLeaves()
{
    core::SourceHub hub;
    const auto descriptor = core::SourceDescriptor::file(m_imagePath);

    hub.acquire(descriptor);
    hub.acquire(descriptor);

    hub.release(descriptor);
    QCOMPARE(hub.activeSourceCount(), 1);  // вторая панель ещё смотрит

    hub.release(descriptor);
    QCOMPARE(hub.activeSourceCount(), 0);  // отпустили все — устройство свободно

    // Лишнее освобождение не должно ничего ломать: панель могла отписаться
    // и при смене источника, и при закрытии окна.
    hub.release(descriptor);
    QCOMPARE(hub.activeSourceCount(), 0);
}

void CoreTests::stillImageIsPublishedOnce()
{
    core::SourceHub hub;
    core::VideoSource *source = hub.acquire(core::SourceDescriptor::file(m_imagePath));
    QVERIFY(source != nullptr);

    // Фотография отдаёт кадр сразу при открытии, поэтому панель, подключённая
    // позже, всё равно получит картинку — через lastFrame.
    QVERIFY(source->lastFrame().isValid());
    QCOMPARE(source->lastFrame().size(), QSize(64, 48));
}

// ============================================================ сигнал 1

void CoreTests::presenceCountsAcrossPanels()
{
    core::AlarmController alarm;
    QSignalSpy spy(&alarm, &core::AlarmController::presenceChanged);

    alarm.reportPresence(0, 2);
    alarm.reportPresence(1, 3);

    // СЧЁТ ПО МАКСИМУМУ, А НЕ ПО СУММЕ.
    //
    // Четыре камеры смотрят на одну и ту же чашу с разных сторон. Человек у
    // воды виден сразу с нескольких, и сложение превращало его в четверых:
    // оператор читал «в зоне людей: 8» там, где их двое. Счётчику, который
    // врёт втрое, перестают верить целиком — а он и есть сигнал присутствия.
    QCOMPARE(alarm.peopleInZone(), 3);
    QCOMPARE(alarm.peopleOnPanel(0), 2);
    QCOMPARE(alarm.peopleOnPanel(1), 3);
    QCOMPARE(spy.count(), 2);

    // Повтор того же числа сигнала не порождает: иначе индикатор перерисовывался
    // бы на каждом кадре без всякой причины.
    alarm.reportPresence(0, 2);
    QCOMPARE(spy.count(), 2);
}

void CoreTests::occupancyReportsOnlyRealChanges()
{
    core::AlarmController alarm;
    QSignalSpy spy(&alarm, &core::AlarmController::zoneOccupancyChanged);

    alarm.reportPresence(0, 1);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    alarm.reportPresence(0, 4);   // людей стало больше — зона всё ещё занята
    QCOMPARE(spy.count(), 0);

    alarm.reportPresence(0, 0);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

void CoreTests::forgettingPanelClearsItsPeople()
{
    core::AlarmController alarm;
    alarm.reportPresence(0, 2);
    alarm.reportPresence(1, 1);

    alarm.forgetPanel(0);   // на панели сменили источник
    QCOMPARE(alarm.peopleInZone(), 1);
    QCOMPARE(alarm.peopleOnPanel(0), 0);
}

// ============================================================ сигнал 2

void CoreTests::alarmFollowsOperatorSequence()
{
    core::AlarmController alarm;
    QSignalSpy raised(&alarm, &core::AlarmController::alarmRaised);
    QSignalSpy acknowledged(&alarm, &core::AlarmController::alarmAcknowledged);
    QSignalSpy confirmed(&alarm, &core::AlarmController::alarmConfirmed);
    QSignalSpy closed(&alarm, &core::AlarmController::alarmClosed);

    core::AlarmEvent event;
    event.panelId = 2;
    event.zoneLabel = QStringLiteral("Юго-западный угол");
    event.origin = core::AlarmOrigin::Manual;

    alarm.raiseAlarm(event);
    QCOMPARE(alarm.state(), core::AlarmState::Raised);
    QCOMPARE(raised.count(), 1);
    QVERIFY(alarm.currentEvent().raisedAt.isValid());  // время проставлено само

    alarm.acknowledge();
    QCOMPARE(alarm.state(), core::AlarmState::Acknowledged);
    QCOMPARE(acknowledged.count(), 1);
    QVERIFY(alarm.currentEvent().acknowledgedAt.isValid());

    alarm.confirm();
    QCOMPARE(alarm.state(), core::AlarmState::Confirmed);
    QCOMPARE(confirmed.count(), 1);

    alarm.closeIncident();
    QCOMPARE(alarm.state(), core::AlarmState::Idle);
    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.takeFirst().at(1).toBool(), false);  // не ложная
}

void CoreTests::alarmCanBeDismissedWithoutAcknowledge()
{
    core::AlarmController alarm;
    QSignalSpy closed(&alarm, &core::AlarmController::alarmClosed);

    core::AlarmEvent event;
    event.zoneLabel = QStringLiteral("вся зона");
    alarm.raiseAlarm(event);

    // Оператор видит на экране, что тревога ложная. Требовать сперва нажать
    // «Принял» — лишний шаг в момент, когда дорога каждая секунда.
    alarm.dismissAsFalse();
    QCOMPARE(alarm.state(), core::AlarmState::Idle);
    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.takeFirst().at(1).toBool(), true);
}

void CoreTests::secondAlarmDoesNotOverrideFirst()
{
    core::AlarmController alarm;

    core::AlarmEvent first;
    first.zoneLabel = QStringLiteral("первая");
    alarm.raiseAlarm(first);

    core::AlarmEvent second;
    second.zoneLabel = QStringLiteral("вторая");
    alarm.raiseAlarm(second);

    // Подмена события под руками оператора недопустима: он принимал решение
    // по первой тревоге и должен видеть именно её.
    QCOMPARE(alarm.currentEvent().zoneLabel, QStringLiteral("первая"));
}

void CoreTests::presenceNeverRaisesAlarm()
{
    core::AlarmController alarm;
    QSignalSpy raised(&alarm, &core::AlarmController::alarmRaised);

    // Несущий принцип системы: сколько бы людей ни вошло в зону, тревогой это
    // не становится никогда. Тревога — только про утопление и потерю сознания.
    for (int panel = 0; panel < 4; ++panel)
        alarm.reportPresence(panel, 25);

    // Двадцать пять, а не сотня: камеры смотрят на одну чашу, и это те же
    // самые люди, увиденные с четырёх сторон.
    QCOMPARE(alarm.peopleInZone(), 25);
    QCOMPARE(alarm.state(), core::AlarmState::Idle);
    QCOMPARE(raised.count(), 0);
}

// ================================================== вычислительный поток

void CoreTests::workerIgnoresFramesWhileDisabled()
{
    core::AnalysisWorker worker;
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::black);
    const QVideoFrame frame(image);

    // Модель ещё не загружена — кадры не должны даже копироваться.
    QVERIFY(!worker.isEnabled());
    for (int i = 0; i < 100; ++i)
        worker.submitFrame(0, frame);
    QCOMPARE(worker.droppedFrames(), 0);
}

void CoreTests::workerKeepsOnlyNewestFramePerPanel()
{
    core::AnalysisWorker worker;
    QSignalSpy ready(&worker, &core::AnalysisWorker::detectorReady);
    worker.start(QStringLiteral(TEST_MODEL_PATH),
                 QStringLiteral(TEST_POSE_MODEL_PATH));

    QCOMPARE(ready.count(), 1);
    QVERIFY2(ready.first().at(0).toBool(),
             qPrintable(ready.first().at(1).toString()));
    QVERIFY(worker.isEnabled());

    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::white);
    const QVideoFrame frame(image);

    // Разбор здесь не крутится (цикл событий не запущен), поэтому все кадры,
    // кроме последнего, обязаны быть отброшены. Иначе очередь росла бы без
    // конца, съедая память и показывая оператору всё более старую картинку.
    for (int i = 0; i < 50; ++i)
        worker.submitFrame(0, frame);

    QCOMPARE(worker.droppedFrames(), 49);
}

// ================================================== распознавание человека

void CoreTests::detectorFindsPeopleOnRealFrame()
{
    // Кадр вырезан из настоящей записи с людьми. Проверять распознавание на
    // нарисованных фигурах бессмысленно: модель обучена на фотографиях.
    const QString framePath = QStringLiteral(TEST_DATA_DIR "/people_frame.png");
    QVERIFY2(QFileInfo::exists(framePath), "нет тестового кадра tests/data/people_frame.png");

    core::PersonDetector detector;
    core::PersonDetector::Config config;
    config.modelPath = QStringLiteral(TEST_MODEL_PATH);
    QVERIFY2(detector.load(config), qPrintable(detector.lastError()));

    const QImage frame(framePath);
    QVERIFY(!frame.isNull());

    const QVector<core::Detection> found = detector.detect(frame);
    QVERIFY2(found.size() >= 2,
             qPrintable(QStringLiteral("на кадре с тремя людьми найдено %1")
                            .arg(found.size())));

    for (const core::Detection &detection : found) {
        QVERIFY(detection.confidence >= config.confidenceThreshold);
        // Рамка обязана лежать внутри кадра: наружу её вынес бы неверный
        // пересчёт координат из размера модели в размер кадра.
        QVERIFY(frame.rect().contains(detection.box));
        QVERIFY(detection.box.width() > 0 && detection.box.height() > 0);
    }
}

void CoreTests::detectorFindsNobodyOnEmptyPool()
{
    const QString photoPath = QStringLiteral(TEST_DATA_DIR "/empty_pool.jpg");
    QVERIFY2(QFileInfo::exists(photoPath), "нет тестового фото tests/data/empty_pool.jpg");

    core::PersonDetector detector;
    core::PersonDetector::Config config;
    config.modelPath = QStringLiteral(TEST_MODEL_PATH);
    QVERIFY2(detector.load(config), qPrintable(detector.lastError()));

    // Ложная тревога на пустом бассейне дороже пропуска: оператор перестанет
    // верить рамкам и начнёт их игнорировать.
    QCOMPARE(detector.detect(QImage(photoPath)).size(), 0);
}

void CoreTests::detectorExplainsMissingModel()
{
    core::PersonDetector detector;
    core::PersonDetector::Config config;
    config.modelPath = m_dir.filePath(QStringLiteral("нет-такого-файла.onnx"));

    QVERIFY(!detector.load(config));
    QVERIFY(!detector.isReady());
    // Программа обязана объяснить, почему распознавания нет, а не молча
    // показывать видео, в котором «никого не находит».
    QVERIFY(!detector.lastError().isEmpty());
    QVERIFY(detector.detect(QImage(64, 64, QImage::Format_RGB32)).isEmpty());
}

// ==================================================== данные для звонка

void CoreTests::siteTemplateMarksUnfilledAddress()
{
    const core::SiteInfo info = core::SiteInfo::defaults();

    // Заготовка не должна выглядеть как готовые настройки: по выдуманному
    // адресу уедет скорая.
    QVERIFY(!info.isAddressReady());
    QVERIFY(info.address.contains(QStringLiteral("НЕ ЗАПОЛНЕНО")));

    // А телефоны экстренных служб — настоящие и работают сразу.
    bool has112 = false;
    for (const core::EmergencyContact &contact : info.contacts)
        if (contact.phone == QStringLiteral("112"))
            has112 = true;
    QVERIFY(has112);
}

void CoreTests::siteSurvivesSaveAndLoad()
{
    core::SiteInfo info = core::SiteInfo::defaults();
    info.address = QStringLiteral("Краснодарский край, Сочи, Красная Поляна, ул. Берёзовая, 1");
    info.objectName = QStringLiteral("Гостиница Пример, крытый бассейн");
    info.contacts = {{QStringLiteral("Спасатель"), QStringLiteral("+7 900 000-00-00"),
                      QStringLiteral("на месте")}};

    const QString path = m_dir.filePath(QStringLiteral("site.json"));
    QVERIFY(info.save(path));

    const core::SiteInfo loaded = core::SiteInfo::load(path);
    QCOMPARE(loaded.address, info.address);
    QCOMPARE(loaded.objectName, info.objectName);
    QCOMPARE(loaded.contacts.size(), 1);
    QCOMPARE(loaded.contacts.first().phone, QStringLiteral("+7 900 000-00-00"));
    QVERIFY(loaded.isAddressReady());
}

void CoreTests::callBriefStatesWhatAndWhere()
{
    core::SiteInfo site = core::SiteInfo::defaults();
    site.address = QStringLiteral("Сочи, Красная Поляна, ул. Берёзовая, 1");
    site.objectName = QStringLiteral("Гостиница Пример");
    site.entrance = QStringLiteral("корпус Б, цокольный этаж");

    core::AlarmEvent event;
    event.zoneLabel = QStringLiteral("Юго-западный угол");
    event.circumstance = core::AlarmCircumstance::UnconsciousInWater;
    event.details = QStringLiteral("мужчина, около 40 лет");
    event.peopleInZone = 3;
    event.raisedAt = QDateTime::currentDateTime();

    const QString brief = core::buildCallBrief(event, site);

    // Диспетчер спрашивает ровно это: что случилось, где, как пройти.
    QVERIFY(brief.contains(core::circumstanceText(core::AlarmCircumstance::UnconsciousInWater)));
    QVERIFY(brief.contains(site.address));
    QVERIFY(brief.contains(site.entrance));
    QVERIFY(brief.contains(event.zoneLabel));
    QVERIFY(brief.contains(event.details));
    QVERIFY(brief.contains(QStringLiteral("3")));
}

void CoreTests::callBriefWarnsAboutMissingAddress()
{
    const core::SiteInfo site = core::SiteInfo::defaults();   // адрес не заполнен

    core::AlarmEvent event;
    event.circumstance = core::AlarmCircumstance::Drowning;
    event.raisedAt = QDateTime::currentDateTime();

    const QString brief = core::buildCallBrief(event, site);

    // Пустая строка на месте адреса заставила бы оператора ждать подсказки,
    // которой нет. Он должен сразу понять, что адрес надо назвать самому.
    QVERIFY(brief.contains(QStringLiteral("АДРЕС НЕ ЗАДАН")));
}

void CoreTests::alarmRemembersPeopleAndDetails()
{
    core::AlarmController alarm;
    alarm.reportPresence(0, 2);
    alarm.reportPresence(1, 1);

    core::AlarmEvent event;
    event.zoneLabel = QStringLiteral("Северо-западный угол");
    event.circumstance = core::AlarmCircumstance::Drowning;
    alarm.raiseAlarm(event);

    // Обстановка запоминается на момент происшествия: диспетчеру нужно знать,
    // сколько людей было тогда, а не сколько сбежалось потом. Число — по самой
    // многолюдной камере: все они смотрят на один бассейн.
    QCOMPARE(alarm.currentEvent().peopleInZone, 2);
    alarm.reportPresence(2, 5);
    QCOMPARE(alarm.currentEvent().peopleInZone, 2);

    alarm.updateDetails(QStringLiteral("ребёнок, у дальнего бортика"));
    QCOMPARE(alarm.currentEvent().details, QStringLiteral("ребёнок, у дальнего бортика"));
    QCOMPARE(alarm.currentEvent().circumstance, core::AlarmCircumstance::Drowning);
}

// ===================================================== биомеханика и правила
//
//  ЗАЧЕМ ЗДЕСЬ ИСКУССТВЕННЫЕ ПОЗЫ. Проверять правила на записи нельзя: запись
//  придётся хранить, она стареет, и по ней невозможно задать точное условие
//  вроде «человек лежал ровно двенадцать секунд». Здесь скелет строится
//  формулой, поэтому каждое условие задаётся однозначно, а проверка идёт за
//  миллисекунды.
//
//  Сама модель распознавания проверяется отдельно, на настоящем кадре
//  (poseModelReturnsSkeletonOnRealFrame): нарисованные фигуры ей показывать
//  бессмысленно.

namespace {

/// Построить скелет человека заданного наклона и положения.
///
/// hips      — середина таза в точках экрана;
/// torso     — длина туловища (мера масштаба для всех признаков);
/// tilt      — наклон туловища от вертикали, °: 0 — стоит, 90 — лежит;
/// legs      — видны ли колени и лодыжки (в воде их не видно);
/// head      — видна ли голова;
/// headRatio — ширина головы в долях туловища (признак ребёнка).
core::Pose makePose(const QPointF &hips, double torso, double tilt,
                    bool legs = true, bool head = true, double headRatio = 0.25)
{
    core::Pose pose;

    const double radians = qDegreesToRadians(tilt);
    // Направление «вверх вдоль туловища». Ось Y экрана растёт вниз.
    const QPointF up(std::sin(radians) * torso, -std::cos(radians) * torso);
    const QPointF side(-up.y() / torso, up.x() / torso);   // единичная поперечная

    const QPointF shoulders = hips + up;
    const QPointF headCenter = shoulders + up * 0.42;

    auto put = [&pose](int index, const QPointF &point, float score) {
        pose.points[size_t(index)] = point;
        pose.scores[size_t(index)] = score;
    };

    put(core::kp::LeftShoulder,  shoulders + side * (torso * 0.22), 1.f);
    put(core::kp::RightShoulder, shoulders - side * (torso * 0.22), 1.f);
    put(core::kp::LeftHip,       hips + side * (torso * 0.16), 1.f);
    put(core::kp::RightHip,      hips - side * (torso * 0.16), 1.f);

    const float headScore = head ? 1.f : 0.f;
    put(core::kp::Nose,     headCenter, headScore);
    put(core::kp::LeftEye,  headCenter + side * (torso * 0.06), headScore);
    put(core::kp::RightEye, headCenter - side * (torso * 0.06), headScore);
    put(core::kp::LeftEar,  headCenter + side * (torso * headRatio / 2.0), headScore);
    put(core::kp::RightEar, headCenter - side * (torso * headRatio / 2.0), headScore);

    const float legScore = legs ? 1.f : 0.f;
    const QPointF leftKnee  = hips + side * (torso * 0.16) - up * 0.5;
    const QPointF rightKnee = hips - side * (torso * 0.16) - up * 0.5;
    put(core::kp::LeftKnee,   leftKnee, legScore);
    put(core::kp::RightKnee,  rightKnee, legScore);
    put(core::kp::LeftAnkle,  leftKnee - up * 0.5, legScore);
    put(core::kp::RightAnkle, rightKnee - up * 0.5, legScore);

    const QPointF leftElbow  = shoulders + side * (torso * 0.30) - up * 0.35;
    const QPointF rightElbow = shoulders - side * (torso * 0.30) - up * 0.35;
    put(core::kp::LeftElbow,  leftElbow, 1.f);
    put(core::kp::RightElbow, rightElbow, 1.f);
    put(core::kp::LeftWrist,  leftElbow - up * 0.35, 1.f);
    put(core::kp::RightWrist, rightElbow - up * 0.35, 1.f);

    // Рамка нужна сопровождению; для правил её точность роли не играет.
    pose.box = QRect(int(hips.x() - torso), int(hips.y() - torso * 1.8),
                     int(torso * 2), int(torso * 3));
    return pose;
}

/// Найти вердикт по нужному положению. Отсутствие означает «норма».
core::Verdict verdictFor(const QVector<core::Verdict> &verdicts, core::Situation wanted)
{
    for (const core::Verdict &verdict : verdicts) {
        if (verdict.situation == wanted)
            return verdict;
    }
    return core::Verdict{};
}

QVector<core::Verdict> analyseTrack(const core::Track &track, bool waterInView = false)
{
    core::SituationAnalyzer analyzer;
    return analyzer.analyse(track, {&track}, waterInView);
}

} // namespace

void CoreTests::tiltIsMeasuredFromVertical()
{
    const core::PoseFeatures standing =
        core::extractFeatures(makePose(QPointF(500, 500), 100, 0));
    QVERIFY(standing.isUsable());
    QVERIFY(qAbs(*standing.torsoTilt) < 1.0);
    QVERIFY(core::isUpright(standing));
    QVERIFY(!core::isHorizontal(standing));
    QCOMPARE(standing.lowerBodyRatio, 1.0);

    const core::PoseFeatures lying =
        core::extractFeatures(makePose(QPointF(500, 500), 100, 88));
    QVERIFY(qAbs(*lying.torsoTilt - 88.0) < 1.0);
    QVERIFY(core::isHorizontal(lying));

    // Ног не видно — по этому признаку и определяется вода, пока её линия не
    // размечена оператором.
    const core::PoseFeatures inWater =
        core::extractFeatures(makePose(QPointF(500, 500), 100, 5, false));
    QVERIFY(core::looksSubmerged(inWater));

    // Длина туловища — мера, в долях которой считается всё остальное.
    QVERIFY(qAbs(*standing.torsoLength - 100.0) < 1.0);
}

void CoreTests::poseModelReturnsSkeletonOnRealFrame()
{
    const QString framePath = QStringLiteral(TEST_DATA_DIR "/people_frame.png");
    QVERIFY2(QFileInfo::exists(framePath), "нет тестового кадра tests/data/people_frame.png");

    QImage frame(framePath);
    QVERIFY(!frame.isNull());

    core::PersonDetector detector;
    core::PersonDetector::Config config;
    config.modelPath = QStringLiteral(TEST_MODEL_PATH);
    config.confidenceThreshold = 0.35f;
    QVERIFY2(detector.load(config), qPrintable(detector.lastError()));

    const QVector<core::Detection> people = detector.detect(frame);
    QVERIFY2(!people.isEmpty(), "детектор не нашёл людей на кадре с людьми");

    core::PoseEstimator estimator;
    core::PoseEstimator::Config poseConfig;
    poseConfig.modelPath = QStringLiteral(TEST_POSE_MODEL_PATH);
    QVERIFY2(estimator.load(poseConfig), qPrintable(estimator.lastError()));

    QVector<QRect> boxes;
    for (const core::Detection &person : people)
        boxes.append(person.box);

    const QVector<core::Pose> poses = estimator.estimate(frame, boxes);
    QCOMPARE(poses.size(), boxes.size());

    // Хотя бы у одного человека туловище должно распознаться уверенно: без оси
    // туловища не считается ни один признак.
    int usable = 0;
    for (const core::Pose &pose : poses) {
        const core::PoseFeatures features = core::extractFeatures(pose);
        if (!features.isUsable())
            continue;
        ++usable;
        // Точки обязаны лежать рядом с кадром: ошибка в обратном пересчёте
        // координат вылезла бы именно здесь.
        for (int index = 0; index < core::kp::Count; ++index) {
            if (!pose.visible(index))
                continue;
            const QPointF &point = pose.point(index);
            QVERIFY(point.x() > -frame.width() && point.x() < frame.width() * 2);
            QVERIFY(point.y() > -frame.height() && point.y() < frame.height() * 2);
        }
    }
    QVERIFY2(usable > 0, "ни у одного найденного человека не распозналось туловище");
}

// -------------------------------------------- что ОБЯЗАНО поднять тревогу

void CoreTests::lyingStillInWaterRaisesAlarm()
{
    // Неподвижность в воде недопустима ни при каких обстоятельствах, и
    // терпение здесь секундное: ног не видно, тело горизонтально, человек не
    // шевелится.
    core::Track track(1, 0.0);
    for (double time = 0.0; time <= 12.0; time += 0.4)
        track.add(time, makePose(QPointF(600, 400), 100, 85, false));

    const core::Verdict verdict =
        verdictFor(analyseTrack(track, true), core::Situation::Unconscious);
    QCOMPARE(verdict.level, core::Level::Alarm);
    QVERIFY2(!verdict.reasons.isEmpty(), "тревога без объяснения бесполезна оператору");
    QVERIFY(verdict.reasons.first().contains(QStringLiteral("без движения")));
}

void CoreTests::motionlessInWaterRaisesAlarm()
{
    core::Track track(1, 0.0);
    // Вертикально, ног не видно, на месте — то самое сочетание, ради которого
    // всё и затевалось. Пловец сюда не попадёт: он продвигается.
    for (double time = 0.0; time <= 12.0; time += 0.4)
        track.add(time, makePose(QPointF(600, 400), 100, 8, false));

    const core::Verdict verdict =
        verdictFor(analyseTrack(track, true), core::Situation::Drowning);
    QCOMPARE(verdict.level, core::Level::Alarm);
    QVERIFY(verdict.reasons.join(QLatin1Char(' '))
                .contains(QStringLiteral("нет продвижения")));

    // А без отметки «здесь вода» то же самое молчит: невидимые ноги сами по
    // себе воду не доказывают. Проверено на веб-камере в комнате — сидящий за
    // столом человек получал тревогу.
    QCOMPARE(verdictFor(analyseTrack(track, false), core::Situation::Drowning).level,
             core::Level::Normal);
}

// ---------------------------------------- что обязано НЕ поднять тревогу

void CoreTests::standingPersonIsCalm()
{
    core::Track track(1, 0.0);
    double x = 400.0;
    for (double time = 0.0; time <= 8.0; time += 0.4) {
        track.add(time, makePose(QPointF(x, 400), 100, 3));
        x += 3.0;   // стоит и слегка переминается
    }

    QVERIFY2(analyseTrack(track).isEmpty(),
             "спокойно стоящий человек не должен тревожить оператора");
}

void CoreTests::swimmerIsNotDrowning()
{
    core::Track track(1, 0.0);
    double x = 200.0;
    // Плывёт: тело горизонтально, ног не видно, продвигается.
    for (double time = 0.0; time <= 10.0; time += 0.4) {
        track.add(time, makePose(QPointF(x, 400), 100, 80, false));
        x += 20.0;
    }

    const QVector<core::Verdict> verdicts = analyseTrack(track, true);
    QCOMPARE(verdictFor(verdicts, core::Situation::Drowning).level, core::Level::Normal);
    QCOMPARE(verdictFor(verdicts, core::Situation::Unconscious).level, core::Level::Normal);
}

void CoreTests::playingInWaterIsNotDrowning()
{
    core::Track track(1, 0.0);
    double x = 200.0;
    // Вертикально в воде, но перемещается и голова уверенно наверху. Именно
    // продвижение и отличает играющего от тонущего — больше ничем они на
    // картинке не различаются.
    for (double time = 0.0; time <= 12.0; time += 0.4) {
        track.add(time, makePose(QPointF(x, 400), 100, 10, false));
        x += 25.0;
    }

    QCOMPARE(verdictFor(analyseTrack(track, true), core::Situation::Drowning).level,
             core::Level::Normal);
}

void CoreTests::walkingPersonDidNotFall()
{
    core::Track track(1, 0.0);
    double x = 200.0;
    int step = 0;
    for (double time = 0.0; time <= 8.0; time += 0.4) {
        // Наклон гуляет на пару градусов — так и ходит живой человек.
        const double tilt = (step % 2 == 0) ? 2.0 : 4.0;
        track.add(time, makePose(QPointF(x, 400), 100, tilt));
        x += 18.0;
        ++step;
    }

    QCOMPARE(verdictFor(analyseTrack(track), core::Situation::Fall).level,
             core::Level::Normal);
}

void CoreTests::oneBadFrameIsNotAFall()
{
    core::Track track(1, 0.0);
    double time = 0.0;
    double x = 200.0;

    for (; time <= 6.0; time += 0.4) {
        track.add(time, makePose(QPointF(x, 400), 100, 3));
        x += 15.0;
    }

    // Один-единственный кадр, на котором модель перепутала плечи с бёдрами.
    // Без сглаживания медианой это выглядело как поворот тела на семьдесят
    // градусов за долю секунды — и правило падения срабатывало на спокойно
    // идущем человеке. Проверено: именно так и было.
    track.add(time, makePose(QPointF(x, 400), 100, 70));
    time += 0.4;
    x += 15.0;

    for (; time <= 9.0; time += 0.4) {
        track.add(time, makePose(QPointF(x, 400), 100, 3));
        x += 15.0;
    }

    QCOMPARE(verdictFor(analyseTrack(track), core::Situation::Fall).level,
             core::Level::Normal);
}

void CoreTests::unsteadyWalkNeverBecomesAlarm()
{
    core::Track track(1, 0.0);
    double x = 200.0;
    int step = 0;
    for (double time = 0.0; time <= 8.0; time += 0.4) {
        // Путь виляет: центр тела уходит вбок и возвращается.
        const double y = 400.0 + ((step % 2 == 0) ? -45.0 : 45.0);
        track.add(time, makePose(QPointF(x, y), 100, 5));
        x += 20.0;
        ++step;
    }

    const core::Verdict verdict =
        verdictFor(analyseTrack(track), core::Situation::Unsteady);
    QCOMPARE(verdict.level, core::Level::Attention);

    // Выше «внимания» это правило не поднимается НИКОГДА: по видео нельзя
    // установить опьянение — та же походка бывает при головокружении, травме,
    // у пожилого человека и просто на мокром кафеле.
    QVERIFY(verdict.level != core::Level::Alarm);
}

void CoreTests::distantPasserbyIsNotJudgedByGait()
{
    // Тот же виляющий путь, но человек далеко: туловище 30 точек экрана —
    // ровно как на записи 768x576, где правило объявляло виляние у людей,
    // идущих по прямой. Дрожание точек скелета не зависит от размера фигуры,
    // а виляние меряется в долях туловища, поэтому у дальнего человека оно
    // раздувается само собой. О походке такой фигуры судить нельзя.
    core::Track track(1, 0.0);
    double x = 200.0;
    int step = 0;
    for (double time = 0.0; time <= 8.0; time += 0.4) {
        const double y = 400.0 + ((step % 2 == 0) ? -13.5 : 13.5);
        track.add(time, makePose(QPointF(x, y), 30, 5));
        x += 6.0;
        ++step;
    }

    QCOMPARE(verdictFor(analyseTrack(track), core::Situation::Unsteady).level,
             core::Level::Normal);
}

void CoreTests::closeUpAdultIsNotMistakenForChild()
{
    // Взрослый сидит вплотную к камере: туловище видно в ракурсе и потому
    // измеряется коротким, голова относительно него — огромной. На веб-камере
    // так и вышло: отношение 0,75 и ТРЕВОГА «ребёнок в воде без взрослого».
    //
    // Значение вне полосы правдоподобия — это сломанное измерение, а не
    // сверхулика. Плюс фигура видна не целиком, и судить по ней о пропорциях
    // нельзя вовсе.
    core::Track track(1, 0.0);
    for (double time = 0.0; time <= 10.0; time += 0.4) {
        // Ног не видно (закрыты столом), голова крупная относительно туловища.
        track.add(time, makePose(QPointF(600, 500), 60, 8, false, true, 0.75));
    }

    QCOMPARE(verdictFor(analyseTrack(track, true), core::Situation::ChildAlone).level,
             core::Level::Normal);
    QCOMPARE(verdictFor(analyseTrack(track, false), core::Situation::ChildAlone).level,
             core::Level::Normal);
}

void CoreTests::childRuleNeverRaisesAlarmUntilCalibrated()
{
    // Признак ребёнка не проверен ни на одном настоящем ребёнке, поэтому по
    // умолчанию он не имеет права включать сирену. Проверка держит это
    // свойство: правило, поднимающее тревогу по непроверенному признаку, за
    // смену приучит оператора не верить сирене вообще.
    core::Track track(1, 0.0);
    for (double time = 0.0; time <= 12.0; time += 0.4) {
        // Пропорции ребёнка, фигура видна целиком, взрослых рядом нет.
        track.add(time, makePose(QPointF(600, 500), 100, 5, true, true, 0.55));
    }

    const core::Verdict quiet =
        verdictFor(analyseTrack(track, true), core::Situation::ChildAlone);
    QCOMPARE(quiet.level, core::Level::Attention);

    // А после калибровки на детях — включается и работает как тревога.
    core::Thresholds calibrated;
    calibrated.childAlarmAllowed = true;
    core::SituationAnalyzer analyzer;
    analyzer.setThresholds(calibrated);
    const core::Verdict loud =
        verdictFor(analyzer.analyse(track, {&track}, true), core::Situation::ChildAlone);
    QCOMPARE(loud.level, core::Level::Alarm);
}

void CoreTests::distantAdultIsNotMistakenForChild()
{
    // Двое: один ближе к камере, другой дальше и потому мельче. Разница в
    // размере — это расстояние, а не возраст, и камера их не различает.
    // Проверено на записи с прохожими: дальний взрослый опознавался ребёнком.
    core::Track near(1, 0.0);
    core::Track far(2, 0.0);
    for (double time = 0.0; time <= 8.0; time += 0.4) {
        near.add(time, makePose(QPointF(300, 700), 100, 4));
        far.add(time, makePose(QPointF(900, 300), 55, 4));   // вдвое мельче
    }

    core::SituationAnalyzer analyzer;
    const QVector<const core::Track *> both = {&near, &far};
    QCOMPARE(verdictFor(analyzer.analyse(far, both), core::Situation::ChildAlone).level,
             core::Level::Normal);
}


void CoreTests::detectorRecognisesBothModelFamilies()
{
    // Вид модели определяется по форме её выхода, а не по имени файла: имя
    // можно переименовать, форма — свойство самой модели. Благодаря этому
    // администратор объекта подменяет файл модели, не трогая настройки.
    struct Case { const char *path; const char *expect; };
    const QVector<Case> cases = {
        {TEST_MODEL_PATH, "YOLOv5"},
        {TEST_ACCURATE_MODEL_PATH, "YOLOX"},
        {TEST_FAST_MODEL_PATH, "YOLOX"},
    };

    for (const Case &item : cases) {
        core::PersonDetector detector;
        core::PersonDetector::Config config;
        config.modelPath = QString::fromUtf8(item.path);
        QVERIFY2(detector.load(config), qPrintable(detector.lastError()));
        QVERIFY2(detector.modelName().contains(QString::fromUtf8(item.expect)),
                 qPrintable(detector.modelName()));
    }
}

void CoreTests::everyModelFindsPeopleAndIgnoresEmptyPool()
{
    // Каждая из трёх моделей обязана найти людей на кадре с людьми и никого —
    // на пустом бассейне.
    //
    // Сравнивать модели МЕЖДУ СОБОЙ на одном кадре бессмысленно, и это
    // проверено: на этом снимке прежняя модель находит на одного человека
    // больше точной, а на записи из 120 кадров точная выигрывает при каждом
    // пороге (5,7 против 5,3 найденных в среднем, провалы счёта 35 % против
    // 56 %). Один кадр — не выборка, и делать из него проверку значило бы
    // закрепить случайность.
    const QString framePath = QStringLiteral(TEST_DATA_DIR "/people_frame.png");
    const QString emptyPath = QStringLiteral(TEST_DATA_DIR "/empty_pool.jpg");
    QVERIFY2(QFileInfo::exists(framePath), "нет тестового кадра");

    const QImage frame(framePath);
    const QImage empty(emptyPath);
    QVERIFY(!frame.isNull());
    QVERIFY(!empty.isNull());

    const QVector<QString> models = {QStringLiteral(TEST_MODEL_PATH),
                                     QStringLiteral(TEST_ACCURATE_MODEL_PATH),
                                     QStringLiteral(TEST_FAST_MODEL_PATH)};

    for (const QString &path : models) {
        core::PersonDetector detector;
        core::PersonDetector::Config config;
        config.modelPath = path;
        config.confidenceThreshold = 0.25f;
        QVERIFY2(detector.load(config), qPrintable(detector.lastError()));

        const int found = int(detector.detect(frame).size());
        QVERIFY2(found >= 3, qPrintable(QStringLiteral("%1 нашла лишь %2")
                                            .arg(detector.modelName()).arg(found)));

        QVERIFY2(detector.detect(empty).isEmpty(),
                 qPrintable(QStringLiteral("%1 нашла людей на пустом бассейне")
                                .arg(detector.modelName())));
    }
}

void CoreTests::walkingPersonKeepsOneTrack()
{
    // САМАЯ ДОРОГАЯ ИЗ НАЙДЕННЫХ ОШИБОК. Дорожки рвались: за четыре десятых
    // секунды идущий человек смещается на полкорпуса, его новая рамка почти не
    // перекрывается с прежней, и сопоставление по пересечению заводило нового
    // человека. Замерено на записи: дорожка жила 1,2 секунды при порогах
    // правил в 5–10 секунд — то есть правила не могли сработать никогда.
    //
    // Здесь человек идёт с тем же шагом, что и на записи: половина ширины
    // рамки за разбор.
    core::PersonTracker tracker;
    double x = 200.0;
    QVector<int> seen;

    for (int step = 0; step < 20; ++step) {
        const double time = step * 0.4;
        core::Pose pose = makePose(QPointF(x, 500), 100, 4);
        pose.box = QRect(int(x) - 100, 200, 200, 300);
        const QVector<int> updated = tracker.update({pose}, time);
        QCOMPARE(updated.size(), 1);
        if (!seen.contains(updated.first()))
            seen.append(updated.first());
        x += 150.0;   // полторы четверти ширины рамки за разбор
    }

    QCOMPARE(seen.size(), 1);

    const core::Track *track = tracker.track(seen.first());
    QVERIFY(track != nullptr);
    QVERIFY2(track->age() > 7.0,
             qPrintable(QStringLiteral("дорожка прожила лишь %1 с")
                            .arg(track->age())));
}

void CoreTests::disabledRuleStaysSilent()
{
    // Выключенное оператором правило обязано молчать совсем — иначе выключатель
    // ничего не значит, а оператор перестанет верить настройкам.
    core::Track track(1, 0.0);
    for (double time = 0.0; time <= 12.0; time += 0.4)
        track.add(time, makePose(QPointF(600, 400), 100, 85, false));

    QCOMPARE(verdictFor(analyseTrack(track, true), core::Situation::Unconscious).level,
             core::Level::Alarm);

    core::Thresholds off;
    off.ruleUnconscious = false;
    core::SituationAnalyzer analyzer;
    analyzer.setThresholds(off);
    QVERIFY(analyzer.analyse(track, {&track}, true).isEmpty());
}


void CoreTests::sunbatherOnLoungerIsNotAnAlarm()
{
    // ЗАГОРАЮЩИЙ И ЧЕЛОВЕК БЕЗ СОЗНАНИЯ НА СНИМКЕ ОДИНАКОВЫ: оба лежат
    // неподвижно. Отличить их по картинке нельзя ни человеком, ни машиной.
    // Поэтому на суше система не поднимает сирену за десяток секунд — иначе
    // каждый отдыхающий на лежаке будил бы оператора, и через смену тот
    // перестал бы верить сирене вообще.
    core::Track track(1, 0.0);
    for (double time = 0.0; time <= 60.0; time += 0.5)
        track.add(time, makePose(QPointF(600, 400), 100, 85));   // ноги видны

    const core::Verdict verdict =
        verdictFor(analyseTrack(track), core::Situation::Unconscious);

    QVERIFY2(verdict.level != core::Level::Alarm,
             "загорающий на лежаке не должен поднимать тревогу");
    QCOMPARE(verdict.level, core::Level::Attention);

    // Тот же человек в воде — тревога немедленно: там лежать неподвижно
    // нельзя ни при каких обстоятельствах.
    core::Track inWater(2, 0.0);
    for (double time = 0.0; time <= 12.0; time += 0.4)
        inWater.add(time, makePose(QPointF(600, 400), 100, 85, false));
    QCOMPARE(verdictFor(analyseTrack(inWater, true), core::Situation::Unconscious).level,
             core::Level::Alarm);
}

void CoreTests::sleepingAdultIsNotSupervision()
{
    // СПЯЩИЙ ВЗРОСЛЫЙ — НЕ ПРИСМОТР. Случай известный и кончался гибелью
    // ребёнка: мать спала на лежаке, дочь в это время утонула в бассейне.
    // Формально взрослый был рядом, фактически ребёнок был один.
    //
    // Ребёнок в воде (ног не видно, пропорции детские), рядом крупный
    // человек, который не двигался всё это время.
    core::Track child(1, 0.0);
    core::Track adult(2, 0.0);

    for (double time = 0.0; time <= 30.0; time += 0.5) {
        child.add(time, makePose(QPointF(600, 500), 60, 8, false, true, 0.55));
        adult.add(time, makePose(QPointF(720, 520), 110, 85));   // лежит, не двигается
    }

    core::SituationAnalyzer analyzer;
    const QVector<const core::Track *> both = {&child, &adult};
    const core::Verdict verdict =
        verdictFor(analyzer.analyse(child, both, true), core::Situation::ChildAlone);

    QCOMPARE(verdict.level, core::Level::Alarm);
    QVERIFY2(verdict.reasons.join(QLatin1Char(' '))
                 .contains(QStringLiteral("неподвижен")),
             qPrintable(verdict.reasons.join(QStringLiteral("; "))));

    // А бодрствующий взрослый рядом снимает вопрос: это и есть присмотр.
    core::Track awake(3, 0.0);
    int step = 0;
    for (double time = 0.0; time <= 30.0; time += 0.5) {
        // Стоит рядом и переминается: движется, но никуда не уходит.
        const double x = 700.0 + ((step % 2 == 0) ? -12.0 : 12.0);
        awake.add(time, makePose(QPointF(x, 520), 110, 5));
        ++step;
    }
    const QVector<const core::Track *> withAwake = {&child, &awake};
    QCOMPARE(verdictFor(analyzer.analyse(child, withAwake, true),
                        core::Situation::ChildAlone).level,
             core::Level::Normal);
}

// ------------------------------------------------ защита от прежних ошибок

void CoreTests::durationsUseFullHistoryNotWindow()
{
    // САМАЯ ОПАСНАЯ ИЗ НАЙДЕННЫХ ОШИБОК. Длительности состояний считались
    // внутри шестисекундного окна, поэтому порог «десять секунд неподвижности»
    // был недостижим в принципе: система молчала бы там, где обязана кричать.
    core::Track track(1, 0.0);
    for (double time = 0.0; time <= 12.0; time += 0.4)
        track.add(time, makePose(QPointF(600, 400), 100, 85));

    const core::WindowMetrics metrics = track.metrics(6.0);
    QVERIFY(qAbs(metrics.span - 6.0) < 0.5);      // окно осталось окном
    QVERIFY2(metrics.horizontalSeconds > 10.0,    // а длительность — по истории
             "длительность состояния снова считается внутри окна");
    QVERIFY(metrics.stillSeconds > 10.0);
}

void CoreTests::fallAlarmSurvivesBeyondHistoryWindow()
{
    // ПРОПУЩЕННАЯ ТРЕВОГА, ВОСПРОИЗВЕДЁННАЯ И ИСПРАВЛЕННАЯ. Правило падения
    // раньше сверялось с резкостью перехода в горизонталь заново на каждом
    // разборе, а резкость считалась по хранимой истории (не длиннее
    // PersonTracker::kHistorySeconds = 15 с). Человек, упавший и оставшийся
    // лежать дольше пятнадцати секунд, переставал давать основание: сам
    // всплеск наклона уходил из истории, и правило гасло — оператор видел
    // «норма», хотя человек так и лежал без сознания. Именно так одна
    // настоящая тревога однажды не прозвучала.
    core::Track track(1, 0.0);
    double time = 0.0;

    // Стоит.
    for (; time <= 1.2; time += 0.4)
        track.add(time, makePose(QPointF(600, 400), 100, 2));

    // Падает — резкий переход в горизонталь за одну выборку (около 200°/с,
    // втрое больше порога).
    time += 0.4;   // 1,6 с
    track.add(time, makePose(QPointF(600, 400), 100, 85));

    // И лежит без движения ещё двадцать три секунды — заведомо дольше
    // пятнадцатисекундной хранимой истории, внутри которой случилось падение.
    for (; time <= 25.0; time += 0.4)
        track.add(time, makePose(QPointF(600, 400), 100, 85));

    QVERIFY2(track.fallTiltRate() > 0.0,
             "резкость падения не должна обнуляться, пока человек не встал");

    const core::Verdict verdict =
        verdictFor(analyseTrack(track), core::Situation::Fall);
    QCOMPARE(verdict.level, core::Level::Alarm);
    QVERIFY2(verdict.reasons.join(QLatin1Char(' ')).contains(QStringLiteral("не встаёт")),
             "тревога без объяснения бесполезна оператору");
}

void CoreTests::slowLieDownIsNotMistakenForAFall()
{
    // Загорающий садится на лежак и ложится за несколько секунд, не рывком.
    // Правило падения не должно спутать это с падением: отличает их именно
    // резкость перехода, а не конечное горизонтальное положение.
    core::Track track(1, 0.0);
    double time = 0.0;
    double tilt = 2.0;

    for (; time <= 0.8; time += 0.4)
        track.add(time, makePose(QPointF(600, 400), 100, tilt));

    // Плавно ложится: около 9° за 0,4 с — 22°/с, вдвое меньше порога падения.
    for (int step = 0; step < 9; ++step) {
        time += 0.4;
        tilt = qMin(85.0, tilt + 9.0);
        track.add(time, makePose(QPointF(600, 400), 100, tilt));
    }

    // И лежит ещё немного — если бы правило спутало это с падением, тревога
    // была бы уже видна.
    for (; time <= 8.0; time += 0.4)
        track.add(time, makePose(QPointF(600, 400), 100, 85));

    QCOMPARE(verdictFor(analyseTrack(track), core::Situation::Fall).level,
             core::Level::Normal);
}

void CoreTests::worstVerdictWinsBySeverity()
{
    core::Verdict attention;
    attention.situation = core::Situation::Unsteady;
    attention.level = core::Level::Attention;
    attention.heldSeconds = 30.0;

    core::Verdict alarm;
    alarm.situation = core::Situation::Unconscious;
    alarm.level = core::Level::Alarm;
    alarm.heldSeconds = 2.0;

    // При сомнении выбирается худший исход — требование заказчика. Долгое
    // «внимание» не должно заслонять короткую тревогу.
    const core::Verdict worst = core::SituationAnalyzer::worst({attention, alarm});
    QCOMPARE(worst.situation, core::Situation::Unconscious);
    QCOMPARE(worst.level, core::Level::Alarm);

    QCOMPARE(core::SituationAnalyzer::worst({}).level, core::Level::Normal);
}

// ======================================================= настройки и журнал

void CoreTests::settingsSurviveSaveAndLoad()
{
    const QString path = m_dir.filePath(QStringLiteral("settings.json"));

    core::Settings settings;
    settings.alarmVolume = 0.75;
    settings.alarmRepeatSeconds = 5;
    settings.escalateAfterSeconds = 45;
    settings.soundEnabled = false;
    settings.autoAlarm = false;
    settings.searchConfidence = 0.42;
    settings.thresholds.unconsciousStillAlarm = 7.5;
    settings.thresholds.childHeadToTorso = 0.36;
    settings.displayFpsCap = 60;
    QVERIFY(settings.save(path));

    const core::Settings loaded = core::Settings::load(path);
    QCOMPARE(loaded.alarmVolume, 0.75);
    QCOMPARE(loaded.alarmRepeatSeconds, 5);
    QCOMPARE(loaded.escalateAfterSeconds, 45);
    QCOMPARE(loaded.soundEnabled, false);
    QCOMPARE(loaded.autoAlarm, false);
    QCOMPARE(loaded.searchConfidence, 0.42);
    QCOMPARE(loaded.thresholds.unconsciousStillAlarm, 7.5);
    QCOMPARE(loaded.thresholds.childHeadToTorso, 0.36);
    QCOMPARE(loaded.displayFpsCap, 60);
}

void CoreTests::invalidFpsCapFallsBackToThirty()
{
    // Файл настроек мог прийти испорченным или от будущей версии программы
    // с другим набором допустимых значений. Показ не должен остаться вовсе
    // без предела частоты — падаем на разумное умолчание.
    const QString path = m_dir.filePath(QStringLiteral("settings_bad_fps.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArrayLiteral(
        "{\"видео\": {\"предел_кадров_в_секунду\": 17}}"));
    file.close();

    const core::Settings loaded = core::Settings::load(path);
    QCOMPARE(loaded.displayFpsCap, 30);
}

void CoreTests::eventLogWritesReadableFile()
{
    core::EventLog log;
    const QString text = QStringLiteral("Лежит без движения 12 с — проверка журнала");
    log.write(core::EventLog::Kind::Alarm, text, 2);

    QCOMPARE(log.recent().size(), 1);
    QCOMPARE(log.recent().first().text, text);
    QCOMPARE(log.recent().first().panelId, 2);

    QFile file(log.currentFilePath());
    QVERIFY2(file.open(QIODevice::ReadOnly), "журнал не открылся на чтение");
    const QString contents = QString::fromUtf8(file.readAll());

    // Русский текст обязан читаться из файла как русский. Кракозябры вместо
    // букв — не косметика: журнал заводился ровно для того, чтобы разбирать
    // происшествия задним числом, а нечитаемый журнал бесполезен.
    QVERIFY2(contents.contains(text), "в журнале не нашлось записанной строки");
    QVERIFY(contents.contains(QStringLiteral("ТРЕВОГА")));
}

// ---------------------------------------------------------------- IP-камеры
//
//  Всё, что здесь проверяется, куплено разбором живого случая: смартфон в
//  роли камеры (приложение «IP Webcam») отдаёт поток по адресу
//  http://192.168.1.126:8080/video и rtsp://192.168.1.126:8080/h264_ulaw.sdp,
//  а программа его не находила и подключить не давала. Причин было три, и все
//  три — допущения в нашем коде: «протокол всегда rtsp», «порт всегда 554»,
//  «шаблона под смартфон нет». Проверки ниже держат каждое из них закрытым.

void CoreTests::streamUrlKeepsSchemeAndPort()
{
    // Схема больше не навязывается. Пока она жёстко была rtsp, ввести адрес
    // MJPEG было нельзя вовсе.
    QCOMPARE(core::CameraDiscovery::buildUrl(
                 QStringLiteral("http"), QStringLiteral("192.168.1.126"), 8080,
                 QString(), QString(), QStringLiteral("/video")),
             QStringLiteral("http://192.168.1.126:8080/video"));

    // Нестандартный порт при rtsp тоже обязан доживать до адреса: смартфон
    // отдаёт RTSP на 8080, а не на 554.
    QCOMPARE(core::CameraDiscovery::buildUrl(
                 QStringLiteral("rtsp"), QStringLiteral("192.168.1.126"), 8080,
                 QString(), QString(), QStringLiteral("/h264_ulaw.sdp")),
             QStringLiteral("rtsp://192.168.1.126:8080/h264_ulaw.sdp"));

    // Логин с паролем входят в адрес — так требует RTSP.
    QCOMPARE(core::CameraDiscovery::buildUrl(
                 QStringLiteral("rtsp"), QStringLiteral("192.168.1.64"), 554,
                 QStringLiteral("admin"), QStringLiteral("secret"),
                 QStringLiteral("/Streaming/Channels/102")),
             QStringLiteral("rtsp://admin:secret@192.168.1.64/Streaming/Channels/102"));

    // Параметры запроса не должны кодироваться: камера Dahua такой адрес не
    // поймёт, а ошибка вылезет только пустой панелью.
    QVERIFY(core::CameraDiscovery::buildUrl(
                QStringLiteral("rtsp"), QStringLiteral("192.168.1.10"), 554,
                QString(), QString(),
                QStringLiteral("/cam/realmonitor?channel=1&subtype=1"))
                .endsWith(QStringLiteral("?channel=1&subtype=1")));
}

void CoreTests::standardPortIsOmittedFromUrl()
{
    QCOMPARE(core::CameraDiscovery::defaultPort(QStringLiteral("rtsp")), 554);
    QCOMPARE(core::CameraDiscovery::defaultPort(QStringLiteral("http")), 80);

    // Стандартный порт схемы в адресе не пишется — короче и привычнее глазу.
    QCOMPARE(core::CameraDiscovery::buildUrl(
                 QStringLiteral("rtsp"), QStringLiteral("192.168.1.64"), 554,
                 QString(), QString(), QStringLiteral("/stream2")),
             QStringLiteral("rtsp://192.168.1.64/stream2"));
    QCOMPARE(core::CameraDiscovery::buildUrl(
                 QStringLiteral("http"), QStringLiteral("192.168.1.64"), 80,
                 QString(), QString(), QStringLiteral("/video")),
             QStringLiteral("http://192.168.1.64/video"));

    // А чужой стандартный — пишется: 80 при rtsp это не умолчание, а выбор.
    QCOMPARE(core::CameraDiscovery::buildUrl(
                 QStringLiteral("rtsp"), QStringLiteral("192.168.1.64"), 80,
                 QString(), QString(), QStringLiteral("/stream2")),
             QStringLiteral("rtsp://192.168.1.64:80/stream2"));
}

void CoreTests::wholeUrlIsParsedIntoParts()
{
    core::CameraDiscovery::UrlParts parts;

    QVERIFY(core::CameraDiscovery::parseUrl(
        QStringLiteral("http://192.168.1.126:8080/video"), &parts));
    QCOMPARE(parts.scheme, QStringLiteral("http"));
    QCOMPARE(parts.host, QStringLiteral("192.168.1.126"));
    QCOMPARE(parts.port, 8080);
    QCOMPARE(parts.path, QStringLiteral("/video"));

    // Порт не указан — подставляется стандартный для схемы, а не ноль: иначе
    // собранный обратно адрес потерял бы его молча.
    QVERIFY(core::CameraDiscovery::parseUrl(
        QStringLiteral("rtsp://admin:secret@192.168.1.64/cam?channel=1"), &parts));
    QCOMPARE(parts.scheme, QStringLiteral("rtsp"));
    QCOMPARE(parts.port, 554);
    QCOMPARE(parts.user, QStringLiteral("admin"));
    QCOMPARE(parts.password, QStringLiteral("secret"));
    QCOMPARE(parts.path, QStringLiteral("/cam?channel=1"));

    // Пробелы по краям — обычное дело при вставке из переписки.
    QVERIFY(core::CameraDiscovery::parseUrl(
        QStringLiteral("  http://192.168.1.126:8080/shot.jpg  "), &parts));
    QCOMPARE(parts.host, QStringLiteral("192.168.1.126"));

    // А голый адрес узла ссылкой не считается: он и так лежит в своём поле, и
    // разбирать его — значит стереть остальные.
    QVERIFY(!core::CameraDiscovery::parseUrl(QStringLiteral("192.168.1.126"), nullptr));
    QVERIFY(!core::CameraDiscovery::parseUrl(QString(), nullptr));
    QVERIFY(!core::CameraDiscovery::parseUrl(QStringLiteral("чаша, вид с востока"),
                                             nullptr));
}

void CoreTests::smartphoneTemplateMatchesLiveCamera()
{
    const auto templates = core::CameraDiscovery::urlTemplates();

    // Шаблон под смартфон обязан быть: без него оператор должен был знать путь
    // /h264_ulaw.sdp наизусть.
    int found = 0;
    for (const auto &item : templates) {
        if (!item.vendor.contains(QStringLiteral("IP Webcam")))
            continue;
        ++found;
        QCOMPARE(item.port, 8080);
        QVERIFY(!item.scheme.isEmpty());
        QVERIFY(!item.path.isEmpty());
    }
    QCOMPARE(found, 2);   // H.264 и MJPEG

    // И оба должны складываться ровно в те адреса, что проверены на живом
    // смартфоне: 24,9 кадр/с по MJPEG и 22,6 кадр/с по RTSP.
    QStringList built;
    for (const auto &item : templates) {
        if (!item.vendor.contains(QStringLiteral("IP Webcam")))
            continue;
        built << core::CameraDiscovery::buildUrl(
            item.scheme, QStringLiteral("192.168.1.126"), item.port,
            QString(), QString(), item.path);
    }
    QVERIFY(built.contains(QStringLiteral("rtsp://192.168.1.126:8080/h264_ulaw.sdp")));
    QVERIFY(built.contains(QStringLiteral("http://192.168.1.126:8080/video")));
}

void CoreTests::discoveryLooksBeyondPort554()
{
    // Поиск смотрел только порт 554, и камера на 8080 не находилась никогда.
    // Если кто-то решит «ускорить поиск», сузив список обратно, — упадёт здесь.
    QVERIFY(core::CameraDiscovery::rtspPorts().contains(554));
    QVERIFY(core::CameraDiscovery::httpPorts().contains(8080));

    // Порт 80 в опрос не входит намеренно: его слушает всё подряд, и список
    // находок превратился бы в перечень роутеров и принтеров.
    QVERIFY(!core::CameraDiscovery::httpPorts().contains(80));

    // Найденная камера показывается вместе с портом — иначе оператор не
    // поймёт, откуда взялось 8080 и почему адрес не открывается на 554.
    core::DiscoveredCamera camera;
    camera.address = QStringLiteral("192.168.1.126");
    camera.description = QStringLiteral("Смартфон (IP Webcam)");
    camera.httpPorts = {8080};
    QVERIFY(camera.title().contains(QStringLiteral("192.168.1.126")));
    QVERIFY(camera.title().contains(QStringLiteral("8080")));
    QVERIFY(camera.title().contains(QStringLiteral("IP Webcam")));
}

// --------------------------------------------------- связь с телефоном
//
//  Проверяется ровно то, на что опирается приложение для Android: строки JSON,
//  разделённые переводом строки. Если здесь что-то поедет, приложение замолчит
//  молча — оно и есть тот случай, когда «работает» и «не работает» снаружи
//  выглядят одинаково.

namespace {

/// Дождаться от сокета строки, начинающейся с нужного вида сообщения.
QJsonObject waitForMessage(QTcpSocket *socket, const QString &type, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    QByteArray buffer;

    while (timer.elapsed() < timeoutMs) {
        // БЕЗ ЭТОГО СЕРВЕР МОЛЧИТ. Ожидание на сокете крутит только его
        // собственные события; QTcpServer принимает подключение в общем
        // событийном цикле, а в консольной проверке он сам не крутится.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        socket->waitForReadyRead(50);
        buffer += socket->readAll();

        int newline = buffer.indexOf('\n');
        while (newline >= 0) {
            const QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1);

            const QJsonObject object = QJsonDocument::fromJson(line).object();
            if (object.value(QStringLiteral("type")).toString() == type)
                return object;

            newline = buffer.indexOf('\n');
        }
    }
    return QJsonObject();
}

/// Порт для проверок. Нарочно не 8765: рабочая программа может быть запущена
/// на том же компьютере, и занимать её порт проверками — верный способ
/// получить необъяснимо падающий тест.
constexpr quint16 kTestPort = 18765;

} // namespace

void CoreTests::phoneLearnsPanelsOnConnect()
{
    core::EventServer server;
    QVERIFY2(server.start(kTestPort), qPrintable(server.lastError()));

    QVector<core::PanelInfo> panels;
    core::PanelInfo panel;
    panel.panelId = 0;
    panel.zone = QStringLiteral("Северо-западный угол");
    panel.streamUrl = QStringLiteral("rtsp://192.168.1.126:8080/h264_ulaw.sdp");
    panel.peopleCount = 3;
    panel.level = core::Level::Attention;
    panels.append(panel);
    server.setPanels(panels);

    QTcpSocket phone;
    phone.connectToHost(QHostAddress::LocalHost, kTestPort);
    QVERIFY2(phone.waitForConnected(3000), "телефон не подключился к серверу");
    QCoreApplication::processEvents();

    // ТЕЛЕФОН УЗНАЁТ ОБСТАНОВКУ СРАЗУ, а не с первого события. Иначе
    // подключившийся посреди тревоги увидел бы спокойный экран.
    const QJsonObject message = waitForMessage(&phone, QStringLiteral("panels"));
    QVERIFY2(!message.isEmpty(), "список панелей не пришёл");

    const QJsonArray items = message.value(QStringLiteral("items")).toArray();
    QCOMPARE(items.size(), 1);

    const QJsonObject first = items.first().toObject();
    QCOMPARE(first.value(QStringLiteral("id")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("zone")).toString(),
             QStringLiteral("Северо-западный угол"));
    QCOMPARE(first.value(QStringLiteral("people")).toInt(), 3);

    // Уровень обязан ехать вместе с панелью. Пока он не заполнялся, телефон
    // считал спокойными даже те панели, где шла тревога.
    QCOMPARE(first.value(QStringLiteral("level")).toInt(), int(core::Level::Attention));

    // Адрес потока телефон берёт отсюда: видео через компьютер не идёт.
    QCOMPARE(first.value(QStringLiteral("stream")).toString(),
             QStringLiteral("rtsp://192.168.1.126:8080/h264_ulaw.sdp"));
}

void CoreTests::phoneReceivesAlarmWithReadableRussian()
{
    core::EventServer server;
    QVERIFY2(server.start(kTestPort), qPrintable(server.lastError()));

    QTcpSocket phone;
    phone.connectToHost(QHostAddress::LocalHost, kTestPort);
    QVERIFY2(phone.waitForConnected(3000), "телефон не подключился к серверу");
    QCoreApplication::processEvents();
    QVERIFY2(!waitForMessage(&phone, QStringLiteral("hello")).isEmpty(),
             "сервер не представился");

    server.publishEvent(QStringLiteral("alarm"), 2,
                        QStringLiteral("Юго-западный угол"),
                        QStringLiteral("человек без сознания в воде"),
                        QStringLiteral("не двигается 12 с"), 2);

    const QJsonObject event = waitForMessage(&phone, QStringLiteral("event"));
    QVERIFY2(!event.isEmpty(), "тревога до телефона не дошла");

    QCOMPARE(event.value(QStringLiteral("kind")).toString(), QStringLiteral("alarm"));
    QCOMPARE(event.value(QStringLiteral("panel")).toInt(), 2);
    QCOMPARE(event.value(QStringLiteral("level")).toInt(), 2);

    // Русский текст обязан доехать русским. Кракозябры здесь означают, что
    // спасатель прочтёт на телефоне бессмыслицу вместо того, что случилось.
    QCOMPARE(event.value(QStringLiteral("zone")).toString(),
             QStringLiteral("Юго-западный угол"));
    QCOMPARE(event.value(QStringLiteral("title")).toString(),
             QStringLiteral("человек без сознания в воде"));
    QCOMPARE(event.value(QStringLiteral("details")).toString(),
             QStringLiteral("не двигается 12 с"));

    // Время нужно телефону, чтобы показать, насколько сигнал свежий.
    QVERIFY(!event.value(QStringLiteral("time")).toString().isEmpty());
}

void CoreTests::verdictSurvivesBetweenPoseRuns()
{
    // Позы считаются реже, чем ищутся люди: на кадре с шестью людьми поиск
    // занимает 87 мс, а позы — 117 мс, при том что самый короткий порог правил
    // равен двум секундам. Между разборами поз рамка обязана сохранять цвет и
    // подпись, иначе тревожная рамка мигала бы красным через кадр — а мигание
    // в этой системе значит совсем другое.

    QVector<core::PersonView> previous;

    core::PersonView danger;
    danger.box = QRect(100, 100, 60, 160);
    danger.trackId = 7;
    danger.level = core::Level::Alarm;
    danger.label = QStringLiteral("лежит без движения");
    previous.append(danger);

    core::PersonView calm;
    calm.box = QRect(400, 100, 60, 160);
    calm.trackId = 8;
    calm.level = core::Level::Normal;
    previous.append(calm);

    // Человек сместился на несколько точек — это он же.
    const core::PersonView *same =
        core::matchPrevious(previous, QRect(108, 104, 60, 160));
    QVERIFY2(same != nullptr, "человек потерялся при малом смещении");
    QCOMPARE(same->trackId, 7);
    QCOMPARE(same->level, core::Level::Alarm);
    QCOMPARE(same->label, QStringLiteral("лежит без движения"));

    // Новый человек в стороне не должен унаследовать чужую тревогу.
    QVERIFY2(core::matchPrevious(previous, QRect(700, 100, 60, 160)) == nullptr,
             "вердикт достался постороннему");

    // Слабое перекрытие — это сосед, стоящий рядом, а не тот же человек.
    QVERIFY2(core::matchPrevious(previous, QRect(150, 100, 60, 160)) == nullptr,
             "вердикт взят у соседа по слабому перекрытию");

    // Пустая история никого не находит и не падает.
    QVERIFY(core::matchPrevious({}, QRect(100, 100, 60, 160)) == nullptr);
}

void CoreTests::serverAnnouncesRealVersion()
{
    // Версия жила в четырёх местах сразу и к выпуску 1.1.0 разошлась:
    // программа звала себя 0.4.0, установщик собирался как 1.0.0, а телефону
    // сервер представлялся как 1.1.0. Теперь строка одна, и эта проверка
    // держит её одной: телефон должен слышать ровно то, чем программа
    // является.
    core::EventServer server;
    QVERIFY2(server.start(kTestPort), qPrintable(server.lastError()));

    QTcpSocket phone;
    phone.connectToHost(QHostAddress::LocalHost, kTestPort);
    QVERIFY2(phone.waitForConnected(3000), "телефон не подключился к серверу");
    QCoreApplication::processEvents();

    const QJsonObject hello = waitForMessage(&phone, QStringLiteral("hello"));
    QVERIFY2(!hello.isEmpty(), "сервер не представился");

    QCOMPARE(hello.value(QStringLiteral("app")).toString(),
             QStringLiteral("PoolSafety"));
    QCOMPARE(hello.value(QStringLiteral("version")).toString(),
             QString::fromLatin1(core::kAppVersion));

    // Версия должна быть похожа на версию, а не на заглушку.
    static const QRegularExpression shape(QStringLiteral(R"(^\d+\.\d+\.\d+$)"));
    QVERIFY2(shape.match(QString::fromLatin1(core::kAppVersion)).hasMatch(),
             "версия записана не в виде «главная.дополнительная.правка»");
}

void CoreTests::phoneCanAcknowledgeAlarm()
{
    // СПАСАТЕЛЬ У ВОДЫ ДОЛЖЕН ГАСИТЬ СИРЕНУ САМ. Пока канал был односторонним,
    // выключить тревогу мог только тот, кто стоит у компьютера, — а он в этот
    // момент, скорее всего, бежит к бассейну.
    core::EventServer server;
    QVERIFY2(server.start(kTestPort), qPrintable(server.lastError()));

    qRegisterMetaType<core::EventServer::Command>("core::EventServer::Command");
    QSignalSpy spy(&server, &core::EventServer::commandReceived);

    QTcpSocket phone;
    phone.connectToHost(QHostAddress::LocalHost, kTestPort);
    QVERIFY2(phone.waitForConnected(3000), "телефон не подключился");
    QCoreApplication::processEvents();

    phone.write(R"({"type":"command","action":"acknowledge"})" "\n");
    phone.write(R"({"type":"command","action":"false_alarm"})" "\n");
    phone.flush();

    QElapsedTimer timer;
    timer.start();
    while (spy.count() < 2 && timer.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).at(0).value<core::EventServer::Command>(),
             core::EventServer::Command::Acknowledge);
    QCOMPARE(spy.at(1).at(0).value<core::EventServer::Command>(),
             core::EventServer::Command::FalseAlarm);

    // Мусор в канале не должен ничего значить: к порту может подключиться
    // что угодно, и принимать его за решение по тревоге нельзя.
    phone.write("не json\n");
    phone.write(R"({"type":"event"})" "\n");
    phone.write(R"({"type":"command"})" "\n");
    phone.flush();
    timer.restart();
    while (timer.elapsed() < 500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(spy.count(), 2);
}

void CoreTests::alarmStateReachesPhone()
{
    // Тревогу погасили на посту — телефон обязан узнать сразу, а не при
    // следующем событии. Иначе на телефоне она висит красным, и спасатель
    // думает, что беда продолжается.
    core::EventServer server;
    QVERIFY2(server.start(kTestPort), qPrintable(server.lastError()));

    QTcpSocket phone;
    phone.connectToHost(QHostAddress::LocalHost, kTestPort);
    QVERIFY2(phone.waitForConnected(3000), "телефон не подключился");
    QCoreApplication::processEvents();
    QVERIFY(!waitForMessage(&phone, QStringLiteral("hello")).isEmpty());

    server.publishAlarmState(QStringLiteral("idle"), 2);

    const QJsonObject message = waitForMessage(&phone, QStringLiteral("alarm_state"));
    QVERIFY2(!message.isEmpty(), "состояние тревоги до телефона не дошло");
    QCOMPARE(message.value(QStringLiteral("state")).toString(), QStringLiteral("idle"));
    QCOMPARE(message.value(QStringLiteral("panel")).toInt(), 2);
}

void CoreTests::zoneCountTakesTheBusiestCamera()
{
    core::AlarmController alarm;

    // Один человек ходит вдоль бассейна, и его видят все четыре камеры.
    // В зоне он по-прежнему один.
    alarm.reportPresence(0, 1);
    alarm.reportPresence(1, 1);
    alarm.reportPresence(2, 1);
    alarm.reportPresence(3, 1);
    QCOMPARE(alarm.peopleInZone(), 1);

    // На одной камере видно троих — значит в зоне не меньше троих.
    alarm.reportPresence(2, 3);
    QCOMPARE(alarm.peopleInZone(), 3);

    // Камера опустела — счёт берёт следующую по многолюдности.
    alarm.reportPresence(2, 0);
    QCOMPARE(alarm.peopleInZone(), 1);

    // Все пусты — зона пуста.
    alarm.reportPresence(0, 0);
    alarm.reportPresence(1, 0);
    alarm.reportPresence(3, 0);
    QCOMPARE(alarm.peopleInZone(), 0);
}

void CoreTests::phoneCanCloseIncident()
{
    // «Помощь вызвана» не закрывает тревогу: между вызовом скорой и её
    // отъездом проходит время, и всё это время происшествие идёт. Закрыть его
    // — отдельное решение, и с телефона оно тоже должно быть доступно, иначе
    // спасатель у воды не может вернуть систему к наблюдению.
    core::EventServer server;
    QVERIFY2(server.start(kTestPort), qPrintable(server.lastError()));

    qRegisterMetaType<core::EventServer::Command>("core::EventServer::Command");
    QSignalSpy spy(&server, &core::EventServer::commandReceived);

    QTcpSocket phone;
    phone.connectToHost(QHostAddress::LocalHost, kTestPort);
    QVERIFY2(phone.waitForConnected(3000), "телефон не подключился");
    QCoreApplication::processEvents();

    phone.write(R"({"type":"command","action":"confirm"})" "\n");
    phone.write(R"({"type":"command","action":"close"})" "\n");
    phone.flush();

    QElapsedTimer timer;
    timer.start();
    while (spy.count() < 2 && timer.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).at(0).value<core::EventServer::Command>(),
             core::EventServer::Command::Confirm);
    QCOMPARE(spy.at(1).at(0).value<core::EventServer::Command>(),
             core::EventServer::Command::Close);
}

void CoreTests::lateJoinerLearnsAlarmIsRunning()
{
    // СПАСАТЕЛЬ МОГ ОТКРЫТЬ ПРИЛОЖЕНИЕ УЖЕ ПОСРЕДИ ПРОИСШЕСТВИЯ — по звонку,
    // по крику, просто зайдя в помещение. Состояние тревоги рассылается при
    // изменении, и подключившийся позже о нём бы не узнал: на телефоне
    // спокойный экран, а на посту в это время воет сирена.
    core::EventServer server;
    QVERIFY2(server.start(kTestPort), qPrintable(server.lastError()));

    // Тревога объявлена ДО того, как телефон подключился.
    server.publishAlarmState(QStringLiteral("raised"), 1);

    QTcpSocket phone;
    phone.connectToHost(QHostAddress::LocalHost, kTestPort);
    QVERIFY2(phone.waitForConnected(3000), "телефон не подключился");
    QCoreApplication::processEvents();

    const QJsonObject message = waitForMessage(&phone, QStringLiteral("alarm_state"));
    QVERIFY2(!message.isEmpty(), "состояние тревоги при подключении не пришло");
    QCOMPARE(message.value(QStringLiteral("state")).toString(), QStringLiteral("raised"));
    QCOMPARE(message.value(QStringLiteral("panel")).toInt(), 1);
}

// ============================================ постоянное окружение
//
//  ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ. Карта копит, какие места кадра никогда не меняются.
//  Кадр в проверках устроен просто: левая половина неподвижна, правая мигает.
//  После обучения модель обязана видеть эту разницу — и обязана молчать, пока
//  не обучилась.

namespace {

/// Кадр, где левая половина всегда одинакова, а правая меняется.
QImage twoHalves(int step)
{
    QImage frame(128, 72, QImage::Format_RGB32);
    frame.fill(QColor(40, 40, 40));

    QPainter painter(&frame);
    // Правая половина мигает: так выглядит место, где всё время ходят.
    const int shade = (step % 2 == 0) ? 20 : 230;
    painter.fillRect(QRect(64, 0, 64, 72), QColor(shade, shade, shade));
    painter.end();
    return frame;
}

} // namespace

void CoreTests::backgroundStaysSilentUntilTrained()
{
    core::BackgroundModel model;

    // Пока модель не обучена, она не имеет права ни на какое суждение:
    // «постоянным окружением» за первые минуты окажется и человек, прилёгший
    // на лежак.
    for (int i = 0; i < 200; ++i)
        model.observe(twoHalves(i));

    QVERIFY(!model.isReady());
    QCOMPARE(model.stillnessAt(QPoint(10, 10), QSize(128, 72)), 0.0);
    QVERIFY(model.status().contains(QStringLiteral("обучение")));
}

void CoreTests::backgroundTellsStillFromBusy()
{
    core::BackgroundModel model;

    // Обучаем с запасом: модель считается готовой не раньше заданного числа
    // разборов, и это намеренно час непрерывной работы.
    const int enough = core::BackgroundModel::kMinSamples + 500;
    for (int i = 0; i < enough; ++i)
        model.observe(twoHalves(i));

    QVERIFY2(model.isReady(), qPrintable(model.status()));

    const QSize size(128, 72);
    const double still = model.stillnessAt(QPoint(20, 36), size);   // слева
    const double busy = model.stillnessAt(QPoint(100, 36), size);   // справа

    QVERIFY2(still > 0.8, "неподвижная часть кадра не опознана");
    QVERIFY2(busy < 0.2, "мигающая часть кадра принята за неподвижную");

    // По области — то же самое: рамка человека попадает в несколько клеток.
    QVERIFY(model.stillnessAt(QRect(4, 10, 40, 50), size) > 0.8);
    QVERIFY(model.stillnessAt(QRect(70, 10, 50, 50), size) < 0.2);
}

void CoreTests::backgroundSurvivesSaveAndLoad()
{
    const QString path = m_dir.filePath(QStringLiteral("окружение.json"));

    core::BackgroundModel first;
    const int enough = core::BackgroundModel::kMinSamples + 500;
    for (int i = 0; i < enough; ++i)
        first.observe(twoHalves(i));
    QVERIFY(first.save(path));

    core::BackgroundModel second;
    QVERIFY2(second.load(path), "карта не прочиталась");
    QCOMPARE(second.samples(), first.samples());

    // Накопленное знание должно пережить перезапуск программы: иначе обучение
    // начиналось бы заново каждое утро и не заканчивалось никогда.
    const QSize size(128, 72);
    QVERIFY(second.stillnessAt(QPoint(20, 36), size) > 0.8);
    QVERIFY(second.stillnessAt(QPoint(100, 36), size) < 0.2);
}

void CoreTests::backgroundRefusesForeignGrid()
{
    const QString path = m_dir.filePath(QStringLiteral("чужая.json"));

    QJsonObject root;
    root.insert(QStringLiteral("сетка_ширина"), 8);
    root.insert(QStringLiteral("сетка_высота"), 8);
    root.insert(QStringLiteral("разборов_учтено"), 999999);
    root.insert(QStringLiteral("изменчивость"), QJsonArray());

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
    file.close();

    // ЧУЖАЯ СЕТКА — ЧУЖАЯ КАРТА. Числа легли бы не на те места, и модель
    // уверенно говорила бы неправду о том, где в кадре ничего не происходит.
    core::BackgroundModel model;
    QVERIFY2(!model.load(path), "принята карта с чужой сеткой");
    QVERIFY(!model.isReady());
}

void CoreTests::rtspFallsBackToHttpForPhoneCamera()
{
    // RTSP шлёт видео отдельными пакетами UDP, а договаривается по TCP. Стоит
    // между программой и камерой оказаться включённой VPN или второму сетевому
    // адресу в той же подсети — и получается худшее: камера найдена,
    // соединение есть, кадров нет. Для смартфона путь по HTTP известен точно,
    // и переход на него спасает положение.
    QCOMPARE(core::CameraDiscovery::httpFallbackFor(
                 QStringLiteral("rtsp://192.168.1.126:8080/h264_ulaw.sdp")),
             QStringLiteral("http://192.168.1.126:8080/video"));

    // Логин и пароль, если они были, обязаны сохраниться.
    QCOMPARE(core::CameraDiscovery::httpFallbackFor(
                 QStringLiteral("rtsp://имя:пароль@192.168.1.5:8080/h264_ulaw.sdp")),
             QStringLiteral("http://имя:пароль@192.168.1.5:8080/video"));

    // Уже HTTP — заменять нечего.
    QVERIFY(core::CameraDiscovery::httpFallbackFor(
                QStringLiteral("http://192.168.1.126:8080/video")).isEmpty());

    // ДЛЯ ОБЫЧНОЙ КАМЕРЫ ЗАПАСНОГО ПУТИ НЕТ, И ВЫДУМЫВАТЬ ЕГО НЕЛЬЗЯ: у
    // каждого производителя свой путь к MJPEG, промах дал бы оператору ложную
    // надежду вместо честного «связи нет».
    QVERIFY(core::CameraDiscovery::httpFallbackFor(
                QStringLiteral("rtsp://192.168.1.64:554/Streaming/Channels/102"))
                .isEmpty());
    QVERIFY(core::CameraDiscovery::httpFallbackFor(QString()).isEmpty());
}

void CoreTests::reconnectBackoffGrowsAndCaps()
{
    // ВОТ ПРИЧИНА, ПО КОТОРОЙ ПРОГРАММА ГРУЗИЛА ПРОЦЕССОР ВХОЛОСТУЮ. Сторож
    // проверял связь каждые две секунды и, найдя молчание, пересоздавал
    // проигрыватель — на КАЖДЫЙ тик, сколько бы подряд попытка ни
    // проваливалась. На объекте с одной нестабильной камерой это дало 163
    // попытки за десять минут и около трёх с половиной ядер процессора
    // непрерывно: разумная камера с перебоями раз в минуту не отличалась от
    // полностью мёртвой.
    //
    // Пауза должна расти с числом неудач и не превышать потолок.
    QCOMPARE(core::NetworkSource::backoffMs(0), qint64(0));
    QCOMPARE(core::NetworkSource::backoffMs(1), qint64(1000));
    QCOMPARE(core::NetworkSource::backoffMs(2), qint64(2000));
    QCOMPARE(core::NetworkSource::backoffMs(5), qint64(5000));

    // Потолок — тридцать секунд, а не бесконечный рост: как только камера
    // снова станет доступна, связь обязана восстановиться быстро, а не через
    // час нарастающего ожидания.
    QCOMPARE(core::NetworkSource::backoffMs(30), core::NetworkSource::kMaxBackoffMs);
    QCOMPARE(core::NetworkSource::backoffMs(1000), core::NetworkSource::kMaxBackoffMs);

    // Отрицательное число попыток не может дать отрицательную (то есть
    // нулевую-в-обход) паузу.
    QCOMPARE(core::NetworkSource::backoffMs(-5), qint64(0));
}

void CoreTests::staleSettingsAreRewrittenAfterMigration()
{
    // РОВНО ЭТО СБИЛО С ТОЛКУ ПРИ РАЗБОРЕ ЖАЛОБЫ НА НАГРУЗКУ. Файл без поля
    // "версия_настроек" и со старым "интервал_разбора_мс": 400 выглядит так,
    // будто перевод на быстрый темп (120 мс) не сработал — а он сработал,
    // просто только в памяти этого запуска, и на диске остался прежний файл.
    // Программа честно работала быстро, а diagnostика по файлу лгала.
    const QString path = m_dir.filePath(QStringLiteral("старые_настройки.json"));

    QJsonObject detection;
    detection.insert(QStringLiteral("интервал_разбора_мс"), 400);
    QJsonObject root;
    root.insert(QStringLiteral("распознавание"), detection);
    // Поля "версия_настроек" нет вовсе — как в файле, написанном до её
    // появления.

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
    file.close();

    const core::Settings loaded = core::Settings::load(path);
    QCOMPARE(loaded.detectIntervalMs, 120);

    // Файл на диске обязан отражать то, что программа сделала, а не то, чем
    // он был написан. Перечитываем его напрямую, в обход Settings::load(),
    // чтобы проверить именно ФАЙЛ, а не только память.
    QFile check(path);
    QVERIFY(check.open(QIODevice::ReadOnly));
    const QJsonObject onDisk = QJsonDocument::fromJson(check.readAll()).object();
    QVERIFY2(onDisk.contains(QStringLiteral("версия_настроек")),
             "файл не переписан после перевода на новые значения");
    const QJsonObject detectionOnDisk =
        onDisk.value(QStringLiteral("распознавание")).toObject();
    QCOMPARE(detectionOnDisk.value(QStringLiteral("интервал_разбора_мс")).toInt(), 120);
}

void CoreTests::newDefaultModelIsFast()
{
    // Измерено (ПРОЕКТ.md, раздел 6): YOLOX-tiny находит 5,67 человека против
    // 5,73 у YOLOX-s на тех же 120 кадрах — разница на грани погрешности — и
    // при этом втрое быстрее. Для новой, ещё не настроенной установки это
    // верный выбор по умолчанию.
    const core::Settings fresh;
    QCOMPARE(fresh.detectorModel, core::PersonDetector::Model::Fast);
}

// ==================================================== приёмник MJPEG
//
//  ПРОВЕРЯЕТСЯ БЕЗ НАСТОЯЩЕЙ СЕТИ И БЕЗ НАСТОЯЩЕГО JPEG. extractLatestFrame()
//  работает с сырыми байтами и ничего не декодирует — кадром для неё
//  достаточно назвать любую последовательность байтов, лишь бы был правильно
//  оформлен заголовок Content-Length. Формат байт в байт как у настоящей
//  камеры (проверено побайтовым разбором потока с IP Webcam).

namespace {

/// Собрать одну часть MJPEG-потока с указанным телом.
QByteArray mjpegPart(const QByteArray &body,
                     const QByteArray &boundary = QByteArrayLiteral("bnd"))
{
    QByteArray part = "\r\n--" + boundary + "\r\n";
    part += "Content-Type: image/jpeg\r\n";
    part += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    part += "\r\n";
    part += body;
    return part;
}

} // namespace

void CoreTests::mjpegExtractsSingleCompleteFrame()
{
    QByteArray buffer = mjpegPart(QByteArrayLiteral("кадр-1"));
    const QByteArray frame = core::MjpegWorker::extractLatestFrame(buffer);

    QCOMPARE(frame, QByteArrayLiteral("кадр-1"));
    QVERIFY2(buffer.isEmpty(), "разобранные байты должны уйти из буфера");
}

void CoreTests::mjpegWaitsForIncompletePayload()
{
    // Камера ещё не дослала тело кадра целиком — заявленная в заголовке
    // длина больше того, что реально пришло. Разбирать нечего, и буфер
    // трогать нельзя: следующий readyRead() довесит недостающее.
    QByteArray full = mjpegPart(QByteArrayLiteral("0123456789"));
    QByteArray partial = full.left(full.size() - 4);
    const QByteArray original = partial;

    const QByteArray frame = core::MjpegWorker::extractLatestFrame(partial);

    QVERIFY2(frame.isEmpty(), "неполный кадр не должен считаться готовым");
    QCOMPARE(partial, original);
}

void CoreTests::mjpegSkipsStaleFramesUnderBacklog()
{
    // СМЫСЛ ВСЕГО КЛАССА. Сеть успела прислать три кадра быстрее, чем мы их
    // разбираем — ровно то, что раньше копилось в очереди QMediaPlayer и
    // превращалось в растущее отставание показа от жизни. Разобрать нужно
    // все три (иначе разбор буфера сам начнёт отставать от сети), но вернуть
    // — только третий, самый свежий. Первые два не должны попасть даже в
    // промежуточный результат: их не декодируют.
    QByteArray buffer = mjpegPart(QByteArrayLiteral("старый"))
                      + mjpegPart(QByteArrayLiteral("средний"))
                      + mjpegPart(QByteArrayLiteral("свежий"));

    const QByteArray frame = core::MjpegWorker::extractLatestFrame(buffer);

    QCOMPARE(frame, QByteArrayLiteral("свежий"));
    QVERIFY2(buffer.isEmpty(), "все три кадра должны быть разобраны и убраны из буфера");
}

void CoreTests::mjpegRecoversFromGarbageBeforeHeader()
{
    // Число после "Content-Length:" оказалось не числом — совпадение
    // случайное или данные повреждены. Разбор не должен встать намертво на
    // этом месте: отступаем на один байт и ищем следующее вхождение.
    QByteArray buffer = QByteArrayLiteral("мусорContent-Length: не-число\r\n\r\n")
                      + mjpegPart(QByteArrayLiteral("настоящий"));

    const QByteArray frame = core::MjpegWorker::extractLatestFrame(buffer);

    QCOMPARE(frame, QByteArrayLiteral("настоящий"));
}

void CoreTests::mjpegAcceptsLowercaseHeader()
{
    // Не все серверы пишут заголовки в Title-Case — принимаем и строчные.
    QByteArray buffer = QByteArrayLiteral("\r\n--bnd\r\ncontent-length: 5\r\n\r\nHELLO");

    const QByteArray frame = core::MjpegWorker::extractLatestFrame(buffer);

    QCOMPARE(frame, QByteArrayLiteral("HELLO"));
}

void CoreTests::mjpegReadsRawJpegWithoutAnyHeaders()
{
    // Часть недорогих и самодельных камер (ESP32-CAM и подобные) не умеет
    // multipart вовсе — просто шлёт один JPEG за другим, без Content-Length,
    // без границы (boundary), без заголовков. Разбор обязан справиться и с
    // этим — по одним лишь маркерам JPEG (SOI FF D8 / EOI FF D9).
    QByteArray buffer = QByteArrayLiteral("\xFF\xD8") + QByteArrayLiteral("кадр") + QByteArrayLiteral("\xFF\xD9");

    const QByteArray frame = core::MjpegWorker::extractLatestFrame(buffer);

    QCOMPARE(frame, QByteArrayLiteral("\xFF\xD8кадр\xFF\xD9"));
    QVERIFY2(buffer.isEmpty(), "разобранный кадр должен уйти из буфера");
}

void CoreTests::mjpegSkipsStaleRawFramesUnderBacklog()
{
    // Тот же смысл, что и у mjpegSkipsStaleFramesUnderBacklog, но для камеры
    // без заголовков вовсе: сеть прислала три кадра быстрее, чем мы успели
    // разобрать, — разобрать нужно все три, а вернуть только последний.
    const auto raw = [](const QByteArray &body) {
        return QByteArrayLiteral("\xFF\xD8") + body + QByteArrayLiteral("\xFF\xD9");
    };
    QByteArray buffer = raw(QByteArrayLiteral("старый"))
                      + raw(QByteArrayLiteral("средний"))
                      + raw(QByteArrayLiteral("свежий"));

    const QByteArray frame = core::MjpegWorker::extractLatestFrame(buffer);

    QCOMPARE(frame, raw(QByteArrayLiteral("свежий")));
    QVERIFY2(buffer.isEmpty(), "все три кадра должны быть разобраны и убраны из буфера");
}

void CoreTests::mjpegWaitsForIncompleteRawFrame()
{
    // Есть начало кадра (SOI), а конца (EOI) ещё нет — сеть недодала хвост.
    // Буфер трогать нельзя: следующий readyRead() довезёт остаток.
    QByteArray buffer = QByteArrayLiteral("\xFF\xD8") + QByteArrayLiteral("недописанный кадр");
    const QByteArray original = buffer;

    const QByteArray frame = core::MjpegWorker::extractLatestFrame(buffer);

    QVERIFY2(frame.isEmpty(), "неполный кадр без EOI не должен считаться готовым");
    QCOMPARE(buffer, original);
}

QTEST_MAIN(CoreTests)
#include "test_core.moc"
