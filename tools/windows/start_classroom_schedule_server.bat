@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
set "CONFIG_FILE=%SCRIPT_DIR%classroom_schedule_server_config.bat"

if not exist "%CONFIG_FILE%" (
    echo [ERROR] Config file not found: %CONFIG_FILE%
    echo Copy classroom_schedule_server_config.example.bat to classroom_schedule_server_config.bat first.
    pause
    exit /b 1
)

call "%CONFIG_FILE%"

if not defined PROJECT_DIR (
    echo [ERROR] PROJECT_DIR is missing in %CONFIG_FILE%
    pause
    exit /b 1
)

set "AUTOSTART_SWITCH="
set "CHECK_ONLY_SWITCH="
if /I "%~1"=="--autostart" set "AUTOSTART_SWITCH=-AutoStart"
if /I "%~1"=="--check-only" set "CHECK_ONLY_SWITCH=-CheckOnly"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_DIR%\tools\windows\start_classroom_schedule_server.ps1" %AUTOSTART_SWITCH% %CHECK_ONLY_SWITCH%
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" echo [ERROR] Classroom schedule server did not start.
if "%~1"=="" pause
exit /b %EXIT_CODE%
