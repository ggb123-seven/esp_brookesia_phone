# Error Handling

> How errors are handled in this project.

---

## Overview

Use ESP-IDF error conventions for firmware services and boolean lifecycle
returns for `ESP_Brookesia_PhoneApp` classes.

Common patterns in this repository:

- Initialization that cannot continue uses `ESP_ERROR_CHECK()` or `assert()`,
  especially in `main/main.cpp` and early app initialization.
- Recoverable service APIs return `esp_err_t` and validate inputs with
  `ESP_RETURN_ON_FALSE()`, `ESP_GOTO_ON_FALSE()`, or explicit `ESP_FAIL`.
- App lifecycle methods return `bool`; log the failure with `ESP_LOGE()` before
  returning `false`.
- Pointer-returning factory functions return `NULL` on failure and log the
  reason, as in `main/lvgl_adapter_init.c` and
  `esp_lvgl_simple_player_create()`.

---

## Error Types

- Prefer standard `esp_err_t` values (`ESP_OK`, `ESP_FAIL`,
  `ESP_ERR_INVALID_ARG`, `ESP_ERR_NO_MEM`) for C-style services.
- Use negative file descriptors for Linux/V4L2-style device open failures, as
  in `app_video_open()`.
- Use `NULL` for object/handle creation failures where the API returns a
  pointer.
- Use `false` for app lifecycle failures (`init()`, `run()`, `close()`,
  `pause()`, `resume()`).
- Do not add C++ exceptions. This codebase uses ESP-IDF C/C++ without exception
  flow.

---

## Error Handling Patterns

- Validate public C API arguments at the top of the function. Example:
  `camera_element_pipeline_new()` checks configuration and allocation with
  `ESP_GOTO_ON_FALSE()`, and `camera_element_pipeline_delete()` checks the
  handle with `ESP_RETURN_ON_FALSE()`.
- Use `goto err` cleanup for multi-step allocation paths that allocate
  semaphores, buffers, or device resources.
- On app-level setup failures, log with the module `TAG` and return `false`.
  Examples: `Game2048::init()`, `MusicPlayer::init()`, and `Camera::run()`.
- Use `ESP_ERROR_CHECK()` only when failure should abort the current firmware
  path. It is acceptable in `app_main()` and critical setup callbacks, but avoid
  it in optional feature paths where the app can display an error or disable a
  feature.
- For cross-task cleanup, signal tasks first, then wait for them to stop before
  freeing shared buffers. `Camera::close()` and the simple video player are the
  local examples.
- LVGL calls from background tasks must only run after acquiring
  `esp_lv_adapter_lock()` and must always release it on the same path.

## Code Example

Use ESP-IDF check macros in allocation-heavy service setup:

```cpp
ESP_GOTO_ON_FALSE(cfg && cfg->elem_num > 0, ESP_ERR_INVALID_ARG, err, TAG,
                  "Invalid configuration: elem_num must be greater than 0.");
stream = static_cast<camera_pipeline_stream *>(
    heap_caps_calloc(1, sizeof(camera_pipeline_stream), cfg->caps));
ESP_GOTO_ON_FALSE(stream, ESP_ERR_NO_MEM, err, TAG,
                  "Failed to allocate memory for camera_pipeline_stream.");
```

---

## Resource Cleanup

There are no HTTP/API error responses in this firmware. The equivalent contract
is resource ownership:

- If a function allocates with `heap_caps_*`, free with `heap_caps_free()` or
  the matching `free()` used by the surrounding code.
- If a function creates a semaphore/task/event group, it owns deletion or a
  documented handoff to the app lifecycle.
- If a C API returns a handle, provide a paired delete/stop API when the handle
  owns memory or a task. Examples: `camera_element_pipeline_delete()`,
  `app_video_stream_task_stop()`, and `esp_lvgl_simple_player_del()`.
- For app `close()` methods, stop tasks and release app-owned buffers before
  returning.

---

## Common Mistakes

- Calling LVGL from a task without `esp_lv_adapter_lock()`.
- Using `ESP_ERROR_CHECK()` in user-triggered or optional feature paths where a
  logged `false`/`ESP_FAIL` would preserve the rest of the UI.
- Allocating SPIRAM buffers without checking for `NULL`.
- Returning early after acquiring a lock, opening a file, or allocating a
  buffer without releasing the resource.
- Logging only numeric error codes when `esp_err_to_name(err)` is available.
- Freeing task-shared buffers before the task has observed its stop/delete
  event.
