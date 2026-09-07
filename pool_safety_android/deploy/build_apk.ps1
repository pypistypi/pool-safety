# ---------------------------------------------------------------------------
#  Сборка приложения для Android.
#
#  СОБИРАЕМ ВО ВРЕМЕННОЙ ПАПКЕ, а не на месте. Путь проекта содержит
#  «ИИ наблюдение», а средства сборки Android на кириллицу в пути отзываются
#  так же скверно, как moc из состава Qt: ошибки появляются не сразу и
#  выглядят как что угодно, только не как проблема с путём.
#
#  Итог кладётся в dist\ рядом с проектом.
# ---------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $env:TEMP 'PoolSafetyAndroid'
$dist = Join-Path $root 'dist'

$jdk = 'C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot'
$sdk = 'D:\Android\Sdk'
$gradle = 'D:\Android\gradle-8.7\bin\gradle.bat'

# --- проверки окружения ----------------------------------------------------
foreach ($item in @(@{ p = $jdk; n = 'JDK 17' },
                    @{ p = $sdk; n = 'Android SDK' },
                    @{ p = $gradle; n = 'Gradle' })) {
    if (-not (Test-Path $item.p)) {
        Write-Host ("Не найдено: " + $item.n + " (" + $item.p + ")") -ForegroundColor Red
        exit 1
    }
}

$env:JAVA_HOME = $jdk
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk

# --- перенос в путь без кириллицы -----------------------------------------
Write-Host 'Готовлю каталог сборки...' -ForegroundColor Cyan
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force $work | Out-Null
robocopy $root $work /E /XD build .gradle dist /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Host 'Копирование не удалось' -ForegroundColor Red; exit 1 }

Set-Content -Path (Join-Path $work 'local.properties') `
            -Value ('sdk.dir=' + $sdk.Replace('\', '\\').Replace(':', '\:')) `
            -Encoding ascii

# --- сборка ----------------------------------------------------------------
$type = if ($args.Count -gt 0 -and $args[0] -eq 'release') { 'Release' } else { 'Debug' }
Write-Host ("Собираю APK (" + $type + ")...") -ForegroundColor Cyan

& $gradle -p $work ("assemble" + $type) --no-daemon --console=plain
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Сборка не удалась' -ForegroundColor Red
    exit 1
}

# --- итог ------------------------------------------------------------------
$apk = Get-ChildItem (Join-Path $work 'app\build\outputs\apk') -Recurse -Filter *.apk |
       Select-Object -First 1
if (-not $apk) {
    Write-Host 'APK не найден' -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force $dist | Out-Null
$target = Join-Path $dist ('PoolSafetyWatch-' + $type.ToLower() + '.apk')
Copy-Item $apk.FullName $target -Force

Write-Host ''
Write-Host ('Готово: ' + $target) -ForegroundColor Green
Write-Host ('Размер: ' + [math]::Round($apk.Length / 1MB, 1) + ' МБ')
Write-Host ''
Write-Host 'Установить на подключённый телефон или эмулятор:' -ForegroundColor Cyan
Write-Host ('  ' + $sdk + '\platform-tools\adb.exe install -r "' + $target + '"')
