; ---------------------------------------------------------------------------
;  Установщик «Наблюдение за бассейном»
;
;  Собирается компилятором Inno Setup 6:
;      ISCC.exe deploy\PoolSafety.iss
;  Обычно вызывается не вручную, а из deploy\build_release.ps1 — тот сначала
;  соберёт программу, прогонит проверки и разложит поставку.
;
;  ЧТО ЗДЕСЬ ВАЖНО И ПОЧЕМУ:
;
;    * Настройки и журналы НЕ лежат рядом с программой. Программа сама
;      выбирает место: рядом с собой, если туда можно писать (распакованный
;      архив), иначе — ProgramData. Поэтому установщику не нужно раздавать
;      права на запись внутрь Program Files, а это известный способ подмены
;      исполняемых файлов. См. src/core/AppPaths.h.
;    * Модели входят в поставку целиком: система обязана работать без
;      интернета, на объекте его может не быть вовсе.
;    * При удалении настройки и журналы НЕ трогаются: журнал происшествий —
;      документ, его нельзя стирать заодно с программой.
; ---------------------------------------------------------------------------

#define AppName "Наблюдение за бассейном"
; Версия задаётся скриптом сборки (ключ /DAppVersion), который читает её из
; src\core\Version.h. Значение ниже — только для ручного запуска ISCC.
#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#define AppPublisher "Система видеонаблюдения за бассейном"
#define AppExe "PoolSafety.exe"

[Setup]
AppId={{7E3A9C41-5B62-4E18-9F0D-2C7A55D1B840}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Установка программы «{#AppName}»

DefaultDirName={autopf}\PoolSafety
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes

; Папку установки оператор выбирает сам — страница выбора показывается всегда.
DisableDirPage=no

LicenseFile=LICENSE.txt
InfoAfterFile=ПОСЛЕ_УСТАНОВКИ.txt

OutputDir=..\dist\Установщик
OutputBaseFilename=Наблюдение за бассейном {#AppVersion}
SetupIconFile=PoolSafety.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}

Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
PrivilegesRequired=admin

; Не ставить поверх запущенной программы: подмена файлов на ходу кончается
; тем, что тревога перестаёт работать в самый неподходящий момент.
AppMutex=PoolSafetyRunning
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать значок на рабочем столе"; GroupDescription: "Дополнительно:"
Name: "autostart"; Description: "Запускать при входе в систему"; GroupDescription: "Дополнительно:"; Flags: unchecked

[Files]
Source: "..\dist\PoolSafety\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "LICENSE.txt"; DestDir: "{app}"; DestName: "Лицензионное соглашение.txt"; Flags: ignoreversion
Source: "THIRD-PARTY.txt"; DestDir: "{app}"; DestName: "Сторонние компоненты.txt"; Flags: ignoreversion

[Dirs]
; Общий каталог данных: настройки и журнал происшествий принадлежат объекту, а
; не учётной записи. Оператор следующей смены должен видеть тот же адрес и тот
; же журнал, под какой бы учётной записью он ни вошёл.
Name: "{commonappdata}\PoolSafety"; Permissions: users-modify
Name: "{commonappdata}\PoolSafety\config"; Permissions: users-modify
Name: "{commonappdata}\PoolSafety\logs"; Permissions: users-modify

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\{#AppExe}"; Comment: "Пост оператора: видео, распознавание людей, тревога"
Name: "{group}\Руководство"; Filename: "{app}\Руководство.md"
Name: "{group}\Лицензионное соглашение"; Filename: "{app}\Лицензионное соглашение.txt"
Name: "{group}\Удалить {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Автозапуск — общий для всех учётных записей, а не для той, под которой шла
; установка. Ставит систему администратор, а работать за ней будет оператор:
; запись в профиль администратора не запустила бы ничего.
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "PoolSafety"; ValueData: """{app}\{#AppExe}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "Запустить программу"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Удаляем только то, что положили сами. Настройки и журналы в ProgramData
; остаются: журнал происшествий — документ, и стирать его заодно с программой
; нельзя.
Type: filesandordirs; Name: "{app}\models"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\multimedia"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\config"
Type: filesandordirs; Name: "{app}\logs"

[Messages]
russian.WelcomeLabel2=Программа наблюдения за бассейном: четыре видеопанели, распознавание людей и опасных положений тела, тревога с готовыми данными для звонка в экстренные службы.%n%nВсё считается на этом компьютере, без интернета. Лица не распознаются.%n%nПрограмма вспомогательная и не заменяет дежурного спасателя.
russian.FinishedLabel=Программа установлена.%n%nПеред вводом в работу откройте «Настройки» и заполните адрес объекта и телефоны: при тревоге программа показывает их оператору.
