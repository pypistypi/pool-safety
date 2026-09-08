#include "ui/SettingsDialog.h"

#include "core/AlarmTypes.h"
#include "core/PersonDetector.h"

#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QFileDialog>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QDateTime>
#include <QScrollArea>
#include <QScreen>
#include <QGuiApplication>

namespace ui {

namespace {

/// Подпись-пояснение под полем. Сплошь и рядом решает больше, чем само поле:
/// администратор объекта видит не только «что вписать», но и зачем.
QLabel *hint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: #939fb0;"));
    return label;
}

/// Обернуть страницу в прокрутку.
///
/// ЗАЧЕМ. Разделов стало много, и на ноутбучном экране окно вырастало выше
/// рабочего стола — кнопки «Сохранить» и «Отмена» уходили под панель задач, и
/// сохранить настройки было нечем. Прокрутка внутри вкладки оставляет кнопки
/// на месте при любом размере экрана.
QWidget *scrollable(QWidget *page)
{
    auto *area = new QScrollArea(page->parentWidget());
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    return area;
}

} // namespace

SettingsDialog::SettingsDialog(const core::SiteInfo &site, const core::Settings &settings,
                               QWidget *parent)
    : QDialog(parent)
    , m_site(site)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("Настройки поста наблюдения"));
    setMinimumSize(780, 480);

    // Окно подгоняется под экран, а не под содержимое: разделов много, и по
    // сумме подписей оно вырастало выше рабочего стола — кнопки «Сохранить» и
    // «Отмена» оказывались под панелью задач.
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        resize(qMin(900, int(available.width() * 0.7)),
               qMin(760, int(available.height() * 0.82)));
    } else {
        resize(880, 700);
    }

    auto *root = new QVBoxLayout(this);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(scrollable(buildReactionsTab()), QStringLiteral("Отклики"));
    tabs->addTab(scrollable(buildSiteTab()), QStringLiteral("Объект"));
    tabs->addTab(buildContactsTab(), QStringLiteral("Телефоны"));
    tabs->addTab(scrollable(buildAlarmTab()), QStringLiteral("Тревога и звук"));
    tabs->addTab(scrollable(buildDetectionTab()), QStringLiteral("Распознавание"));
    root->addWidget(tabs, 1);

    auto *buttons = new QDialogButtonBox(this);

    auto *preview = buttons->addButton(QStringLiteral("Проверить: что скажет оператор"),
                                       QDialogButtonBox::ActionRole);
    connect(preview, &QPushButton::clicked, this, &SettingsDialog::previewCallBrief);

    buttons->addButton(QStringLiteral("Сохранить"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

// ----------------------------------------------------------------- отклики

QWidget *SettingsDialog::buildReactionsTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    layout->addWidget(hint(QStringLiteral(
        "Здесь выключается всё, на что программа откликается сама. Видео, "
        "счётчик людей, журнал и тревога вручную работают в любом случае — "
        "выключаются только автоматические отклики.\n\n"
        "Это нужно на время настройки: пока пороги не подогнаны под вашу "
        "сцену, часть правил будет шуметь. Оператор, которому нечем унять "
        "программу, просто закроет её — и объект останется вовсе без "
        "наблюдения."), page));

    m_reactionsEnabled = new QCheckBox(
        QStringLiteral("Откликаться на события (главный выключатель)"), page);
    m_reactionsEnabled->setChecked(m_settings.reactionsEnabled);
    QFont bold = m_reactionsEnabled->font();
    bold.setBold(true);
    m_reactionsEnabled->setFont(bold);
    layout->addWidget(m_reactionsEnabled);

    auto *group = new QGroupBox(QStringLiteral("На что именно откликаться"), page);
    auto *box = new QVBoxLayout(group);

    m_presenceReaction = new QCheckBox(
        QStringLiteral("Появление людей в зоне — звук и строка"), group);
    m_presenceReaction->setChecked(m_settings.presenceReaction);
    box->addWidget(m_presenceReaction);

    box->addWidget(hint(QStringLiteral("Опасные положения:"), group));

    struct Row { QCheckBox **field; QString title; bool value; QString note; };
    const QVector<Row> rows = {
        {&m_ruleUnconscious, QStringLiteral("Лежит без движения"),
         m_settings.thresholds.ruleUnconscious,
         QStringLiteral("самое надёжное правило: положение и неподвижность "
                        "измеряются точно и почти не зависят от бликов")},
        {&m_ruleFall, QStringLiteral("Упал и не встаёт"),
         m_settings.thresholds.ruleFall,
         QStringLiteral("надёжно: резкий переход в горизонталь и человек "
                        "остался лежать")},
        {&m_ruleDrowning, QStringLiteral("Завис в воде без движения"),
         m_settings.thresholds.ruleDrowning,
         QStringLiteral("работает только на панелях, отмеченных как водные; "
                        "признак похож на утопление, но не доказывает его")},
        {&m_ruleChild, QStringLiteral("Ребёнок без присмотра"),
         m_settings.thresholds.ruleChildAlone,
         QStringLiteral("не проверено ни на одном настоящем ребёнке; "
                        "тревогу не поднимает, пока не откалибровано")},
        {&m_ruleUnsteady, QStringLiteral("Неуверенная походка"),
         m_settings.thresholds.ruleUnsteady,
         QStringLiteral("только подозрение, никогда не тревога; по видео "
                        "опьянение установить нельзя")},
    };

    for (const Row &row : rows) {
        auto *check = new QCheckBox(row.title, group);
        check->setChecked(row.value);
        check->setToolTip(row.note);
        box->addWidget(check);
        auto *note = hint(QStringLiteral("      %1").arg(row.note), group);
        box->addWidget(note);
        *row.field = check;
    }

    layout->addWidget(group);

    // Главный выключатель гасит всю группу — так видно, что именно он делает.
    auto updateGroup = [this, group](bool on) { group->setEnabled(on); };
    connect(m_reactionsEnabled, &QCheckBox::toggled, page, updateGroup);
    updateGroup(m_reactionsEnabled->isChecked());

    layout->addStretch(1);
    return page;
}

// ------------------------------------------------------------------ объект

QWidget *SettingsDialog::buildSiteTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    layout->addWidget(hint(QStringLiteral(
        "Эти сведения программа показывает оператору при тревоге и готовит для "
        "звонка в экстренные службы. В момент происшествия вспоминать адрес "
        "некогда — он должен быть на экране."), page));

    auto *form = new QFormLayout;
    form->setSpacing(8);

    m_objectName = new QLineEdit(m_site.objectName, page);
    m_objectName->setPlaceholderText(
        QStringLiteral("Гостиничный комплекс «…», крытый бассейн"));
    form->addRow(QStringLiteral("Название объекта:"), m_objectName);

    m_address = new QPlainTextEdit(m_site.address, page);
    m_address->setMaximumHeight(70);
    m_address->setPlaceholderText(QStringLiteral(
        "Краснодарский край, Сочи, Красная Поляна, ул. …, д. …"));
    form->addRow(QStringLiteral("Адрес для скорой:"), m_address);

    m_landmark = new QLineEdit(m_site.landmark, page);
    m_landmark->setPlaceholderText(
        QStringLiteral("Заезд со стороны … , шлагбаум, охрана пропустит"));
    form->addRow(QStringLiteral("Как проехать:"), m_landmark);

    m_entrance = new QLineEdit(m_site.entrance, page);
    m_entrance->setPlaceholderText(
        QStringLiteral("Корпус …, цокольный этаж, вход через …"));
    form->addRow(QStringLiteral("Как пройти внутри:"), m_entrance);

    m_responsible = new QLineEdit(m_site.responsible, page);
    m_responsible->setPlaceholderText(QStringLiteral("Должность и фамилия"));
    form->addRow(QStringLiteral("Ответственный:"), m_responsible);

    layout->addLayout(form);

    layout->addWidget(hint(QStringLiteral(
        "Программа НЕ придумывает адрес. Пока поле не заполнено, при тревоге "
        "на экране будет написано «АДРЕС НЕ ЗАДАН» — выдуманный адрес хуже "
        "отсутствующего, по нему уедет скорая."), page));

    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------- телефоны

QWidget *SettingsDialog::buildContactsTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    layout->addWidget(hint(QStringLiteral(
        "Порядок в списке — это порядок звонков. Первым идёт тот, кому звонят "
        "первым. Нажатие по номеру во время тревоги кладёт его в буфер обмена."),
        page));

    m_contacts = new QTableWidget(page);
    m_contacts->setColumnCount(3);
    m_contacts->setHorizontalHeaderLabels({QStringLiteral("Кому звоним"),
                                           QStringLiteral("Телефон"),
                                           QStringLiteral("Когда звонить")});
    m_contacts->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_contacts->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_contacts->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_contacts->verticalHeader()->setVisible(false);
    m_contacts->setSelectionBehavior(QAbstractItemView::SelectRows);
    fillContacts(m_site.contacts);
    layout->addWidget(m_contacts, 1);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Добавить"), page);
    auto *remove = new QPushButton(QStringLiteral("Удалить"), page);
    auto *up = new QPushButton(QStringLiteral("Выше"), page);
    auto *down = new QPushButton(QStringLiteral("Ниже"), page);
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addContact);
    connect(remove, &QPushButton::clicked, this, &SettingsDialog::removeContact);
    connect(up, &QPushButton::clicked, this, &SettingsDialog::moveContactUp);
    connect(down, &QPushButton::clicked, this, &SettingsDialog::moveContactDown);
    for (QPushButton *button : {add, remove, up, down})
        buttons->addWidget(button);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    layout->addWidget(hint(QStringLiteral(
        "112 и 103 — общероссийские номера, их менять не нужно. Заполнить "
        "надо внутренние: дежурного спасателя и старшего смены."), page));

    return page;
}

