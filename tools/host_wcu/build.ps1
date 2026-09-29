# Сборка стенда обновлятора WC тем же компилятором, что и остальные стенды.
# Прошивочный код берётся как есть из src/; эмулятор WC — из tools/host_smb.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe из Visual Studio Build Tools не найден'
}
$visualStudio = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $visualStudio) { throw 'C++ workload Build Tools не найден' }
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'

$out = '.test-build\host_wcu'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force $out | Out-Null }

$includes = @(
  '/Itests\stubs_host',
  '/Iinclude',
  '/Itools\host_smb'
) -join ' '

$sources = @(
  'tools\host_wcu\main.cpp',
  'tools\host_smb\z80_sim.cpp',
  'src\wc_updater.cpp',
  'src\github_tree.cpp',
  'src\git_sha1.cpp',
  'src\vfs_bridge.cpp',
  'src\vfs_client.cpp',
  'src\fat_allocation_cache.cpp',
  'src\spsc_ring.cpp',
  'src\protocol.cpp',
  'src\uart_transport.cpp',
  'src\diagnostic_log.cpp'
) -join ' '

$command = "cl.exe /nologo /std:c++17 /EHsc /utf-8 /Zi /MDd /W3 " +
           "/D_CRT_SECURE_NO_WARNINGS /DZIFI_HOST_BUILD /D_WINDOWS " +
           "/FIzifi_msvc_prelude.h $includes $sources " +
           "/Fo$out\ /Fd$out\host_wcu.pdb /Fe$out\host_wcu.exe /link ws2_32.lib"

Write-Output 'Сборка стенда обновлятора WC...'
cmd /c ('"' + $vcvars + '" >nul && ' + $command)
if ($LASTEXITCODE -ne 0) { throw "Сборка не удалась ($LASTEXITCODE)" }
Write-Output "Готово: $out\host_wcu.exe"
