@echo off
rem Copy this file beside start_classroom_schedule_server.bat and remove .example.
rem Keep real tokens in the local copy only. Never commit them.

set "PROJECT_DIR=F:\ESP32\esp32p4-demo\esp-dev-kits-a88faa6\examples\esp32-p4-function-ev-board\examples\esp_brookesia_phone"
set "SERVER_HOST=0.0.0.0"
set "SERVER_PORT=8080"

rem Leave DISPLAY_IP empty to select the active LAN adapter automatically.
set "DISPLAY_IP="

set "SCHEDULE_API_PATH=/classroom-schedule/today"
set "SCHEDULE_API_TOKEN=change-me-local-only"
set "PARENT_CALL_ALERT_API_PATH=/parent-call-alert/trigger"
set "PARENT_CALL_ALERT_API_TOKEN=change-me-local-only"
set "PARENT_CALL_ALERT_MODE=mock"

set "SCHEDULE_PROVIDER=eams-room-occupancy"
set "EAMS_SESSION_FILE=.local-secrets\eams-session.json"
set "EAMS_LOGIN_FILE=.local-secrets\eams-login.json"
set "CHECK_EAMS_SESSION=1"
set "UPSTREAM_TIMEOUT_SECONDS=12"
set "FIXTURE_PATH=tools\fixtures\classroom_schedule_fixture.json"
