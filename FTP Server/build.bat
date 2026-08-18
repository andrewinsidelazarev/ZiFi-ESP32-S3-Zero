@echo off
setlocal
chcp 65001
rem Put all generated files under build.
if not exist "%~dp0build" mkdir "%~dp0build"
set "SHARED_Z80=%~dp0..\shared\z80"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared sources were not found: %SHARED_Z80%
  exit /b 1
)
cd /d "%~dp0\src"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
rem This path is resolved from FTP Server\src.
if not defined SJASM if exist "..\..\..\ZiFi\sjasmplus.exe" set "SJASM=..\..\..\ZiFi\sjasmplus.exe"
if not defined SJASM (
  where sjasmplus.exe
  if not errorlevel 1 set "SJASM=sjasmplus.exe"
)
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

"%SJASM%" --inc="%SHARED_Z80%" --sym=..\build\ZIFIFTP.sym --lst=..\build\ZIFIFTP.lst main.asm
if errorlevel 1 exit /b 1

echo Built: %~dp0build\ZIFIFTP.WMF
echo Firmware and update.sna: ..\firmware\
