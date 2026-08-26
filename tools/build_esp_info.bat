@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

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

set "SHARED_Z80=%~dp0..\shared\z80"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared Z80 sources were not found: %SHARED_Z80%
  exit /b 1
)

"%SJASM%" --inc="%SHARED_Z80%" esp_info.asm
if errorlevel 1 exit /b 1

echo Built: %~dp0esp_info.sna