void SettingsDialog::fillContacts(const QVector<core::EmergencyContact> &contacts)
{
    m_contacts->setRowCount(contacts.size());
    for (int row = 0; row < contacts.size(); ++row) {
        const core::EmergencyContact &contact = contacts.at(row);
        m_contacts->setItem(row, 0, new QTableWidgetItem(contact.title));
        m_contacts->setItem(row, 1, new QTableWidgetItem(contact.phone));
        m_contacts->setItem(row, 2, new QTableWidgetItem(contact.note));
    }
}

void SettingsDialog::addContact()
{
    const int row = m_contacts->rowCount();
    m_contacts->insertRow(row);
    m_contacts->setItem(row, 0, new QTableWidgetItem(QString()));
    m_contacts->setItem(row, 1, new QTableWidgetItem(QString()));
    m_contacts->setItem(row, 2, new QTableWidgetItem(QString()));
    m_contacts->setCurrentCell(row, 0);
    m_contacts->editItem(m_contacts->item(row, 0));
}

void SettingsDialog::removeContact()
{
    const int row = m_contacts->currentRow();
    if (row >= 0)
        m_contacts->removeRow(row);
}

void SettingsDialog::moveContact(int delta)
{
    const int row = m_contacts->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_contacts->rowCount())
        return;

    for (int column = 0; column < 3; ++column) {
        QTableWidgetItem *first = m_contacts->takeItem(row, column);
        QTableWidgetItem *second = m_contacts->takeItem(target, column);
        m_contacts->setItem(row, column, second);
        m_contacts->setItem(target, column, first);
    }
    m_contacts->setCurrentCell(target, 0);
}

