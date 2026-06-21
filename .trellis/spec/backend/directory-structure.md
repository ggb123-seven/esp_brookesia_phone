# Directory Structure

> How backend code is organized in this project.

---

## Overview

This is an ESP-IDF firmware project for `esp32p4`, not a server backend. Treat
"backend" as the firmware/service layer: board bring-up, storage, camera/video
capture, AI model wrappers, FreeRTOS tasks, and non-visual application logic.

The source of truth for project code is `main/`, `components/`, and `spiffs/`.
Do not derive project conventions from `managed_components/` or `build/`:
`managed_components/` contains third-party component-manager dependencies and
`build/` contains generated build output.

---

## Directory Layout

```text
.
|-- CMakeLists.txt                  # ESP-IDF project entry and global flags
|-- partitions.csv                  # NVS, phy, factory app, SPIFFS layout
|-- sdkconfig.defaults              # Minimal checked-in project defaults
|-- main/
|   |-- main.cpp                    # app_main(), storage/display/app bootstrap
|   |-- lvgl_adapter_init.c/.h      # BSP display, touch, LVGL adapter setup
|   |-- Kconfig.projbuild           # Example-level options such as SD card
|   `-- idf_component.yml           # Top-level ESP-IDF component dependencies
|-- components/
|   |-- apps/                       # ESP-Brookesia phone apps and UI logic
|   |-- human_face_detect/          # ESP-DL face model wrapper and model assets
|   `-- pedestrian_detect/          # ESP-DL pedestrian model wrapper/assets
|-- spiffs/                         # Files embedded into the SPIFFS partition
|-- managed_components/             # External dependencies, do not edit locally
`-- build/                          # Generated output, never edit or document
```

---

## Module Organization

- `main/main.cpp` owns system bootstrap only: initialize NVS, SPIFFS/SD card,
  codec, display/LVGL, create `ESP_Brookesia_Phone`, then install app objects.
- `main/lvgl_adapter_init.c` owns BSP display/touch setup. Keep display bring-up
  here instead of duplicating it in app modules.
- `components/apps/` is one ESP-IDF component containing all phone apps. Each
  app uses its own folder, e.g. `calculator/`, `camera/`, `setting/`,
  `music_player/`, `video_player/`, and `game_2048/`.
- App classes live in matching `*.hpp` and `*.cpp` files and derive from
  `ESP_Brookesia_PhoneApp`. Examples: `components/apps/camera/Camera.hpp`,
  `components/apps/setting/Setting.hpp`, and
  `components/apps/video_player/VideoPlayer.hpp`.
- Shared app-level C APIs stay next to the app that owns them. Examples:
  `components/apps/camera/app_video.h`, `app_camera_pipeline.hpp`,
  `app_pedestrian_detect.h`, and `app_humanface_detect.h`.
- AI model components are separate ESP-IDF components under
  `components/human_face_detect/` and `components/pedestrian_detect/`, each
  with its own `CMakeLists.txt`, `Kconfig`, model files, and wrapper class.
- Runtime media assets that must be in SPIFFS belong under `spiffs/` and are
  included through `spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)`.

---

## Naming Conventions

- ESP-IDF components and C helper modules use snake_case paths and symbols:
  `app_video.c`, `app_camera_pipeline.cpp`, `lvgl_adapter_init.c`.
- C++ app classes use PascalCase class names and matching file names:
  `Calculator`, `Game2048`, `MusicPlayer`, `AppSettings`, `Camera`,
  `AppVideoPlayer`.
- Compile-time constants use uppercase macros: `EXAMPLE_CAM_BUF_NUM`,
  `CAMERA_INIT_TASK_WAIT_MS`, `LVGL_ADAPTER_BUFFER_HEIGHT`.
- ESP-IDF configuration symbols use `CONFIG_...`; project Kconfig options use
  the existing `EXAMPLE_...`, `HUMAN_FACE_DETECT_...`, or
  `PEDESTRIAN_DETECT_...` prefixes.
- FreeRTOS task functions and C callback functions use lower_snake_case or the
  existing app-local style, e.g. `wifiScanTask`, `camera_dectect_task`,
  `camera_video_frame_operation`.
- Public C headers that are consumed from C++ must keep `extern "C"` guards,
  as in `main/lvgl_adapter_init.h`, `components/apps/camera/app_video.h`, and
  `components/apps/video_player/esp_lvgl_simple_player/esp_lvgl_simple_player.h`.

---

## Examples

- `main/main.cpp`: clear example of system initialization and app installation.
- `main/lvgl_adapter_init.c`: single-purpose display/touch/LVGL adapter module.
- `components/apps/camera/Camera.cpp`: app lifecycle plus camera, AI detection,
  FreeRTOS event group, and LVGL update locking.
- `components/apps/camera/app_camera_pipeline.hpp` and
  `app_camera_pipeline.cpp`: C-style firmware service API with documented
  structs and `esp_err_t` return values.
- `components/human_face_detect/CMakeLists.txt` and
  `components/pedestrian_detect/CMakeLists.txt`: model selection, packing, and
  flash/rodata embedding conventions.

## Hardware Driver Components

### 1. Scope / Trigger

Use a standalone component under `components/<device>/` when a hardware module
has protocol code, porting code, and service APIs that may be consumed by more
than one app. This prevents UI code under `components/apps/` from owning UART,
packet framing, checksums, or third-party driver details.

### 2. Signatures

Expose project-owned service headers from `components/<device>/include/` and
return `esp_err_t` from recoverable operations:

```c
esp_err_t as608_service_init(const as608_service_config_t *config);
esp_err_t as608_service_deinit(void);
esp_err_t as608_service_enroll(as608_service_enroll_cb_t callback,
                               void *user_ctx,
                               uint16_t *page_id,
                               uint16_t *score,
                               as608_status_t *status);
