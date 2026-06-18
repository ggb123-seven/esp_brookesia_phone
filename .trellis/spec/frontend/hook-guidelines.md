# Hook Guidelines

> How hooks are used in this project.

---

## Overview

There are no React hooks. The equivalent extension points are LVGL event
callbacks, ESP-Brookesia app lifecycle methods, FreeRTOS tasks, and ESP-IDF
event handlers.

Use the existing callback style instead of introducing a new event framework.

---

## Custom Hook Patterns

- App lifecycle hooks:
  - `init()` performs one-time app setup and backend initialization.
  - `run()` creates or resumes the visible UI.
  - `pause()` and `resume()` control background work when implemented.
  - `back()` reports navigation closure through Brookesia.
  - `close()` stops tasks and releases app-owned buffers.
- LVGL callbacks should be static class methods when they operate on app state.
  Pass `this` as callback user data.
- ESP-IDF event handlers, such as Wi-Fi handlers in `Setting.cpp`, should keep
  event decoding small and delegate UI work to app methods or tasks.
- FreeRTOS task entry points are static functions that receive `void *arg` or a
  typed app pointer, then delete themselves with `vTaskDelete(NULL)` when done.

## Code Example

LVGL callbacks pass the app instance through user data:

```cpp
lv_obj_add_event_cb(ui_ButtonCameraShotBtn, onScreenCameraShotBtnClick,
                    LV_EVENT_CLICKED, this);

Camera *camera = (Camera *)e->user_data;
if (camera == NULL) {
    return;
}
```

---

## Data Fetching

"Data fetching" is device I/O:

- Wi-Fi scan results are fetched by settings tasks and then copied into LVGL UI
  under `esp_lv_adapter_lock()`.
- Camera frames are received through `app_video` callbacks and passed through
  the camera pipeline before display.
- Video files are discovered from the SD card by the video player app.
- SPIFFS media is mounted once in `main/main.cpp`; apps consume files through
  BSP/file helper APIs.

Long-running I/O must not block the LVGL UI thread. Use FreeRTOS tasks,
semaphores, event groups, and short locked UI update sections.

---

## Naming Conventions

- LVGL UI callbacks are named for the object and event:
  `onButtonWifiListClickedEventCallback`,
  `onScreenCameraShotBtnClick`, `file_changed`.
- FreeRTOS tasks use names that describe the work:
  `wifiScanTask`, `wifiConnectTask`, `euiRefresTask`,
  `camera_dectect_task`.
- ESP-IDF event handlers end in `EventHandler`, as in `wifiEventHandler`.
- C callback typedefs should include the subsystem name and `_cb_t`, as in
  `app_video_frame_operation_cb_t`.

---

## Common Mistakes

- Doing slow file, Wi-Fi, video, or AI work directly inside an LVGL event
  callback.
- Keeping an LVGL lock while performing blocking I/O or long computation.
- Passing a pointer through callback user data without checking it before use.
- Updating UI after the app has set its delete/close flag.
- Forgetting that callbacks may run after a mode transition unless event bits
  or app state explicitly guard them.
