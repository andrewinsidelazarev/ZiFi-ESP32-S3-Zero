# Сборка нативного симулятора SMB-сервера тем же компилятором, которым уже
# собираются тесты проекта. Прошивочный код берётся как есть, из src/ и lib/:
# симулятор существует именно для того, чтобы проверять его, а не копию.

param([switch]$ServerOnly)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

# Компилятор ищем ровно так же, как tests\run_tests.ps1: без -products * vswhere
# не показывает отдельно установленные Build Tools.
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe из Visual Studio Build Tools не найден'
}
$visualStudio = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $visualStudio) { throw 'C++ workload Build Tools не найден' }
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'

# Путь держим относительным: полный содержит пробелы, а cmd /c теряет кавычки
# внутри длинной командной строки компилятора.
$out = '.test-build\host_smb'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force $out | Out-Null }

# Заглушки платформы идут первыми: они подменяют Arduino.h, WiFi.h, FreeRTOS и
# heap_caps, не трогая ни одной строки прошивочных исходников.
$includes = @(
  '/Itests\stubs_host',
  '/Iinclude',
  '/Ilib\libsmb2\include',
  '/Ilib\libsmb2\include\smb2',
  '/Itools\host_smb'
) -join ' '

$sources = @(
  'tools\host_smb\main.cpp',
  'tools\host_smb\z80_sim.cpp',
  'tools\host_smb\crash_report.cpp',
  'src\smb_server.cpp',
  'src\vfs_bridge.cpp',
  'src\vfs_client.cpp',
  'src\directory_cache.cpp',
  'src\fat_allocation_cache.cpp',
  'src\spsc_ring.cpp',
  'src\protocol.cpp',
  'src\uart_transport.cpp',
  'src\ws_discovery.cpp',
  'src\ws_discovery_xml.cpp',
  'src\diagnostic_log.cpp'
) -join ' '