esp_err_t as608_service_identify(uint16_t *page_id, uint16_t *score,
                                 as608_status_t *status);
esp_err_t as608_service_match(uint16_t *page_id, uint16_t *score,
                              as608_status_t *status);
esp_err_t as608_service_delete_template(uint16_t page_id,
                                        as608_status_t *status);
```

### 3. Contracts

- `config` owns board-specific runtime choices such as UART port, TX/RX GPIO,
  baud rate, buffer size, timeout, and device address.
- Protocol source imported from a third party goes under `vendor/` with its
  license preserved.
- App/UI code consumes the service header only; it must not include third-party
  protocol headers unless the public result type intentionally exposes one.
- Temporary hardware validation entry points must be gated by Kconfig and
  default off. For AS608, `CONFIG_EXAMPLE_ENABLE_AS608_VALIDATION` appends the
  validation task source and `as608` dependency only when explicitly enabled.
- Validation tasks may live in `main/` when they only orchestrate boot-time
  checks and serial logs. They should call the service API, not vendor protocol
  functions directly.
- For ESP32-P4 AS608 validation, expose regular HP UART ports only
  (`UART1`-`UART4`) in Kconfig. `UART0` remains the console and
  `LP_UART_NUM_0` is not the default external-module validation path.

### 4. Validation & Error Matrix

- Missing `config` -> `ESP_ERR_INVALID_ARG`.
- Invalid GPIO, baud rate, or buffer size -> `ESP_ERR_INVALID_ARG`.
- Operation before successful init -> `ESP_ERR_INVALID_STATE`.
- Device timeout -> `ESP_ERR_TIMEOUT`.
- Fingerprint/template not found -> `ESP_ERR_NOT_FOUND`.
- AS608 no-finger, not-match, and not-found statuses all map to
  `ESP_ERR_NOT_FOUND`; callers that need to distinguish those cases must inspect
  the returned `as608_status_t`.
- AS608 library full -> `ESP_ERR_NO_MEM`.
- Vendor protocol or UART failure -> `ESP_FAIL`.

### 5. Good / Base / Bad Cases

- Good: `components/as608/` wraps AS608 UART and exposes enroll/identify APIs.
- Base: A single app-private helper may stay beside that app if no other app or
  service can reuse it.
- Bad: A page class directly sends UART packets or parses hardware protocol
  frames.

### 6. Tests Required

- At minimum, run `idf.py build` and verify the new component appears in the
  component list.
- For Kconfig-gated validation tools, also build a temporary config with the
  gate enabled, then remove the temporary defaults/build output before commit.
- On hardware, verify init, one enrollment, one successful match, one successful
  identify, one not-match/not-found path, and one no-finger path before wiring
  the service to attendance records.

### 7. Wrong vs Correct

Wrong:

```cpp
// In an LVGL page callback:
uart_write_bytes(UART_NUM_1, raw_as608_command, command_len);
```

Correct:

```c
as608_service_config_t config = {
    .port = {
        .uart_num = UART_NUM_1,
        .tx_io = tx_gpio,
        .rx_io = rx_gpio,
        .baud_rate = AS608_PORT_DEFAULT_BAUD_RATE,
    },
    .address = AS608_SERVICE_DEFAULT_ADDRESS,
};
ESP_ERROR_CHECK(as608_service_init(&config));
```

## AS608 Template Page Ownership

### 1. Scope / Trigger

Use this contract whenever an app binds AS608 fingerprint templates to local
records such as students, employees, or attendance identities. This applies to
any flow where the app persists `page_id` outside the AS608 module.

### 2. Signatures

The hardware validation path may use automatic enrollment:

```c
esp_err_t as608_service_enroll(as608_service_enroll_cb_t callback,
                               void *user_ctx,
                               uint16_t *page_id,
                               uint16_t *score,
                               as608_status_t *status);
