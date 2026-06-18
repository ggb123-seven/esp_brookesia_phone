# Directory Structure

> How frontend code is organized in this project.

---

## Overview

This project has no web frontend. Treat "frontend" as the on-device UI layer:
ESP-Brookesia phone apps, LVGL screens, SquareLine-exported UI files, icons,
fonts, and user interaction callbacks.

UI code lives mainly under `components/apps/`. Generated dependency UI under
`managed_components/` is not project-owned.

---

## Directory Layout

```text
components/apps/
|-- apps.h                         # Aggregates app class headers for main.cpp
|-- CMakeLists.txt                 # Builds all app .c/.cpp files recursively
|-- calculator/
|   |-- Calculator.hpp/.cpp        # Hand-written LVGL calculator app
|   `-- assets/                    # Generated LVGL image C files and PNG source
|-- game_2048/
|   |-- Game_2048.hpp/.cpp         # Hand-written LVGL game app
|   `-- assets/
|-- music_player/
|   |-- MusicPlayer.hpp/.cpp
|   |-- gui_music/                 # LVGL music demo UI sources/assets
|   `-- assets/
|-- setting/
|   |-- Setting.hpp/.cpp           # App logic and callbacks
|   |-- app_sntp.c/.h              # Settings support service
|   |-- ui/                        # SquareLine exported screens/images/fonts
|   `-- assets/
|-- camera/
|   |-- Camera.hpp/.cpp            # App lifecycle and UI glue
|   |-- ui/                        # Camera LVGL exported screens/components
|   |-- app_video.*                # Camera/video service boundary
|   `-- app_*_detect.*             # UI-facing AI detection wrappers
`-- video_player/
    |-- VideoPlayer.hpp/.cpp
    |-- esp_lvgl_simple_player/    # Reusable LVGL video player widget
    `-- assets/
```

---

## Module Organization

- Each phone app gets a folder under `components/apps/<app>/` with one primary
  class header/source pair.
- `components/apps/apps.h` includes app headers that are installed from
  `main/main.cpp`. Add new app headers there when the app should appear in the
  launcher.
- Generated UI files stay below an app-local `ui/` folder. For settings, the
  generated source list is in `components/apps/setting/ui/CMakeLists.txt`.
- App-specific icons and images stay under the app's `assets/` or generated
  `ui/images/` folder. Keep PNG source files next to generated LVGL `.c` image
  files when both exist.
- C support modules that feed UI widgets, such as camera frame capture or the
  simple video player, remain under the owning app folder unless multiple apps
  actually share them.

---

## Naming Conventions

- App classes use PascalCase and usually match the visible app name:
  `Calculator`, `MusicPlayer`, `Game2048`, `Camera`, `AppSettings`,
  `AppVideoPlayer`.
- Generated SquareLine symbols use the `ui_` prefix and screen names such as
  `ui_ScreenSettingWiFi` or `ui_ButtonCameraShotBtn`. Do not rename generated
  symbols manually.
- LVGL event callbacks are static functions on the app class when they need app
  state, e.g. `onScreenLoadEventCallback()` or
  `onSliderPanelLightSwitchValueChangeEventCallback()`.
- UI constants use uppercase macros grouped near the top of the source file:
  `KEYBOARD_H_PERCENT`, `LABEL_FONT_BIG`, `HOME_REFRESH_TASK_PERIOD_MS`.
- Icon/image declarations use `LV_IMG_DECLARE(...)` in the app source that owns
  the icon.

---

## Examples

- `components/apps/calculator/Calculator.cpp`: compact hand-written LVGL UI
  with local constants, app lifecycle methods, and `lv_event_get_user_data()`.
- `components/apps/setting/Setting.cpp`: integration of generated screens,
  callbacks, NVS-backed UI state, Wi-Fi tasks, and LVGL locking.
- `components/apps/camera/Camera.cpp`: camera UI plus cross-task frame updates.
- `components/apps/video_player/VideoPlayer.cpp`: app-level UI that wraps a
  reusable LVGL video player widget.

## Code Example

App headers expose the same lifecycle shape:

```cpp
class Camera: public ESP_Brookesia_PhoneApp {
public:
    bool run(void);
    bool back(void);
    bool close(void);
    bool init(void) override;
};
```
