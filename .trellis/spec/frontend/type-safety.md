# Type Safety

> Type safety patterns in this project.

---

## Overview

This project uses C and C++, not TypeScript. Type safety comes from explicit
ESP-IDF types, fixed-width integer types, enums, config structs, handle typedefs,
and careful C/C++ interop boundaries.

Do not introduce untyped `void *` payloads beyond existing C callback and handle
interfaces. When a `void *` is required by LVGL, FreeRTOS, or ESP-IDF, cast it
at the boundary and validate it before use.

---

## Type Organization

- Public C APIs expose `esp_err_t`, concrete structs, typedefs, and
  `extern "C"` guards in headers.
- App classes keep private enums and member fields in their `*.hpp` files.
  Examples: `SettingScreenIndex_t`, `WifiSignalStrengthLevel_t`, and
  `WifiConnectState_t` in `Setting.hpp`.
- Hardware/video formats use enums that wrap external constants, as in
  `video_fmt_t` in `app_video.h`.
- C handles use opaque typedefs when ownership should not leak internals:
  `typedef void *pipeline_handle_t`.
- Buffers use fixed-width pointer types (`uint8_t *`, `uint16_t *`) and explicit
  size fields.

## Code Example

Use typed enums and callback typedefs at C/C++ boundaries:

```c
typedef enum {
    APP_VIDEO_FMT_RGB565 = V4L2_PIX_FMT_RGB565,
    APP_VIDEO_FMT_RGB888 = V4L2_PIX_FMT_RGB24,
} video_fmt_t;

typedef void (*app_video_frame_operation_cb_t)(
    uint8_t *camera_buf, uint8_t camera_buf_index,
    uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len);
```

---

## Validation

- Validate public function arguments before use and return `ESP_ERR_INVALID_ARG`,
  `NULL`, or `false` according to the API.
- Check every allocation result, especially `heap_caps_aligned_alloc()` and
  `heap_caps_calloc()` for SPIRAM buffers.
- For event callback user data, check the recovered app pointer before using it
  when the callback can outlive or detach from the object.
- Validate vector/list contents before indexing detection outputs. Camera code
  already checks bounding boxes and keypoints for size and non-zero values.
- For file paths and buffers, keep destination array sizes visible and use
  bounded formatting/copying APIs.

---

## Common Patterns

- `esp_err_t` for service success/failure and `ESP_OK` on success.
- `bool` for app lifecycle success/failure.
- Fixed-width integer types for hardware data, frame dimensions, and pixels.
- `static_cast` or `reinterpret_cast` in C++ code when converting callback data
  or frame buffers.
- `PRIu32`/`PRId32` format macros for fixed-width logging.
- Bit fields for compact option flags in C config structs, as used by
  `esp_lvgl_simple_player_cfg_t`.

---

## Forbidden Patterns

- C++ exceptions for firmware control flow.
- Unchecked C-style casts from `void *` followed by immediate dereference.
- Magic integer modes when a local enum or Kconfig symbol already exists.
- Writing past fixed-size arrays for paths, labels, or result buffers.
- Assuming LVGL object pointers remain valid after `close()` or screen deletion.
- Mixing RGB565/RGB888 buffer interpretations without checking the active
  `CONFIG_BSP_LCD_COLOR_FORMAT_*` path.
