# Logging Guidelines

> How logging is done in this project.

---

## Overview

Use ESP-IDF logging macros from `esp_log.h`: `ESP_LOGI`, `ESP_LOGW`, and
`ESP_LOGE`. Each `.c` or `.cpp` module should define a file-local tag:

```c
static const char *TAG = "Camera";
```

Small wrappers that predate the convention may log with literal tags; new code
should prefer a `TAG` constant for consistency.

---

## Log Levels

- `ESP_LOGI`: successful subsystem transitions and useful runtime state.
  Examples: storage mounted in `main/main.cpp`, camera/video stream start/stop,
  Wi-Fi scan progress, video file selection.
- `ESP_LOGW`: degraded but expected runtime conditions. Examples: SD card/video
  player prerequisites, uninitialized player operations, optional mirror
  controls that fail and can be skipped.
- `ESP_LOGE`: failed initialization, allocation, file I/O, device operations,
  or invalid app state that prevents the requested action.
- `printf`: only for high-frequency performance counters or demo output where
  the existing code already uses it, such as camera FPS printing. Prefer
  `ESP_LOG*` elsewhere.

---

## Structured Logging

- Use concise English messages with enough context to identify the failing
  subsystem.
- Include `esp_err_to_name(err)` for ESP-IDF errors when the symbolic name helps
  diagnosis, as in NVS handling in `Setting.cpp` and `Game_2048.cpp`.
- Use integer format macros for fixed-width or platform-sensitive values when
  appropriate (`PRIu32`, `PRId32`), as shown in video player and camera code.
- For pointer/buffer diagnostics, log the pointer and ownership/internal flag
  only at initialization or debug-worthy transitions; avoid per-frame spam.

## Code Example

```cpp
static const char *TAG = "Game2048";

if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
    return false;
}
```

---

## What to Log

- Mounting or initialization success for storage, display, camera, codec, Wi-Fi,
  and player subsystems.
- File discovery and selected media paths for the video player.
- Task start/stop and timeout failures for long-running FreeRTOS tasks.
- NVS default initialization, load, set, and commit failures.
- Allocation failures for large buffers, especially SPIRAM frame buffers and
  media caches.
- User-visible feature failures that should help reproduce the issue from the
  serial monitor.

---

## What NOT to Log

- Do not log Wi-Fi passwords, credentials, tokens, or private file contents.
  Existing Wi-Fi connect logs include the password; do not copy or extend that
  pattern.
- For Python HTTP middleware or test servers based on `BaseHTTPRequestHandler`,
  do not use the default request logging when URLs can contain secrets in the
  query string. Override `log_request()` or equivalent logging hooks so logs
  include only the method, path, status, and size, not `?token=...`, cookies, or
  session identifiers.
- Do not log every frame, every LVGL event, or every loop iteration in
  high-frequency camera/video paths.
- Do not log full binary buffers, model contents, audio data, image data, or
  raw SPIFFS/SD card payloads.
- Avoid noisy success logs inside callbacks that run from ISR or frame
  operations.

## Python HTTP Middleware Request Logging

### 1. Scope / Trigger

Use this contract whenever a project tool or server exposes an HTTP endpoint
whose request URL can include secrets, session material, or device access
tokens in the query string.

### 2. Signatures

For standard-library Python servers:

```python
class Handler(BaseHTTPRequestHandler):
    def log_request(self, code: int | str = "-", size: int | str = "-") -> None:
        ...
```

### 3. Contracts

- Safe log fields: client address, HTTP method, parsed path, protocol version,
  status code, and response size.
- Unsafe log fields: raw `self.path`, full request line, query string,
  `token`, `Cookie`, `JSESSIONID`, WebVPN session URLs, and complete private
  payloads.
- Startup logs may show a sample URL only if secret values are replaced with a
  placeholder such as `<redacted>`.

### 4. Validation & Error Matrix

- URL contains `?token=...` -> log path only, never the query string.
- Unauthorized request -> response may say `invalid token`, but must not echo
  the supplied or expected token.
- Unexpected server exception -> log a sanitized category; do not dump request
  headers or private upstream responses by default.

