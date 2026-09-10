@echo off
setlocal
chcp 65001 >nul
rem Сборка ZiFi: самостоятельный FAT32, основной код, затем упаковка SPG.

for %%I in ("%~dp0.") do set "SPG_DIR=%%~sI"
for %%I in ("%~dp0..\shared\z80") do set "SHARED_Z80=%%~sI"
if not defined FAT32_DRIVER_DIR set "FAT32_DRIVER_DIR=%~dp0..\..\FAT32 Driver"
for %%I in ("%FAT32_DRIVER_DIR%") do set "FAT32_DIR=%%~sI"

if not exist "%SPG_DIR%\build" mkdir "%SPG_DIR%\build"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared Z80 sources were not found: %SHARED_Z80%
  exit /b 1
)

cd /d "%SPG_DIR%"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "..\..\ZiFi\sjasmplus.exe" set "SJASM=..\..\ZiFi\sjasmplus.exe"
if not defined SJASM (
  where sjasmplus.exe >nul 2>nul
  if not errorlevel 1 set "SJASM=sjasmplus.exe"
)
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

set "SJASMPLUS=%SJASM%"
python "%FAT32_DIR%\build.py"
if errorlevel 1 exit /b 1
copy /y "%FAT32_DIR%\build\fat32.bin" "%SPG_DIR%\build\fat32.bin" >nul
if errorlevel 1 exit /b 1
copy /y "%FAT32_DIR%\build\fat32-work.bin" "%SPG_DIR%\build\fat32-work.bin" >nul
if errorlevel 1 exit /b 1

"%SJASM%" --nologo --inc="%SHARED_Z80%" --inc="%FAT32_DIR%\include" --sym="build\zifi.sym" zifi.asm
if errorlevel 1 exit /b 1

set "PATH=%SPG_DIR%\_spg;%PATH%"
cd /d "%SPG_DIR%\_spg"
"%SPG_DIR%\_spg\spgbld.exe" -b spgbld.ini "%SPG_DIR%\build\zifi.spg"
if errorlevel 1 exit /b 1

echo Built: %~dp0build\zifi.spg
