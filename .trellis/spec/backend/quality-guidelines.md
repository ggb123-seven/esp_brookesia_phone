# Quality Guidelines

> Code quality standards for backend development.

---

## Overview

Backend quality means firmware reliability: deterministic initialization,
bounded memory use, explicit FreeRTOS ownership, correct LVGL locking, and
repeatable ESP-IDF builds. The repository currently relaxes several compiler
warnings in `CMakeLists.txt`; do not treat those suppressions as permission to
add sloppy code.

---

## Forbidden Patterns

- Editing `managed_components/` or `build/` for project behavior. Use local
  wrapper code, Kconfig, CMake options, or component manifests instead.
- Calling LVGL APIs from a FreeRTOS task or callback without
  `esp_lv_adapter_lock()` unless the call is already on the LVGL-owned context.
- Adding large frame/audio/model buffers to internal RAM by default. Use
  `heap_caps_*` with `MALLOC_CAP_SPIRAM` for large UI/video/AI buffers.
- Adding silent catch-all failures. Log failures and propagate `esp_err_t`,
  `false`, `NULL`, or a negative descriptor according to the local API.
- Hard-coding board-specific values in app logic when a BSP, Kconfig symbol, or
  existing macro already owns the value.
- Introducing C++ exceptions or RTTI-dependent patterns into firmware code.
- Copying generated SquareLine/LVGL asset patterns manually without updating the
  corresponding source asset or generated file list.

---

## Required Patterns

- Start C/C++ source and public headers with the existing SPDX header when
  creating new project files.
- Keep C/C++ interop headers wrapped in `extern "C"` when they expose C
  functions to C++ callers.
- Use `ESP_ERROR_CHECK()` only for mandatory setup paths; use recoverable
  returns for optional app paths.
- For new phone apps, derive from `ESP_Brookesia_PhoneApp`, implement
  `init()`, `run()`, `back()`, and `close()`, declare the app icon with
  `LV_IMG_DECLARE`, and install the app from `main/main.cpp`.
- For background tasks that update UI, use event groups/semaphores for state
  and acquire `esp_lv_adapter_lock()` around LVGL calls.
- For large DMA/cache-sensitive buffers, use aligned allocations and the cache
  alignment helpers already used by camera/video code.
- Add new app source files under `components/apps/<app>/`; the component CMake
  currently uses recursive globbing, but new include paths and external
  dependencies still belong in `components/apps/CMakeLists.txt`.

## Code Example

Keep LVGL updates from worker callbacks short and locked:

```cpp
if (!(current_bits & CAMERA_EVENT_DELETE) && (esp_lv_adapter_lock(100) == ESP_OK)) {
    lv_canvas_set_buffer(ui_ImageCameraShotImage, camera_buf,
                         camera_buf_hes, camera_buf_ves, LV_IMG_CF_TRUE_COLOR);
    lv_refr_now(NULL);
    esp_lv_adapter_unlock();
}
```

---

## Testing Requirements

- At minimum, run an ESP-IDF build after code changes:

```bash
idf.py build
```

- For configuration changes, run or document the relevant `idf.py menuconfig`
  path and update `sdkconfig.defaults` when the default matters.
- For UI changes, boot the firmware on the ESP32-P4 function EV board and verify
  the affected app opens, closes, and returns through the navigation bar.
- For camera/video changes, verify stream start/stop, app close, and task
  cleanup because these paths own large shared buffers.
- For storage changes, test first-boot defaults and reboot persistence.

---

## Code Review Checklist

- Does the change stay out of `managed_components/` and `build/`?
- Are all LVGL calls from tasks protected by `esp_lv_adapter_lock()`?
- Are allocations checked and freed on every failure and close path?
- Does the return type match the local convention (`esp_err_t`, `bool`,
  pointer, or descriptor)?
- Are Kconfig symbols, partition names, and model locations kept consistent
  across `Kconfig`, `CMakeLists.txt`, and code?
- Does new app code follow the existing `ESP_Brookesia_PhoneApp` lifecycle?
- Are secrets and high-frequency frame details kept out of logs?
- Did the change update `sdkconfig.defaults`, `partitions.csv`, or SPIFFS
  content when needed?