```

Record-binding apps must use explicit page ownership:

```c
esp_err_t as608_service_enroll_to_page(as608_service_enroll_cb_t callback,
                                       void *user_ctx,
                                       uint16_t target_page_id,
                                       uint16_t *page_id,
                                       uint16_t *score,
                                       as608_status_t *status);
```

### 3. Contracts

- AS608 stores fingerprint templates by numeric page id. Local services store
  the metadata mapping from page id to user/student identity.
- `as608_get_valid_template_number()` returns a count of valid templates, not
  the next free page id.
- Apps that persist page ids must allocate `target_page_id` from their local
  binding table before enrollment and pass it to
  `as608_service_enroll_to_page()`.
- On success, the returned `page_id` must equal `target_page_id`; treat any
  mismatch as an error.
- Keep `STUDENT_STORE_PAGE_ID_MAX` and AS608 search ranges aligned. The current
  student binding range is `0..299`.

### 4. Validation & Error Matrix

- Target page already bound locally -> reject before touching AS608, return or
  surface a page-occupied error.
- No local free page -> return `ESP_ERR_INVALID_STATE` or an app-level store
  full status before enrollment.
- AS608 no finger / not match / not found -> return `ESP_ERR_NOT_FOUND` and
  inspect `as608_status_t` for the exact sensor status.
- AS608 timeout -> return `ESP_ERR_TIMEOUT`.
- AS608 saved template but local bind failed -> delete the just-saved AS608
  template page as rollback, then report the bind failure.

### 5. Good / Base / Bad Cases

- Good: Fingerprint App allocates an unused local page id, enrolls to that page,
  verifies the returned page, then binds the same page to the selected student.
- Base: A one-shot hardware validation task enrolls with
  `as608_service_enroll()` and immediately verifies the returned page, without
  persisting an identity mapping.
- Bad: A record-binding app calls `as608_service_enroll()` and trusts the AS608
  valid-template count as the next free page. Deletes, residual templates, or
  local/AS608 drift can overwrite an existing user's template.

### 6. Tests Required

- Build with `idf.py build`.
- On hardware, enroll student A, identify A, delete A, enroll student B, and
  identify B; assert B does not map to A's old record.
- Test a locally occupied page by trying to enroll over it; assert the app
  refuses before AS608 writes a template.
- Test AS608 success plus local bind failure; assert rollback deletes the
  target AS608 template page.

### 7. Wrong vs Correct

Wrong:

```c
uint16_t page_id = 0;
as608_get_valid_template_number(&handle, addr, &page_id, &status);
as608_store_feature(&handle, addr, AS608_BUFFER_NUMBER_2, page_id, &status);
student_store_bind_page_id(student_id, page_id, &store_status);
```

Correct:

```c
uint16_t target_page_id = 0;
ESP_ERROR_CHECK(student_app_allocate_free_page(&target_page_id));
ESP_ERROR_CHECK(as608_service_enroll_to_page(cb, ctx, target_page_id,
                                             &page_id, &score, &status));
ESP_ERROR_CHECK(page_id == target_page_id ? ESP_OK : ESP_FAIL);
ESP_ERROR_CHECK(student_store_bind_page_id(student_id, target_page_id,
                                           &store_status));
```

## Code Example

The app installation pattern belongs in `main/main.cpp`:

```cpp
Calculator *calculator = new Calculator();
assert(calculator != nullptr && "Failed to create calculator");
assert((phone->installApp(calculator) >= 0) && "Failed to begin calculator");
```