void SettingsDialog::moveContactUp()   { moveContact(-1); }
void SettingsDialog::moveContactDown() { moveContact(1); }

// --------------------------------------------------------- тревога и звук

QWidget *SettingsDialog::buildAlarmTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    layout->addWidget(hint(QStringLiteral(
        "Задача тревоги — не «сообщить», а гарантированно привлечь внимание. "
        "Сирена звучит непрерывно, пока оператор не нажмёт «ПРИНЯЛ»: один "
        "короткий сигнал в начале легко пропустить, отвернувшись."), page));

    auto *sound = new QGroupBox(QStringLiteral("Звук"), page);
    auto *soundForm = new QFormLayout(sound);

    m_soundEnabled = new QCheckBox(QStringLiteral("Звук включён"), sound);
    m_soundEnabled->setChecked(m_settings.soundEnabled);
    soundForm->addRow(m_soundEnabled);

    m_alarmVolume = new QSpinBox(sound);
    m_alarmVolume->setRange(10, 100);
    m_alarmVolume->setSuffix(QStringLiteral(" %"));
    m_alarmVolume->setValue(int(m_settings.alarmVolume * 100));
    soundForm->addRow(QStringLiteral("Громкость сирены:"), m_alarmVolume);

    m_presenceVolume = new QSpinBox(sound);
    m_presenceVolume->setRange(0, 100);
    m_presenceVolume->setSuffix(QStringLiteral(" %"));
    m_presenceVolume->setValue(int(m_settings.presenceVolume * 100));
    soundForm->addRow(QStringLiteral("Громкость уведомлений:"), m_presenceVolume);

    m_presenceSound = new QCheckBox(
        QStringLiteral("Подавать сигнал, когда в зоне появляются люди"), sound);
    m_presenceSound->setChecked(m_settings.presenceSound);
    soundForm->addRow(m_presenceSound);

    m_repeatSeconds = new QSpinBox(sound);
    m_repeatSeconds->setRange(1, 60);
    m_repeatSeconds->setSuffix(QStringLiteral(" с"));
    m_repeatSeconds->setValue(m_settings.alarmRepeatSeconds);
    soundForm->addRow(QStringLiteral("Повтор сирены каждые:"), m_repeatSeconds);

    m_escalateSeconds = new QSpinBox(sound);
    m_escalateSeconds->setRange(0, 300);
    m_escalateSeconds->setSuffix(QStringLiteral(" с"));
    m_escalateSeconds->setSpecialValueText(QStringLiteral("не усиливать"));
    m_escalateSeconds->setValue(m_settings.escalateAfterSeconds);
    soundForm->addRow(QStringLiteral("Усилить сирену, если нет реакции:"),
                      m_escalateSeconds);

    layout->addWidget(sound);

    auto *window = new QGroupBox(QStringLiteral("Окно при тревоге"), page);
    auto *windowLayout = new QVBoxLayout(window);

    m_raiseWindow = new QCheckBox(
        QStringLiteral("Разворачивать окно и выводить его поверх остальных"), window);
    m_raiseWindow->setChecked(m_settings.raiseWindowOnAlarm);
    windowLayout->addWidget(m_raiseWindow);

    m_flashTaskbar = new QCheckBox(
        QStringLiteral("Мигать значком в панели задач, пока тревога не принята"), window);
    m_flashTaskbar->setChecked(m_settings.flashTaskbarOnAlarm);
    windowLayout->addWidget(m_flashTaskbar);

    m_blink = new QCheckBox(
        QStringLiteral("Мигать красной рамкой и карточкой тревоги"), window);
    m_blink->setChecked(m_settings.blinkOnAlarm);
    windowLayout->addWidget(m_blink);

    layout->addWidget(window);
    layout->addStretch(1);
    return page;
}

