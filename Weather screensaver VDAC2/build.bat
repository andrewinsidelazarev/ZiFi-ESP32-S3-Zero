@echo off
setlocal
chcp 65001 >nul
rem Build order: tools/gen_assets.py (fonts, icons, FT812 RAM_G image, tables,
rem strings), then sjasmplus assembles WEATHER2.WMF with INCBIN of the data
rem pages. Shared Z80 modules come from ../shared/z80 (proto, config, uart)
rem and ../shared/weather (the weather saver core shared with WEATHER.WMF).
if not exist "%~dp0build" mkdir "%~dp0build"
set "SHARED_Z80=%~dp0..\shared\z80"
set "SHARED_WEATHER=%~dp0..\shared\weather"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared Z80 sources were not found: %SHARED_Z80%
  exit /b 1
)
if not exist "%SHARED_WEATHER%\saver.asm" (
  echo Error: shared weather sources were not found: %SHARED_WEATHER%
  exit /b 1
)

python "%~dp0tools\gen_assets.py"
if errorlevel 1 exit /b 1

cd /d "%~dp0src"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "..\..\..\ZiFi\sjasmplus.exe" set "SJASM=..\..\..\ZiFi\sjasmplus.exe"
if not defined SJASM (
  where sjasmplus.exe >nul 2>nul
  if not errorlevel 1 set "SJASM=sjasmplus.exe"
)
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

"%SJASM%" --inc="%SHARED_Z80%" --inc="%SHARED_WEATHER%" --sym=..\build\WEATHER2.sym --lst=..\build\WEATHER2.lst main.asm
if errorlevel 1 exit /b 1
rem sjasmplus saved the header and the code page; the four 16 KiB data
rem pages do not fit into its 64 KiB address space and are appended here.
cd /d "%~dp0build"
copy /b WEATHER2.WMF+page1.bin+page2.bin+page3.bin+page4.bin WEATHER2.WMF >nul
if errorlevel 1 exit /b 1

echo Built: %~dp0build\WEATHER2.WMF
