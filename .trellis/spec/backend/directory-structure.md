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

## Code Example

The app installation pattern belongs in `main/main.cpp`:

```cpp
Calculator *calculator = new Calculator();
assert(calculator != nullptr && "Failed to create calculator");
assert((phone->installApp(calculator) >= 0) && "Failed to begin calculator");
```
