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
- Do not log every frame, every LVGL event, or every loop iteration in
  high-frequency camera/video paths.
- Do not log full binary buffers, model contents, audio data, image data, or
  raw SPIFFS/SD card payloads.
- Avoid noisy success logs inside callbacks that run from ISR or frame
  operations.