$libsmb2 = (Get-ChildItem 'lib\libsmb2\lib\*.c' |
            Where-Object { $_.Name -notmatch 'krb5|aes_apple|dreamcast' } |
            ForEach-Object { 'lib\libsmb2\lib\' + $_.Name }) -join ' '

# /Zi — отладочная информация: ради неё симулятор и затевался, паника должна
# превращаться в обычный стек вызовов.
$command = "cl.exe /nologo /std:c++17 /EHsc /utf-8 /Zi /MDd /W3 " +
           "/D_CRT_SECURE_NO_WARNINGS /DZIFI_DIAGNOSTIC_LOG=1 " +
           # ZIFI_HOST_BUILD отключает куски прошивки, которые на ПК
           # уже предоставлены системой (например, gethostname).
           # libsmb2 умеет Windows, но включает эту ветку по _WINDOWS и
           # __USE_WINSOCK__, а MSVC сам задаёт только _WIN32.
           "/DZIFI_HOST_BUILD /D_WINDOWS /DHAVE_LINGER " +
           # libsmb2 под MSVC не подтягивает stdint сам.
           "/FIzifi_msvc_prelude.h " +
           "$includes $sources $libsmb2 " +
           "/Fo$out\ /Fd$out\host_smb.pdb " +
           "/Fe$out\host_smb.exe /link ws2_32.lib /MAP:$out\host_smb.map"

Write-Output 'Сборка симулятора...'
cmd /c ('"' + $vcvars + '" >nul && ' + $command)
if ($LASTEXITCODE -ne 0) { throw "Сборка не удалась ($LASTEXITCODE)" }
Write-Output "Готово: $out\host_smb.exe"

if ($ServerOnly) {
    return
}

# Клиент-пробник: Проводник к симулятору не подпустить (порт в UNC не указать,
# 445 занят ядром), поэтому сценарии прогоняет он.
$probe = "cl.exe /nologo /TC /utf-8 /Zi /MDd /W3 /D_CRT_SECURE_NO_WARNINGS " +
         "/D_WINDOWS /DHAVE_LINGER /FIzifi_msvc_prelude.h " +
         "/Itests\stubs_host /Ilib\libsmb2\include /Ilib\libsmb2\include\smb2 " +
         "tools\host_smb\smb_probe.c $libsmb2 " +
         "/Fo$out\probe\ /Fd$out\smb_probe.pdb " +
         "/Fe$out\smb_probe.exe /link ws2_32.lib"
if (-not (Test-Path "$out\probe")) { New-Item -ItemType Directory -Force "$out\probe" | Out-Null }
Write-Output 'Сборка клиента-пробника...'
cmd /c ('"' + $vcvars + '" >nul && ' + $probe)
if ($LASTEXITCODE -ne 0) { throw "Сборка пробника не удалась ($LASTEXITCODE)" }
Write-Output "Готово: $out\smb_probe.exe"

# Проверялка чтения: читает весь ресурс и складывает файлы на диск, сверку
# содержимого делает вызывающая сторона.
$readcheck = "cl.exe /nologo /TC /utf-8 /Zi /MDd /W3 /D_CRT_SECURE_NO_WARNINGS " +
         "/D_WINDOWS /DHAVE_LINGER /FIzifi_msvc_prelude.h " +
         "/Itests\stubs_host /Ilib\libsmb2\include /Ilib\libsmb2\include\smb2 " +
         "tools\host_smb\smb_readcheck.c $libsmb2 " +
         "/Fo$out\probe2\ /Fd$out\smb_readcheck.pdb " +
         "/Fe$out\smb_readcheck.exe /link ws2_32.lib"
if (-not (Test-Path "$out\probe2")) { New-Item -ItemType Directory -Force "$out\probe2" | Out-Null }
Write-Output 'Сборка проверялки чтения...'
cmd /c ('"' + $vcvars + '" >nul && ' + $readcheck)
if ($LASTEXITCODE -ne 0) { throw "Сборка проверялки не удалась ($LASTEXITCODE)" }
Write-Output "Готово: $out\smb_readcheck.exe"
# Повтор хвоста Проводника после чтения: именно на нём вставало железо.

$tailcheck = "cl.exe /nologo /TC /utf-8 /Zi /MDd /W3 /D_CRT_SECURE_NO_WARNINGS " +
         "/D_WINDOWS /DHAVE_LINGER /FIzifi_msvc_prelude.h " +
         "/Itests\stubs_host /Ilib\libsmb2\include /Ilib\libsmb2\include\smb2 " +
         "tools\host_smb\smb_tailcheck.c $libsmb2 " +
         "/Fo$out\probe3\ /Fd$out\smb_tailcheck.pdb " +
         "/Fe$out\smb_tailcheck.exe /link ws2_32.lib"
if (-not (Test-Path "$out\probe3")) { New-Item -ItemType Directory -Force "$out\probe3" | Out-Null }
Write-Output 'Сборка проверялки хвоста...'
cmd /c ('"' + $vcvars + '" >nul && ' + $tailcheck)
if ($LASTEXITCODE -ne 0) { throw "Сборка проверялки хвоста не удалась ($LASTEXITCODE)" }
Write-Output "Готово: $out\smb_tailcheck.exe"

$reproduce = "cl.exe /nologo /TC /utf-8 /Zi /MDd /W3 /D_CRT_SECURE_NO_WARNINGS " +
         "/D_WINDOWS /DHAVE_LINGER /FIzifi_msvc_prelude.h " +
         "/Itests\stubs_host /Ilib\libsmb2\include /Ilib\libsmb2\include\smb2 " +
         "tools\host_smb\smb_reproduce_test.c $libsmb2 " +
         "/Fo$out\probe4\ /Fd$out\smb_reproduce_test.pdb " +
         "/Fe$out\smb_reproduce_test.exe /link ws2_32.lib"
if (-not (Test-Path "$out\probe4")) { New-Item -ItemType Directory -Force "$out\probe4" | Out-Null }
Write-Output 'Сборка проверялки воспроизведения багов...'
cmd /c ('"' + $vcvars + '" >nul && ' + $reproduce)
if ($LASTEXITCODE -ne 0) { throw "Сборка проверялки воспроизведения не удалась ($LASTEXITCODE)" }
Write-Output "Готово: $out\smb_reproduce_test.exe"

# Проводник Windows всегда подключается к TCP/445, но этот порт уже занят
# системным LanmanServer. Не останавливаем системные службы: отдельный
# пакетный мост представляет 192.168.1.222:445 и переводит только этот поток в
# нативный ZiFi SMB-сервер на 127.0.0.1:1445.
$winDivert = Join-Path $out 'windivert\WinDivert-2.2.2-A'
$winDivertHeader = Join-Path $winDivert 'include\windivert.h'
$winDivertLib = Join-Path $winDivert 'x64\WinDivert.lib'
if ((Test-Path -LiteralPath $winDivertHeader) -and
    (Test-Path -LiteralPath $winDivertLib)) {
    $virtualBridge = "cl.exe /nologo /std:c++17 /EHsc /utf-8 /Zi /MDd /W4 " +
        "/D_CRT_SECURE_NO_WARNINGS " +
        "/I`"$winDivert\include`" tools\host_smb\virtual_ip_bridge.cpp " +
        "`"$winDivertLib`" /Fo$out\virtual-bridge\ " +
        "/Fd$out\zifi_virtual_ip_bridge.pdb " +
        "/Fe$out\zifi_virtual_ip_bridge.exe /link ws2_32.lib"
    if (-not (Test-Path "$out\virtual-bridge")) {
        New-Item -ItemType Directory -Force "$out\virtual-bridge" | Out-Null
    }
    Write-Output 'Сборка виртуального SMB-адреса...'
    cmd /c ('"' + $vcvars + '" >nul && ' + $virtualBridge)
    if ($LASTEXITCODE -ne 0) {
        throw "Сборка виртуального SMB-адреса не удалась ($LASTEXITCODE)"
    }
    Copy-Item -LiteralPath (Join-Path $winDivert 'x64\WinDivert.dll') `
        -Destination $out -Force
    Copy-Item -LiteralPath (Join-Path $winDivert 'x64\WinDivert64.sys') `
        -Destination $out -Force
    Write-Output "Готово: $out\zifi_virtual_ip_bridge.exe"
} else {
    Write-Warning 'WinDivert не найден; виртуальный адрес не собран.'
}
