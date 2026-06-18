# Database Guidelines

> Database patterns and conventions for this project.

---

## Overview

This firmware has no SQL database, ORM, migrations, or server persistence
layer. Persistent data is stored through ESP-IDF facilities:

- NVS for small key/value settings, as used by `components/apps/setting/Setting.cpp`
  and `components/apps/game_2048/Game_2048.cpp`.
- SPIFFS for packaged media under `spiffs/`, mounted by `bsp_spiffs_mount()` in
  `main/main.cpp`.
- SD card files when `CONFIG_EXAMPLE_ENABLE_SD_CARD` is enabled, mainly for
  MJPEG video playback.
- Optional model storage in flash rodata, flash partitions, or SD card through
  `components/human_face_detect/Kconfig` and `components/pedestrian_detect/Kconfig`.

Do not introduce a database abstraction unless the firmware actually gains a
new persistent storage mechanism.

---

## Query Patterns

- NVS reads and writes should stay localized to the app that owns the setting.
  `AppSettings::loadNvsParam()` and `AppSettings::setNvsParam()` own settings
  such as volume and brightness; `Game2048::init()` persists the best score.
- Open an NVS namespace with `nvs_open()`, read/write with typed NVS APIs, then
  call `nvs_commit()` after writes. Always log `esp_err_to_name(err)` on
  failures.
- When a key is missing, initialize it explicitly to a default value and commit
  the default. This is the current pattern in `Setting.cpp` and `Game_2048.cpp`.
- File enumeration belongs to the feature that consumes the files. The video
  player searches SD card media in `AppVideoPlayer::searchMideaFiles()` instead
  of creating a global media index.

## Code Example

Use the local NVS pattern: open, write, commit, and log named errors.

```cpp
err = nvs_set_i32(my_handle, key.c_str(), value);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error (%s) setting %s", esp_err_to_name(err), key.c_str());
    return false;
}
err = nvs_commit(my_handle);
```

---

## Storage Layout

- The partition table is checked in at `partitions.csv` and currently defines:
  `nvs`, `phy_init`, `factory`, and `storage` (SPIFFS).
- SPIFFS content is generated from the repository `spiffs/` directory by
  `spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)` in
  `main/CMakeLists.txt`.
- AI models are packed at build time by each model component's `pack_model.py`.
  Depending on Kconfig, the packed `.espdl` output is embedded as aligned
  binary rodata or flashed to a named model partition.
- `sdkconfig.defaults` is the minimal checked-in configuration baseline.
  Avoid hand-editing generated `sdkconfig.old` or generated files under
  `build/config/`.

---

## Naming Conventions

- NVS keys use uppercase descriptive macros when shared inside a module, for
  example `NVS_BEST_SCORE` in `Game_2048.cpp`.
- Settings maps use string keys that match the NVS key names so UI state and
  persistence stay easy to trace.
- Partition names are short lower_snake_case labels such as `storage`,
  `human_face_det`, and `pedestrian_det`.
- Runtime file paths should use the mounted filesystem path expected by the
  owning BSP/component. Do not hard-code host paths.

---

## Common Mistakes

- Treating `sdkconfig` or `build/config/sdkconfig.*` as the configuration
  source of truth. Put reusable defaults in `sdkconfig.defaults`.
- Adding new packaged media without placing it under `spiffs/` or without
  checking partition size in `partitions.csv`.
- Writing NVS values without committing them.
- Logging Wi-Fi passwords or other credentials while debugging settings.
  Existing Wi-Fi code currently logs SSID/password during connect events; avoid
  expanding that pattern and remove it when touching related code.
- Editing files under `managed_components/` to change persistence behavior.
  Wrap or configure dependencies from project code instead.
