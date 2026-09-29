@echo off
setlocal
chcp 65001
rem Put all generated files under build.
if not exist "%~dp0build" mkdir "%~dp0build"
set "SHARED_Z80=%~dp0..\shared\z80"
rem File requests are served by the FTP plugin modules (vfs.asm, fs.asm).
set "FTP_SRC=%~dp0..\FTP Server\src"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared sources were not found: %SHARED_Z80%
  exit /b 1
)
if not exist "%FTP_SRC%\vfs.asm" (
  echo Error: FTP plugin sources were not found: %FTP_SRC%
  exit /b 1
)
cd /d "%~dp0\src"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
rem This path is resolved from WC Update\src.
if not defined SJASM if exist "..\..\..\ZiFi\sjasmplus.exe" set "SJASM=..\..\..\ZiFi\sjasmplus.exe"
if not defined SJASM (
  where sjasmplus.exe
  if not errorlevel 1 set "SJASM=sjasmplus.exe"
)
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

"%SJASM%" --inc="%SHARED_Z80%" --inc="%FTP_SRC%" --sym=..\build\WCUPDATE.sym --lst=..\build\WCUPDATE.lst main.asm
if errorlevel 1 exit /b 1

echo Built: %~dp0build\WCUPDATE.WMF
