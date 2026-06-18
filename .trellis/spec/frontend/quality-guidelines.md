# Quality Guidelines

> Code quality standards for frontend development.

---

## Overview

Frontend quality means responsive embedded UI, correct app lifecycle behavior,
safe LVGL access from tasks, and UI code that fits the ESP-Brookesia app model.
The UI runs on constrained hardware, so every visual feature must account for
memory, task timing, and close/pause behavior.

---

## Forbidden Patterns

- Blocking Wi-Fi, file scanning, video decoding, or AI detection directly inside
  LVGL event callbacks.
- Updating LVGL objects from non-LVGL tasks without `esp_lv_adapter_lock()`.
- Creating new apps that do not implement `back()` and `close()`.
- Editing generated UI/asset files without knowing whether they will be
  regenerated from SquareLine or source assets.
- Adding large fonts/images without checking SPIFFS/flash/PSRAM impact.
- Logging credentials from UI forms.
- Adding UI text or controls that cannot fit the 1024x600 target panel.

---

## Required Patterns

- Keep app installation explicit in `main/main.cpp`.
- Keep UI callbacks static and recover app state from callback user data.
- Keep generated UI initialization calls (`ui_init()` style functions) in the
  app `run()`/initialization path, with app-specific extra setup in the app
  class.
- Use FreeRTOS tasks for ongoing Wi-Fi scan, camera stream, video playback, and
  periodic refresh work.
- Stop tasks and wait for stop completion before freeing buffers or deleting
  player/camera resources.
- Use existing fonts, image declarations, and LVGL style APIs unless a shared
  helper already exists.
- For page parameter tables or centralized UI config lists, keep comments above
  the list definition and update them whenever parameters move between pages.

## Code Example

Close paths must stop background work before releasing UI-owned resources:

```cpp
app_video_stream_task_stop(_camera_ctlr_handle);
app_video_stream_wait_stop();

if (_img_album_buffer) {
    heap_caps_free(_img_album_buffer);
    _img_album_buffer = NULL;
}
```

---

## Testing Requirements

- Build the firmware after UI changes:

```bash
idf.py build
```

- On hardware, open the affected app, interact with every changed control, use
  the navigation bar/back path, close the app, and reopen it.
- For settings UI, test Wi-Fi scan/connect paths without exposing credentials in
  logs.
- For camera UI, test normal, pedestrian detection, and face detection modes,
  then close the app and confirm the stream task exits.
- For video player UI, test play, pause, resume, stop, repeat, file selection,
  and app close with an SD card inserted.
- For generated UI exports, verify `components/apps/setting/ui/CMakeLists.txt`
  includes every exported screen/image/font source.

---

## Code Review Checklist

- Does the app follow the `ESP_Brookesia_PhoneApp` lifecycle?
- Are LVGL calls from tasks locked and unlocked on all paths?
- Are UI object pointers only used while the owning screen/app is alive?
- Are background tasks bounded by pause/close/delete state?
- Are app state fields centralized instead of duplicated across callbacks?
- Are generated UI files and source assets kept consistent?
- Does the UI still fit the target display and use existing enabled fonts?
- Are secrets, media buffers, and per-frame data kept out of logs?