### 5. Good / Base / Bad Cases

- Good: `GET /classroom-schedule/today HTTP/1.1 200 -`
- Base: `GET /health HTTP/1.1 200 -`
- Bad: `GET /classroom-schedule/today?classroom=A101&token=secret HTTP/1.1`

### 6. Tests Required

- Run an HTTP self-test or curl check with a known fake token and inspect output
  to ensure the token and query string do not appear.
- Verify successful, unauthorized, and bad-request paths all use the same
  sanitized logging behavior.

### 7. Wrong vs Correct

Wrong:

```python
def log_message(self, fmt: str, *args: object) -> None:
    print(fmt % args)  # BaseHTTPRequestHandler formats the full request line.
```

Correct:

```python
def log_request(self, code: int | str = "-", size: int | str = "-") -> None:
    parsed = urlparse(self.path)
    request_line = f"{self.command} {parsed.path} {self.request_version}"
    print(f'{self.address_string()} - "{request_line}" {code} {size}')
```

## Python Parent Call Alert Receiver Contract

### 1. Scope / Trigger

Use this contract when a Python HTTP middleware endpoint receives firmware
alert events from `parent_call_alert` or another sensor-triggered service.
This endpoint is a cross-layer boundary: ESP32 firmware builds the payload,
the middleware validates it, and logs must remain safe for normal operations.

### 2. Signatures

For the schedule middleware alert receiver:

```text
POST /parent-call-alert/trigger
Header: Content-Type: application/json
Header: X-Alert-Token: <PARENT_CALL_ALERT_API_TOKEN>
Body: {"reason": "...", "detail": "...", "message": "...", "timestamp_ms": 123}
```

### 3. Contracts

- `reason`: required string, max 48 characters; stable machine-readable event
  kind such as `mq2_alarm`.
- `detail`: optional string, max 96 characters; short diagnostic detail.
- `message`: optional string, max 160 characters; user-facing alert summary.
- `timestamp_ms`: optional non-negative number; firmware event timestamp in
  milliseconds.
- Request body limit: 2048 bytes.
- Environment keys: `PARENT_CALL_ALERT_API_PATH`,
  `PARENT_CALL_ALERT_API_TOKEN`, and `PARENT_CALL_ALERT_MODE`.
- Success response: `{"ok":true,"status":"accepted","mode":"mock",...}`.
- Logs may include `reason` and field lengths only. Do not log
  `X-Alert-Token`, full `detail`, full `message`, headers, cookies, or raw
  request bodies.

### 4. Validation & Error Matrix

- Missing or wrong `X-Alert-Token` -> `401 unauthorized`.
- Missing/invalid `Content-Type` -> `400 bad_request`.
- Missing/invalid `Content-Length` -> `400 bad_request`.
- Body larger than 2048 bytes -> `413 payload_too_large`.
- Invalid UTF-8 or malformed JSON -> `400 bad_request`.
- Non-object JSON -> `400 bad_request`.
- Missing `reason` -> `400 bad_request`.
- Unsupported characters or overlong fields -> `400 bad_request`.

### 5. Good / Base / Bad Cases

- Good: MQ-2 alarm posts `reason=mq2_alarm` with short detail/message and a
  valid alert token; middleware returns `accepted` and logs only lengths.
- Base: `PARENT_CALL_ALERT_MODE=mock` acknowledges the event without dialing.
- Bad: logging the complete message, query string, token, or raw JSON body.

### 6. Tests Required

- Self-test or curl check for successful alert acceptance.
- Unauthorized token check; assert response body does not echo either token.
- Missing `reason` check; assert `400 bad_request`.
- Oversized body check; assert `413 payload_too_large`.
- Non-JSON Content-Type check; assert `400 bad_request`.
- Inspect test logs to confirm only method/path/status, `reason`, and field
  lengths are printed.

### 7. Wrong vs Correct

Wrong:

```python
print(f"alert headers={self.headers} body={body}")
```

Correct:

```python
self.log_message(
    "parent call alert accepted reason=%s detail_len=%d message_len=%d",
    alert["reason"],
    len(alert["detail"]),
    len(alert["message"]),
)
```
