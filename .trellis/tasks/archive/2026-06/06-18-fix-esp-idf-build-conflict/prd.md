# Fix ESP-IDF build conflict

## Goal

Resolve the current ESP-IDF build conflict in the project after verifying the relevant behavior against the Espressif documentation MCP.

## Requirements

- Use the current repository state as the source of truth.
- Prefer the Espressif official documentation MCP before making ESP-IDF, component, or sdkconfig-related decisions.
- Fix the build failure observed in `components/apps/setting/Setting.cpp`, where Wi-Fi Remote configuration macros such as `CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM` are missing during compilation.
- Keep the existing optional-app reduction for Calculator and 2048 separate from the Wi-Fi Remote build fix.
- Avoid broad sdkconfig churn; preserve unrelated user configuration.

## Acceptance Criteria

- [ ] `espressif-documentation` MCP is available to the active Codex session before implementation starts.
- [x] The ESP-IDF project can at least complete CMake reconfiguration.
- [x] `idf.py build` either completes successfully or fails only on a clearly unrelated pre-existing issue documented in the task notes.
- [x] Any `sdkconfig` or Kconfig changes are minimal and directly tied to the build conflict.

## Notes

- On 2026-06-18, `codex mcp login espressif-documentation` succeeded using `CODEX_HOME=C:\Users\seven\.codex`.
- The current conversation still exposes only the `matlab` MCP server through tool calls, so a new/reloaded Codex session is required before direct Espressif MCP calls are possible here.
- On 2026-06-18, this session read the local `esp_wifi_remote` component Kconfig/CMake files under `managed_components/espressif__esp_wifi_remote/`. The component Kconfig defines `CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM` and related Wi-Fi Remote defaults, and its CMake file injects the Wi-Fi Remote headers/sources into the `esp_wifi` component when Wi-Fi Remote is enabled.
- `idf.py reconfigure` completed successfully in the ESP-IDF v5.5.4 environment. The configure log included `espressif__esp_wifi_remote` and `Using Hosted Wi-Fi`.
- `idf.py build` completed successfully. `components/apps/setting/Setting.cpp` compiled without the previous missing `CONFIG_WIFI_RMT_*` macro failure, and the final image used `0x6298e0` bytes of the `0x900000` factory app partition, leaving `0x2d6720` bytes free.
- The generated `sdkconfig` and `build/` outputs from verification were cleaned back out of the working tree. No broad sdkconfig churn is part of this task result.
