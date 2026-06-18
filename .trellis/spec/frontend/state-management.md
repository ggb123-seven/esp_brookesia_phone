# State Management

> How state is managed in this project.

---

## Overview

State is managed with C++ app members, LVGL object state, FreeRTOS primitives,
NVS, and a few module-level variables for hardware pipelines. There is no
global frontend state library.

Prefer app-owned state first. Use module-level static state only when it
represents a singleton hardware pipeline, model instance, or C callback bridge.

---

## State Categories

- UI object state: LVGL objects and generated `ui_...` globals.
- App lifecycle state: class members such as `_screen_index`, `_is_ui_resumed`,
  `_img_album_buffer`, `_midea_info_vect`, and `_nvs_param_map`.
- Task/event state: FreeRTOS event groups, semaphores, and task handles such as
  `camera_event_group`, `_camera_init_sem`, and `_detect_task_handle`.
- Persistent state: NVS settings and game best score.
- File/media state: SD card video selections and SPIFFS media paths.
- Hardware/model state: camera controller handles, PPA client handles, and
  ESP-DL detector singletons.

---

## When to Use Global State

Use static/module-level state only when the underlying resource is effectively
singleton:

- Camera frame pipeline and event group shared by V4L2 callbacks and app tasks.
- Detection model wrappers cached by `get_pedestrian_detect()` or
  `get_humanface_detect()`.
- Simple video player context that represents one active player instance.

Do not add globals just to avoid passing `this` into a callback. LVGL supports
callback user data and the existing apps already use it.

## Code Example

Mode shared between camera callbacks and tasks is stored in event bits:

```cpp
xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);

if (xEventGroupGetBits(camera_event_group) & CAMERA_EVENT_PED_DETECT) {
    detect_results = app_pedestrian_detect((uint16_t *)p->buffer,
                                           app->_hor_res, app->_ver_res);
}
```

---

## Server State

There is no server state. Hardware and storage state should be synchronized
through the owning component:

- NVS values are loaded into app members/maps and written back through the app's
  setter methods.
- Wi-Fi scan/connect state is translated into UI state by the settings app.
- Camera/video state is synchronized with event groups and stop/wait APIs before
  buffers are released.
- SD card media state is rebuilt by scanning files rather than cached across
  app lifetimes unless the app owns that cache.

---

## Derived State

- Keep screen indexes as enums near the class that owns them.
- Keep mode bits in event groups when background tasks and callbacks both need
  them, as in the camera app.
- Recompute display-only strings in the UI update path. Avoid storing the same
  displayed value in multiple places unless one copy is the persistent source of
  truth.
- When NVS defaults are first created, update both the NVS value and the app's
  in-memory map before updating LVGL.

---

## Common Mistakes

- Updating generated `ui_...` objects after the app has been closed or deleted.
- Releasing buffers while a task or frame callback can still use them.
- Keeping UI mode in both an enum and event bits without defining which one
  drives behavior.
- Making persistent settings local to one callback instead of routing through
  the NVS load/set helpers.
- Holding LVGL locks across state waits, file scans, or Wi-Fi operations.
