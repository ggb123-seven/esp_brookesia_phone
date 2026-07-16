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

## Hosted Wi-Fi Connection Contract

### 1. Scope / Trigger

Use this contract for Wi-Fi enablement, saved credentials, reconnect behavior,
or AP discovery on the ESP32-P4 board. The P4 uses `esp_wifi_remote` and
ESP-Hosted to control an ESP32-C6, so Wi-Fi control RPCs and network data share
the SDIO transport.

### 2. Signatures

The long-lived `WifiConnectionManager` owns these app-facing operations:

```cpp
esp_err_t begin(bool enabled);
bool setEnabled(bool enabled);
bool connectCandidate(const char *ssid, const char *password);
bool startDiscovery(void);
bool endDiscovery(void);
bool getSnapshot(Snapshot *snapshot);
size_t copyScanResults(wifi_ap_record_t *records, size_t capacity,
                       uint32_t *version);
```

Only its manager task may call `esp_wifi_start/stop/connect/disconnect`,
`esp_wifi_set/get_config`, `esp_wifi_set_storage`, or `esp_wifi_scan_*`.
Settings and status-bar code consume `Snapshot`; the snapshot never contains a
password.

### 3. Contracts

- `storage/wifi_en=0` disables connection and retries but retains credentials.
- The first implementation stores exactly one network: the last candidate that
  reached `IP_EVENT_STA_GOT_IP` and was committed successfully.
- Candidate credentials use `WIFI_STORAGE_RAM`. A wrong password, timeout, or
  missing AP must not replace the saved Flash configuration.
- With Wi-Fi enabled and a saved SSID, startup calls `esp_wifi_connect()`
  without requiring the Settings app to be opened.
- Temporary failures retry after 1, 2, 5, 10, then 30 seconds. Authentication
  failures retry no faster than once per 60 seconds.
- A connected station must never start an automatic or periodic scan. Explicit
  discovery disconnects the station, allows the link to settle, performs one
  blocking scan in the manager task, and reconnects the saved network when the
  discovery page is left without a successful replacement.
- The manager never calls LVGL. Settings polls versioned snapshots from an LVGL
  timer and may enqueue commands from callbacks without waiting for Wi-Fi RPCs.
- Logs may contain SSID, state, disconnect reason, and stack watermark. They
  must not contain passwords or complete `wifi_config_t` values.

### 4. Validation & Error Matrix

| Condition | Required behavior |
|-----------|-------------------|
| No saved SSID | Enter `IDLE_NO_CONFIG`; allow explicit discovery |
| Saved AP absent | Enter bounded `RETRY_WAIT`; keep UI responsive |
| Authentication failure | Preserve saved config; apply 60-second retry delay |
| Candidate reaches GOT_IP | Commit candidate to Flash, then publish success |
| Candidate timeout or disconnect | Restore saved Flash config and publish failure |
| Wi-Fi disabled | Stop Wi-Fi, cancel deadlines/discovery, retain saved config |
| Connected user requests refresh | Controlled disconnect, settle, one scan; no concurrent connected scan |
| Stale GOT_IP outside a connecting state | Ignore it; do not publish a false connected state |
| Command/event queue full | Log an error without printing credentials |

### 5. Good / Base / Bad Cases

- **Good**: Reboot with `wifi_en=1` and a saved AP available; the manager gets an
  IP, starts no scan, and SNTP/HTTP traffic does not reset the SDIO transport.
- **Base**: No saved network; Settings shows results only after discovery and a
  successful candidate becomes the sole saved network.
- **Bad**: A Settings task periodically calls `esp_wifi_scan_start()` after
  GOT_IP while SNTP or HTTP is active. Older C6 firmware can report
  `H_SDIO_DRV: Unrecoverable host sdio state` and restart the P4.

### 6. Tests Required

- Build with `idf.py build`; assert no direct Wi-Fi control calls remain outside
  `WifiConnectionManager` in the Settings component.
- Reboot without opening Settings; assert logs show saved-network connect and
  GOT_IP, with no subsequent automatic `Scan start Req`.
- Keep SNTP and a schedule HTTP request active after GOT_IP; assert there is no
  SDIO transport restart, software reset, panic, or watchdog.
- Turn the hotspot off and on; assert retry delays are bounded and GOT_IP
  recovers without UI interaction.
- Try a wrong candidate password; reboot or restore the old AP and assert the
  previous saved network still connects.
- Toggle Wi-Fi off/on and close/reopen Settings during discovery; assert no
  invalid LVGL access and no retry while disabled.

### 7. Wrong vs Correct

Wrong:

```cpp
// A page task scans every few seconds, including while connected.
esp_wifi_scan_start(nullptr, true);
```

Correct:

```cpp
// LVGL only enqueues intent; the manager serializes disconnect and scan.
wifi_manager.startDiscovery();
```