// ----------------------------------------------------------- распознавание

QWidget *SettingsDialog::buildDetectionTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    layout->addWidget(hint(QStringLiteral(
        "Пороги подобраны в комнате перед веб-камерой. На объекте их предстоит "
        "уточнить по записям: высота камеры, угол и блики на воде меняют всё. "
        "Уменьшение времени делает систему чувствительнее — и добавляет ложных "
        "срабатываний; увеличение делает наоборот."), page));

    auto *general = new QGroupBox(QStringLiteral("Общее"), page);
    auto *generalForm = new QFormLayout(general);

    m_detectorModel = new QComboBox(general);
    for (auto model : {core::PersonDetector::Model::Accurate,
                       core::PersonDetector::Model::Fast,
                       core::PersonDetector::Model::Legacy}) {
        m_detectorModel->addItem(core::PersonDetector::modelTitle(model), int(model));
    }
    m_detectorModel->setCurrentIndex(int(m_settings.detectorModel));
    m_detectorModel->setToolTip(QStringLiteral(
        "Замерено на записи с идущими людьми: точная модель находит больше "
        "людей и втрое реже теряет их между кадрами. Прежняя (YOLOv5n) "
        "оставлена для совместимости, но её лицензия AGPL-3.0 не годится для "
        "коммерческого объекта."));
    generalForm->addRow(QStringLiteral("Модель поиска людей:"), m_detectorModel);

    m_detectInterval = new QSpinBox(general);
    m_detectInterval->setRange(100, 5000);
    m_detectInterval->setSingleStep(50);
    m_detectInterval->setSuffix(QStringLiteral(" мс"));
    m_detectInterval->setValue(m_settings.detectIntervalMs);
    generalForm->addRow(QStringLiteral("Разбирать панель не чаще, чем раз в:"),
                        m_detectInterval);

    m_fpsCap = new QComboBox(general);
    m_fpsCap->addItem(QStringLiteral("24 к/с"), 24);
    m_fpsCap->addItem(QStringLiteral("30 к/с"), 30);
    m_fpsCap->addItem(QStringLiteral("60 к/с"), 60);
    m_fpsCap->setCurrentIndex(m_fpsCap->findData(m_settings.displayFpsCap));
    if (m_fpsCap->currentIndex() < 0)
        m_fpsCap->setCurrentIndex(1);   // 30 — умолчание, если в файле чужое число
    m_fpsCap->setToolTip(QStringLiteral(
        "Общий предел для всех панелей сразу. IP-камера (в том числе смартфон "
        "с IP Webcam) не всегда держит ровный темп потока сама по себе — предел "
        "не добавляет недостающие кадры, а лишь не даёт показу превысить "
        "выбранный темп."));
    generalForm->addRow(QStringLiteral("Показывать видео не чаще, чем:"), m_fpsCap);

    m_searchConfidence = new QDoubleSpinBox(general);
    m_searchConfidence->setRange(0.15, 0.90);
    m_searchConfidence->setSingleStep(0.05);
    m_searchConfidence->setDecimals(2);
    m_searchConfidence->setValue(m_settings.searchConfidence);
    generalForm->addRow(QStringLiteral("Порог поиска людей:"), m_searchConfidence);

    m_poseEnabled = new QCheckBox(
        QStringLiteral("Разбирать позу и опасные положения"), general);
    m_poseEnabled->setChecked(m_settings.poseEnabled);
    generalForm->addRow(m_poseEnabled);

    m_autoAlarm = new QCheckBox(
        QStringLiteral("Поднимать тревогу автоматически"), general);
    m_autoAlarm->setChecked(m_settings.autoAlarm);
    m_autoAlarm->setToolTip(QStringLiteral(
        "Выключение оставит подсветку панели и запись в журнал, но сирену "
        "включать не будет. Режим для первых дней на новом объекте."));
    generalForm->addRow(m_autoAlarm);

    layout->addWidget(general);

    // --- обучение постоянному окружению -----------------------------------
    auto *learning = new QGroupBox(QStringLiteral("Постоянное окружение"), page);
    auto *learningLayout = new QVBoxLayout(learning);

    m_selfLearning = new QCheckBox(
        QStringLiteral("Учить постоянное окружение"), learning);
    m_selfLearning->setChecked(m_settings.selfLearning);
    m_selfLearning->setToolTip(QStringLiteral(
        "Программа копит, какие места кадра никогда не меняются: стена, дно, "
        "дверной проём. Слабый отклик в таком месте — почти наверняка не "
        "человек, а узор плитки или тень.\n\n"
        "ВКЛЮЧАТЬ, КОГДА КАМЕРЫ СТОЯТ НЕПОДВИЖНО и обстановка меняется мало. "
        "Перевесили камеру или перестроили помещение — обучение надо начать "
        "заново.\n\n"
        "Уверенный отклик проходит всегда: человек может стоять неподвижно, и "
        "в тихом углу кадра он тем более важен."));
    learningLayout->addWidget(m_selfLearning);

    m_learningStatus = new QLabel(learning);
    m_learningStatus->setWordWrap(true);
    m_learningStatus->setStyleSheet(QStringLiteral("color: #939fb0;"));
    m_learningStatus->setText(m_learningState.isEmpty()
                                  ? QStringLiteral("состояние неизвестно")
                                  : m_learningState);
    learningLayout->addWidget(m_learningStatus);

    auto *learnButtons = new QHBoxLayout;

    auto *resetLearning = new QPushButton(
        QStringLiteral("Начать обучение заново"), learning);
    resetLearning->setToolTip(QStringLiteral(
        "Нужно после переноса камер или перестановки в помещении: старая "
        "карта описывает то, чего больше нет."));
    connect(resetLearning, &QPushButton::clicked, this, [this] {
        emit learningResetRequested();
        m_learningStatus->setText(QStringLiteral("обучение начато заново"));
    });
    learnButtons->addWidget(resetLearning);

    auto *importLearning = new QPushButton(
        QStringLiteral("Загрузить готовое…"), learning);
    importLearning->setToolTip(QStringLiteral(
        "Взять карту, накопленную на другом компьютере с той же расстановкой "
        "камер. При ином расположении карта не подойдёт, и программа это "
        "заметит."));
    connect(importLearning, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Файл обучения"), QString(),
            QStringLiteral("Карта окружения (*.json)"));
        if (!path.isEmpty()) {
            emit learningImportRequested(path);
            m_learningStatus->setText(QStringLiteral("загружено из файла"));
        }
    });
    learnButtons->addWidget(importLearning);
    learnButtons->addStretch();
    learningLayout->addLayout(learnButtons);

    layout->addWidget(learning);

    // --- какие панели смотрят на воду ------------------------------------
    auto *water = new QGroupBox(QStringLiteral("Где в кадре вода"), page);
    auto *waterLayout = new QVBoxLayout(water);
    waterLayout->addWidget(hint(QStringLiteral(
        "Отметьте панели, в которые попадает чаша бассейна. Правила, "
        "завязанные на воду (человек завис в воде, укороченное терпение к "
        "неподвижности, ребёнок в воде), работают ТОЛЬКО на отмеченных "
        "панелях.\n\nБез отметки программа считала бы водой любое место, где "
        "не видно ног, — например, стол, за которым сидит человек, — и "
        "поднимала бы тревогу на пустом месте."), water));

    static const QStringList zones = {
        QStringLiteral("Панель 1 — северо-западный угол"),
        QStringLiteral("Панель 2 — северо-восточный угол"),
        QStringLiteral("Панель 3 — юго-западный угол"),
        QStringLiteral("Панель 4 — юго-восточный угол"),
    };
    for (int i = 0; i < zones.size(); ++i) {
        auto *box = new QCheckBox(zones.at(i), water);
        box->setChecked(i < m_settings.panelHasWater.size()
                        && m_settings.panelHasWater.at(i));
        waterLayout->addWidget(box);
        m_water.append(box);
    }
    layout->addWidget(water);

    auto *rules = new QGroupBox(QStringLiteral("Пороги правил"), page);
    auto *rulesForm = new QFormLayout(rules);

    auto addSeconds = [&](const QString &label, double value, double maximum) {
        auto *box = new QDoubleSpinBox(rules);
        box->setRange(0.5, maximum);
        box->setSingleStep(0.5);
        box->setDecimals(1);
        box->setSuffix(QStringLiteral(" с"));
        box->setValue(value);
        rulesForm->addRow(label, box);
        return box;
    };

    m_stillAttention = addSeconds(QStringLiteral("Лежит без движения — внимание через:"),
                                  m_settings.thresholds.unconsciousStillAttention, 120.0);
    m_stillAlarm = addSeconds(QStringLiteral("Лежит без движения — ТРЕВОГА через:"),
                              m_settings.thresholds.unconsciousStillAlarm, 120.0);
    m_stillInWaterAlarm = addSeconds(
        QStringLiteral("То же, но в воде — ТРЕВОГА через:"),
        m_settings.thresholds.unconsciousInWaterAlarm, 60.0);
    m_uprightAttention = addSeconds(
        QStringLiteral("Завис в воде вертикально — внимание через:"),
        m_settings.thresholds.drowningUprightAttention, 60.0);
    m_uprightAlarm = addSeconds(
        QStringLiteral("Завис в воде вертикально — ТРЕВОГА через:"),
        m_settings.thresholds.drowningUprightAlarm, 60.0);
    m_fallStayAlarm = addSeconds(
        QStringLiteral("Упал и не встаёт — ТРЕВОГА через:"),
        m_settings.thresholds.fallStayAlarm, 60.0);

    m_childHeadToTorso = new QDoubleSpinBox(rules);
    m_childHeadToTorso->setRange(0.20, 0.80);
    m_childHeadToTorso->setSingleStep(0.02);
    m_childHeadToTorso->setDecimals(2);
    m_childHeadToTorso->setValue(m_settings.thresholds.childHeadToTorso);
    m_childHeadToTorso->setToolTip(QStringLiteral(
        "Отношение ширины головы к длине туловища. У взрослых измерено "
        "0,22–0,30, у детей заметно больше. Порог проверен только сверху: "
        "детей в кадре при настройке не было."));
    rulesForm->addRow(QStringLiteral("Ребёнок: голова к туловищу от:"), m_childHeadToTorso);

    m_childAlarm = new QCheckBox(
        QStringLiteral("Разрешить тревогу по признаку «ребёнок без присмотра»"), rules);
    m_childAlarm->setChecked(m_settings.thresholds.childAlarmAllowed);
    m_childAlarm->setToolTip(QStringLiteral(
        "Выключено намеренно. Признак ребёнка — единственный, не проверенный "
        "ни на одном настоящем ребёнке: взрослый вблизи камеры даёт те же "
        "числа. Пока выключено, правило подсвечивает панель и пишет в журнал, "
        "но сирену не включает. Включайте после съёмки детей на объекте."));
    rulesForm->addRow(m_childAlarm);

    layout->addWidget(rules);

    layout->addWidget(hint(QStringLiteral(
        "Что программа НЕ умеет и не научится без записей с объекта: отличать "
        "утопление от отдыха в воде наверняка. Она умеет замечать, что человек "
        "перестал двигаться, и звать оператора посмотреть — этого достаточно, "
        "чтобы беда не осталась незамеченной."), page));

    layout->addStretch(1);
    return page;
}

