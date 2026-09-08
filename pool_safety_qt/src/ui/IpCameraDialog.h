#pragma once

// ---------------------------------------------------------------------------
//  Подключение IP-камеры.
//
//  ТРИ ПУТИ В ОДНОМ ОКНЕ, потому что нужны все:
//
//    * НАЙТИ В СЕТИ — для того, кто не знает, какие камеры стоят на объекте.
//      Программа опрашивает сеть и показывает список. Камеру, которая назвала
//      себя сама, подставляет целиком: адрес, протокол, порт и путь.
//    * ВСТАВИТЬ ГОТОВЫЙ АДРЕС — для того, у кого ссылка уже есть: из браузера,
//      из переписки, из настроек самой камеры. Строка разбирается по полям
//      сама. Раньше этот путь был закрыт, и адрес приходилось разносить
//      вручную, ошибаясь в порту и в пути.
//    * ЗАПОЛНИТЬ ПОЛЯ — для того, кто знает параметры. Это же единственный
//      путь, если камера в другой подсети или молчит на опрос.
//
//  ПРОТОКОЛ ВЫБИРАЕТСЯ, А НЕ ПОДРАЗУМЕВАЕТСЯ. Пока схема жёстко была rtsp://,
//  смартфон в роли камеры подключить было нельзя вовсе: он отдаёт MJPEG по
//  http://…:8080/video. Оба протокола проверены на живом потоке и работают.
//
//  ПРОВЕРКА СВЯЗИ ОБЯЗАТЕЛЬНА. Кнопка «Проверить» пробует получить кадр и
//  показывает результат. Без неё оператор узнал бы об ошибке в адресе только
//  тогда, когда панель осталась пустой, — и гадал бы, где ошибся: в пути, в
//  пароле или в номере потока.
// ---------------------------------------------------------------------------

#include "core/SourceDescriptor.h"
#include "core/MjpegWorker.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QMediaPlayer;
class QVideoSink;
class QTimer;

namespace core {
class CameraDiscovery;
struct DiscoveredCamera;
}

namespace ui {

class IpCameraDialog : public QDialog
{
    Q_OBJECT

public:
    explicit IpCameraDialog(QWidget *parent = nullptr);
    ~IpCameraDialog() override;

    /// Что выбрал оператор. Действителен после accept().
    core::SourceDescriptor source() const;

private slots:
    void startSearch();
    void onCameraFound(const core::DiscoveredCamera &camera);
    void onSearchFinished(int found);
    void useSelected();
    void applyTemplate(int index);
    void testConnection();
    void updatePreviewUrl();

    /// Адрес ввели или вставили. Если это целая ссылка — разложить по полям.
    void hostTextChanged();

private:
    void stopTest(const QString &message, bool success);

    /// Текущая схема: rtsp либо http.
    QString currentScheme() const;

    /// Итоговый адрес потока.
    ///
    /// Целая ссылка в поле адреса главнее отдельных полей: если оператор
    /// вставил её и сразу нажал «Подключить», не уводя фокус, поля разобраться
    /// ещё не успели — а подключиться должно то, что он вставил.
    QString composeUrl() const;

    /// Выставить поля, не вызывая обратной волны сигналов.
    void setFields(const QString &scheme, const QString &host, int port,
                   const QString &user, const QString &password, const QString &path);

    core::CameraDiscovery *m_discovery = nullptr;

    QListWidget *m_list = nullptr;
    QPushButton *m_search = nullptr;
    QProgressBar *m_progress = nullptr;

    QLineEdit *m_host = nullptr;
    QComboBox *m_scheme = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QComboBox *m_vendor = nullptr;
    QLineEdit *m_path = nullptr;
    QLineEdit *m_name = nullptr;
    QLabel *m_urlPreview = nullptr;
    QLabel *m_testResult = nullptr;
    QPushButton *m_test = nullptr;

    /// Поля заполняются программой, а не человеком — обработчики молчат.
    bool m_filling = false;

    // --- пробное подключение -----------------------------------------------
    //
    // ДВА ПУТИ, КАК И У NetworkSource. Проверка честна только тогда, когда
    // повторяет то же самое подключение, которое случится после «Подключить»:
    // http(s) идёт через MjpegWorker (он и понимает голые потоки, снимки без
    // multipart и пароль — см. core/MjpegWorker.h), rtsp — через QMediaPlayer,
    // как раньше. Раздельная логика проверки здесь однажды уже разошлась бы с
    // тем, что покажется на панели, если бы обе схемы шли через один путь.
    QMediaPlayer *m_player = nullptr;
    QVideoSink *m_sink = nullptr;
    core::MjpegWorker *m_mjpegTest = nullptr;
    QTimer *m_testTimeout = nullptr;
};

} // namespace ui
