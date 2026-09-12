#pragma once

// ---------------------------------------------------------------------------
//  Настройки: всё, что администратор объекта меняет без разработчика.
//
//  ПОЧЕМУ ЭТО ОКНО ОБЯЗАТЕЛЬНО. Адрес, телефоны и порядок действий программа
//  показывает оператору в момент, когда человек тонет. Пока их можно было
//  править только в файле, они с большой вероятностью остались бы
//  незаполненными — и в тревоге на экране красовалось бы «НЕ ЗАПОЛНЕНО».
//  Настройка, которая требует текстового редактора и знания формата, на
//  объекте не происходит никогда.
//
//  ЧЕТЫРЕ РАЗДЕЛА, ПО ПОРЯДКУ ВАЖНОСТИ:
//
//    1. ОБЪЕКТ — адрес для скорой, заезд, проход внутри, ответственный.
//    2. ТЕЛЕФОНЫ — кому звонить, в каком порядке и когда.
//    3. ТРЕВОГА И ЗВУК — насколько громко и настойчиво звать оператора.
//    4. РАСПОЗНАВАНИЕ — пороги правил; их подстраивают на объекте по записям.
//
//  Здесь же кнопка «Проверить» — она показывает ровно тот текст, который
//  оператор будет читать в трубку. Проверять настройки надо до происшествия, а
//  не во время.
// ---------------------------------------------------------------------------

#include "core/Settings.h"
#include "core/SiteInfo.h"

#include <QDialog>
#include <QVector>

class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QCheckBox;
class QLabel;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;

namespace ui {

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(const core::SiteInfo &site, const core::Settings &settings,
                   QWidget *parent = nullptr);

    /// Сведения об объекте с учётом правок оператора.
    core::SiteInfo site() const;

    /// Настройки программы с учётом правок оператора.
    core::Settings settings() const;

private slots:
    void addContact();
    void removeContact();
    void moveContactUp();
    void moveContactDown();
    void previewCallBrief();

public:
    /// Показать состояние обучения. Вызывается до открытия окна.
    void setLearningState(const QString &text) { m_learningState = text; }

signals:
    /// Оператор просит забыть накопленное и начать заново.
    void learningResetRequested();

    /// Оператор выбрал готовый файл обучения.
    void learningImportRequested(const QString &path);

private:
    QWidget *buildReactionsTab();
    QWidget *buildSiteTab();
    QWidget *buildContactsTab();
    QWidget *buildAlarmTab();
    QWidget *buildDetectionTab();
    void fillContacts(const QVector<core::EmergencyContact> &contacts);
    void moveContact(int delta);

    core::SiteInfo m_site;
    core::Settings m_settings;

    // --- отклики -----------------------------------------------------------
    QCheckBox *m_reactionsEnabled = nullptr;
    QCheckBox *m_presenceReaction = nullptr;
    QCheckBox *m_ruleDrowning = nullptr;
    QCheckBox *m_ruleUnconscious = nullptr;
    QCheckBox *m_ruleFall = nullptr;
    QCheckBox *m_ruleChild = nullptr;
    QCheckBox *m_ruleUnsteady = nullptr;
    QCheckBox *m_ruleSplashing = nullptr;

    // --- объект ------------------------------------------------------------
    QLineEdit *m_objectName = nullptr;
    QPlainTextEdit *m_address = nullptr;
    QLineEdit *m_landmark = nullptr;
    QLineEdit *m_entrance = nullptr;
    QLineEdit *m_responsible = nullptr;

    // --- телефоны ----------------------------------------------------------
    QTableWidget *m_contacts = nullptr;

    // --- тревога и звук ----------------------------------------------------
    QCheckBox *m_soundEnabled = nullptr;
    QSpinBox *m_alarmVolume = nullptr;
    QSpinBox *m_presenceVolume = nullptr;
    QSpinBox *m_repeatSeconds = nullptr;
    QSpinBox *m_escalateSeconds = nullptr;
    QCheckBox *m_presenceSound = nullptr;
    QCheckBox *m_raiseWindow = nullptr;
    QCheckBox *m_flashTaskbar = nullptr;
    QCheckBox *m_blink = nullptr;

    // --- распознавание -----------------------------------------------------
    QComboBox *m_detectorModel = nullptr;
    QSpinBox *m_detectInterval = nullptr;
    QComboBox *m_fpsCap = nullptr;
    QDoubleSpinBox *m_searchConfidence = nullptr;
    QCheckBox *m_poseEnabled = nullptr;
    QCheckBox *m_autoAlarm = nullptr;
    /// Что показать про обучение. Задаётся снаружи: состояние знает
    /// вычислительный поток, а окно только показывает.
    QString m_learningState;

    QCheckBox *m_selfLearning = nullptr;
    QLabel *m_learningStatus = nullptr;
    QVector<QCheckBox *> m_water;
    QDoubleSpinBox *m_stillAttention = nullptr;
    QDoubleSpinBox *m_stillAlarm = nullptr;
    QDoubleSpinBox *m_stillInWaterAlarm = nullptr;
    QDoubleSpinBox *m_uprightAttention = nullptr;
    QDoubleSpinBox *m_uprightAlarm = nullptr;
    QDoubleSpinBox *m_fallStayAlarm = nullptr;
    QDoubleSpinBox *m_splashingAlarm = nullptr;
    QCheckBox *m_splashingAlarmAllowed = nullptr;
    QDoubleSpinBox *m_childHeadToTorso = nullptr;
    QCheckBox *m_childAlarm = nullptr;
};

} // namespace ui