// ------------------------------------------------------------------ итоги

core::SiteInfo SettingsDialog::site() const
{
    core::SiteInfo result = m_site;
    result.objectName = m_objectName->text().trimmed();
    result.address = m_address->toPlainText().trimmed();
    result.landmark = m_landmark->text().trimmed();
    result.entrance = m_entrance->text().trimmed();
    result.responsible = m_responsible->text().trimmed();

    result.contacts.clear();
    for (int row = 0; row < m_contacts->rowCount(); ++row) {
        core::EmergencyContact contact;
        contact.title = m_contacts->item(row, 0) ? m_contacts->item(row, 0)->text().trimmed()
                                                 : QString();
        contact.phone = m_contacts->item(row, 1) ? m_contacts->item(row, 1)->text().trimmed()
                                                 : QString();
        contact.note  = m_contacts->item(row, 2) ? m_contacts->item(row, 2)->text().trimmed()
                                                 : QString();
        if (contact.title.isEmpty() && contact.phone.isEmpty())
            continue;   // пустую строку в списке телефонов хранить незачем
        result.contacts.append(contact);
    }
    return result;
}

core::Settings SettingsDialog::settings() const
{
    core::Settings result = m_settings;

    result.soundEnabled = m_soundEnabled->isChecked();
    result.alarmVolume = m_alarmVolume->value() / 100.0;
    result.presenceVolume = m_presenceVolume->value() / 100.0;
    result.presenceSound = m_presenceSound->isChecked();
    result.alarmRepeatSeconds = m_repeatSeconds->value();
    result.escalateAfterSeconds = m_escalateSeconds->value();

    result.raiseWindowOnAlarm = m_raiseWindow->isChecked();
    result.flashTaskbarOnAlarm = m_flashTaskbar->isChecked();
    result.blinkOnAlarm = m_blink->isChecked();

    result.detectIntervalMs = m_detectInterval->value();
    result.displayFpsCap = m_fpsCap->currentData().toInt();
    result.selfLearning = m_selfLearning->isChecked();
    result.searchConfidence = m_searchConfidence->value();
    result.poseEnabled = m_poseEnabled->isChecked();
    result.autoAlarm = m_autoAlarm->isChecked();
    result.detectorModel =
        core::PersonDetector::Model(m_detectorModel->currentData().toInt());

    result.reactionsEnabled = m_reactionsEnabled->isChecked();
    result.presenceReaction = m_presenceReaction->isChecked();
    result.thresholds.ruleDrowning = m_ruleDrowning->isChecked();
    result.thresholds.ruleUnconscious = m_ruleUnconscious->isChecked();
    result.thresholds.ruleFall = m_ruleFall->isChecked();
    result.thresholds.ruleChildAlone = m_ruleChild->isChecked();
    result.thresholds.ruleUnsteady = m_ruleUnsteady->isChecked();

    result.panelHasWater.clear();
    for (QCheckBox *box : m_water)
        result.panelHasWater.append(box->isChecked());

    result.thresholds.unconsciousStillAttention = m_stillAttention->value();
    result.thresholds.unconsciousStillAlarm = m_stillAlarm->value();
    result.thresholds.unconsciousInWaterAlarm = m_stillInWaterAlarm->value();
    result.thresholds.drowningUprightAttention = m_uprightAttention->value();
    result.thresholds.drowningUprightAlarm = m_uprightAlarm->value();
    result.thresholds.fallStayAlarm = m_fallStayAlarm->value();
    result.thresholds.childHeadToTorso = m_childHeadToTorso->value();
    result.thresholds.childAlarmAllowed = m_childAlarm->isChecked();

    return result;
}

void SettingsDialog::previewCallBrief()
{
    // Показываем ровно тот текст, который оператор будет читать в трубку.
    // Проверять настройки надо до происшествия, а не во время.
    core::AlarmEvent sample;
    sample.zoneLabel = QStringLiteral("северо-западный угол");
    sample.circumstance = core::AlarmCircumstance::UnconsciousInWater;
    sample.peopleInZone = 3;
    sample.origin = core::AlarmOrigin::Automatic;
    sample.raisedAt = QDateTime::currentDateTime();
    sample.details = QStringLiteral("лежит без движения 12 с; ног не видно");

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Так это будет выглядеть при тревоге"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral("Пример текста для диспетчера с текущими настройками:"));
    box.setDetailedText(core::buildCallBrief(sample, site()));
    box.exec();
}

} // namespace ui
