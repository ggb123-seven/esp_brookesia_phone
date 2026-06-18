# Component Guidelines

> How components are built in this project.

---

## Overview

UI components are ESP-Brookesia apps and LVGL object trees. There are two valid
styles:

- Hand-written app UI, as in `Calculator.cpp`, `Game_2048.cpp`, `Camera.cpp`,
  and `VideoPlayer.cpp`.
- Generated SquareLine/LVGL UI exported into `ui/` folders, with app logic
  attached from the app class, as in `components/apps/setting/ui/`.

Keep app lifecycle, state, and event callbacks in the app class. Keep generated
screen construction in generated `ui_*.c` files.

---

## Component Structure

New phone apps should follow the existing `ESP_Brookesia_PhoneApp` pattern:

1. Declare a class in `components/apps/<app>/<AppName>.hpp`.
2. Derive from `ESP_Brookesia_PhoneApp`.
3. Implement at least `init()`, `run()`, `back()`, and `close()`.
4. Declare the app icon with `LV_IMG_DECLARE(img_app_...)`.
5. Construct the base app with name, icon, and `auto_resize_visual_area`.
6. Add the app header to `components/apps/apps.h`.
7. Instantiate and install it in `main/main.cpp`.

Use `back()` to call `notifyCoreClosed()` or return `notifyCoreClosed()` so the
Brookesia shell gets a consistent close event.

## Code Example

The constructor binds the launcher name, icon, and resize behavior:

```cpp
LV_IMG_DECLARE(img_app_music_player);

MusicPlayer::MusicPlayer():
    ESP_Brookesia_PhoneApp("Music Player", &img_app_music_player, true)
{
}
```

---

## Props Conventions

There are no React-style props. Component inputs are C/C++ constructor
arguments, app member variables, `lv_event_t` user data, and config structs.

- Pass app instances to LVGL callbacks through `lv_obj_add_event_cb(..., this)`
  and recover them with `lv_event_get_user_data(e)` or `e->user_data`.
- For C widget/helper APIs, prefer explicit config structs. Example:
  `esp_lvgl_simple_player_cfg_t`.
- Store long-lived widget pointers as class members when they are reused across
  callbacks; keep temporary container widgets local to `run()` when they are not
  needed later.
- Keep UI resource descriptors (`lv_img_dsc_t`, generated images, fonts) owned
  by the app or generated UI module that uses them.

---

## Styling Patterns

- Prefer LVGL style calls close to the object creation code so layout, size, and
  visual state are easy to audit.
- Use file-local macros for repeated dimensions, colors, and fonts. Group the
  macros near the top of the file, as in `Calculator.cpp`.
- For generated or centralized page parameter tables, put explanatory comments
  above the table or `#define XXX_PARAM_LIST`; do not scatter explanations as
  line-end comments for every variable.
- Preserve the existing direct LVGL style API (`lv_obj_set_style_*`,
  `lv_obj_set_size`, `lv_obj_align`) instead of adding a new styling wrapper
  unless multiple apps clearly share the same pattern.
- Keep generated UI styling in generated files unless the change belongs to
  runtime behavior or cannot be represented in the source UI tool.

---

## Accessibility

This is an embedded touchscreen UI, so accessibility means clear touch targets,
legible labels, and predictable navigation rather than web ARIA attributes.

- Keep touch targets large enough for finger interaction on the 1024x600 panel.
- Preserve navigation bar behavior by implementing `back()` and `close()`.
- Avoid hiding essential controls without an obvious way back. Camera and video
  mode switches should leave the user able to return or stop playback.
- Use readable fonts already enabled in `sdkconfig.defaults`; adding a new font
  requires checking binary size and LVGL configuration.

---

## Common Mistakes

- Updating generated `ui/` symbols by hand and then losing the change when UI
  files are re-exported.
- Calling LVGL APIs from Wi-Fi, camera, or video tasks without
  `esp_lv_adapter_lock()`.
- Forgetting to clear/hide generated UI widgets when a mode changes.
- Storing callback state in globals when a class member or callback user data
  would keep ownership clearer.
- Adding an app class but forgetting `apps.h` or `main/main.cpp` installation.
