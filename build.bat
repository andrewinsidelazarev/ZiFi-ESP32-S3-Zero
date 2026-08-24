@echo off
setlocal

rem Xtensa GCC 8.4 cannot read framework or linker map paths through Unicode.
rem Use existing DOS short paths only in this child PlatformIO process.
for %%I in ("%USERPROFILE%\.platformio") do set "PLATFORMIO_CORE_DIR=%%~sI"
if not exist "%PLATFORMIO_CORE_DIR%\platforms" (
  echo Error: PlatformIO core directory was not found: %PLATFORMIO_CORE_DIR%
  exit /b 1
)

for %%I in ("%~dp0.") do set "ZIFI_PROJECT_DIR=%%~sI"
cd /d "%ZIFI_PROJECT_DIR%"

python -u -m platformio run %*
if errorlevel 1 exit /b %errorlevel%

call "%~dp0Update Mode\build.bat"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0Online Update\build.bat"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0SMB Server\build.bat"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0FTP Server\build.bat"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0NTP Time Sync\build.bat"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0ZiFi SPG\build.bat"
exit /b %errorlevel%
