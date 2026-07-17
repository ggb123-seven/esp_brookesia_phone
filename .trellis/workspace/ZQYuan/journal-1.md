# Journal - ZQYuan (Part 1)

> AI development session journal
> Started: 2026-06-18

---



## Session 1: Bootstrap project guidelines

**Date**: 2026-06-18
**Task**: Bootstrap project guidelines
**Branch**: `main`

### Summary

Filled backend and frontend Trellis guidelines from the ESP-IDF/LVGL project structure, including real code examples and updated bootstrap checklist.

### Main Changes

- Read `AGENTS.md`, task PRD, spec indexes, shared guides, and the existing
  empty journal before editing.
- Filled all backend spec files with ESP-IDF firmware conventions for directory
  layout, storage/NVS/SPIFFS, error handling, logging, and quality checks.
- Filled all frontend spec files with on-device ESP-Brookesia/LVGL conventions
  for app structure, callbacks, state, type safety, and UI quality.
- Updated backend/frontend spec indexes from pending status to `Filled`.
- Updated the bootstrap PRD checklist to mark backend guidelines, frontend
  guidelines, and code examples complete.

### Git Commits

(No commits yet)

### Testing

- [OK] `rg` found no remaining bootstrap placeholders in `.trellis/spec/`.
- [OK] Verified every guideline file has a `## Code Example` section.
- [OK] `git diff --check` passed; only CRLF normalization warnings were printed.

### Status

[OK] **Guidelines filled; pending commit/archive**

### Next Steps

- Review the spec docs, commit the documentation changes, then archive the
  bootstrap task when ready.


## Session 2: Verify ESP-IDF build conflict

**Date**: 2026-06-18
**Task**: Verify ESP-IDF build conflict
**Branch**: `main`

### Summary

Verified ESP-IDF reconfigure/build with Wi-Fi Remote enabled and archived the build-conflict task.

### Main Changes

(Add details)

### Git Commits

| Hash | Message |
|------|---------|
| `56d1e48` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete

## Session 6: 细化学生指纹 App 子页面与图标规划

**Date**: 2026-06-21
**Task**: 学生指纹模板库绑定接口
**Branch**: `main`

### Summary

根据用户新要求，补充 Fingerprint App UI 规划：主页面只保留 `导入学生指纹`、`识别学生指纹`、`删除学生指纹` 三个入口，点击后分别进入独立子页面；子页面 UI 要简约、大气，并复用 Setting App 的 LVGL 26 键 keyboard + textarea 模式输入学生姓名。另补充 launcher 图标重构要求：指纹 App 图标改为 app-local 静态 PNG/LVGL 资源，视觉风格对齐现有本地 App 图标。

### Main Changes

- 更新 `.trellis/tasks/06-20-student-fingerprint-template-binding/prd.md` 的需求与验收标准。
- 更新该任务的 `design.md`，记录主页面、三个子页面和 launcher 图标资产方案。
- 更新该任务的 `implement.md`，补充子页面实现和 `img_app_fingerprint` 静态图标清单。

### Testing

- [OK] 使用 `rg --hidden --no-ignore` 核对新 UI 与图标规划已写入任务文档。

### Status

[OK] **Planning updated**

### Follow-up

- 用户确认学生姓名输入第一版只用于筛选/定位已导入名单，不支持现场新建学生；已同步关闭任务文档中的开放问题。

### Implementation

- 新增 `components/student_store`，提供学生档案、CSV 持久化、名单导入合并、模板绑定/解绑/查询 API。
- Fingerprint App 改为主页面三入口，加 `导入学生指纹`、`识别学生指纹`、`删除学生指纹` 三个子页面。
- 导入和删除子页面使用学生列表 + 姓名筛选 + LVGL keyboard/textarea，筛选仅针对已导入名单。
- 识别成功后按模板 ID 查询学生姓名、班级、学号；录入成功后自动绑定选中学生；删除成功后清理本地绑定。
- 指纹 App launcher 图标改为 `img_app_fingerprint.png/.c` 静态资产，移除运行时绘制图标逻辑。
- 重新生成 `fingerprint_font_20.c`，覆盖新增 UI 文案和示例名单中文字符。

### Verification

- [OK] `idf.py build`
- [OK] `compile_commands.json` 包含 `student_store.c`、`fingerprint_font_20.c`、`img_app_fingerprint.c`
- [OK] 字体覆盖检查：175 个中文/标点字符，缺失 `NONE`


## Session 3: Optional app trim and ESP32-P4 ECO2 verification

**Date**: 2026-06-18
**Task**: Small maintenance / no active Trellis task
**Branch**: `main`

### Summary

Reduced optional app build surface and fixed the ESP32-P4 ECO2 boot configuration so the project builds cleanly with the current ESP-IDF 5.5.4 environment.

### Main Changes

- Added Kconfig/CMake gating for optional apps and committed the app-trim work as `9050f5d`.
- Added ESP32-P4 ECO2 revision defaults in `sdkconfig.defaults` and synced `sdkconfig`.
- Confirmed the revision options are present:
  - `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`
  - `CONFIG_ESP32P4_REV_MIN_100=y`
- Cleaned generated `build/` artifacts after verification and restored tracked build files to keep the worktree clean.

### Git Commits

| Hash | Message |
|------|---------|
| `9050f5d` | 精简可选应用编译 |
| `83d50e9` | fix: support esp32p4 eco2 config |

### Testing

- [OK] `idf.py build` completed successfully after activating the ESP-IDF 5.5.4 PowerShell environment.
- [WARN] Build still reports `init_sd_ldo_only()` as unused in `main/main.cpp`.
- [INFO] Build reports duplicate `BSP_I2S_NUM` symbol definitions and component update notices.
- [OK] Final `git status --short` was clean after removing generated build output.

### Status

[OK] **Completed and pushed; repository clean at `83d50e9`**

### Next Steps

- For Video Player, SD-card videos still need to be converted to root-level `.mjpeg` files before the app can list/play them.


## Session 4: README_CN ffmpeg Windows guidance

**Date**: 2026-06-19
**Task**: Small documentation fix / no active Trellis task
**Branch**: `main`

### Summary

Updated the Chinese README video-player notes so Windows users do not try Ubuntu-only `sudo apt` commands in PowerShell when preparing MJPEG files for the SD card.

### Main Changes

- Added a Windows PowerShell install path for ffmpeg using `winget install Gyan.FFmpeg`.
- Kept the Ubuntu/Linux `sudo apt update` and `sudo apt install ffmpeg` commands as a separate platform path.
- Added a post-install verification step with `ffmpeg -version`.
- Documented that users may need to reopen PowerShell and add ffmpeg's `bin` directory to `PATH` if the command is not recognized.

### Testing

- [OK] Confirmed local ffmpeg is installed and runnable from its full WinGet package path.
- [OK] Verified the direct `ffmpeg` command failure is caused by `PATH`, not by a missing install.
- [OK] Reviewed `README_CN.md` target section after editing.
- [OK] `git diff --check` passed; only CRLF normalization warnings were printed.
- [SKIP] `idf.py build` was not run because the change is documentation-only.

### Status

[OK] **Documentation updated and checked; pending commit**

### Next Steps

- Commit `README_CN.md` and this journal update when ready.


## Session 3: AS608 最小移植与后续硬件验证规划

**Date**: 2026-06-19
**Task**: AS608 最小移植与后续硬件验证规划
**Branch**: `main`

### Summary

完成 AS608 最小驱动组件移植：新增 components/as608，封装 UART 适配和 as608_service 录入、识别、对比 API；保留 docs/as608_research 资料；idf.py build 通过。补充 AGENTS.md 规则，默认忽略 build 生成物并清理临时文件。更新 backend 规范记录硬件驱动组件边界。创建后续子任务 06-19-as608-hardware-validation，用于真实 AS608 硬件录入、识别、未匹配和无手指路径验证。

### Main Changes

(Add details)

### Git Commits

(No commits - planning session)

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete

## Session 4: 修复 Fingerprint App 中文字体显示

**Date**: 2026-06-20
**Task**: 修复 Fingerprint App 中文字体显示
**Branch**: `main`

### Summary

修复 Fingerprint App 中文乱码和缺字问题：新增本地 NotoSansSC LVGL 字体子集，将 app UI 和 Brookesia launcher stylesheet 切换到 fingerprint_font_20，并通过 idf.py build 与字体覆盖检查验证。

### Main Changes

(Add details)

### Git Commits

| Hash | Message |
|------|---------|
| `cbc4307` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 5: 规划学生指纹模板绑定接口

**Date**: 2026-06-20
**Task**: 规划学生指纹模板绑定接口
**Branch**: `main`

### Summary

记录大学生打卡系统下一步规划：在 AGENTS.md 补充中文字体显示维护规则；创建学生指纹模板库绑定接口子任务，并在 PRD 中明确学生姓名、班级、学号与 AS608 模板 ID 的本地绑定关系，以及第一版通过 SPIFFS/SD 卡 JSON 或 CSV 导入学生名单。

### Main Changes

(Add details)

### Git Commits

| Hash | Message |
|------|---------|
| `96aacfc` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 6: 学生指纹模板绑定接口实现与 SD 卡名单修正

**Date**: 2026-06-21
**Task**: 学生指纹模板绑定接口
**Branch**: `main`

### Summary

实现学生档案与 AS608 指纹模板 ID 的本地绑定服务，接入 Fingerprint App 三个独立子页面，并按用户确认修正名单工作流：老师维护的工作表放 SD 卡根目录 `/sdcard/students.csv`，设备内部 `/spiffs/students.csv` 只保存运行时学生档案和模板绑定，SPIFFS 示例名单仅作兜底初始化。

### Main Changes

- 新增 `components/student_store`，支持 CSV 导入、持久化、按学号/模板 ID 查询、绑定、解绑和删除。
- Fingerprint App 改成 `导入学生指纹`、`识别学生指纹`、`删除学生指纹` 三个入口和三个子页面。
- 导入页面向学生名单筛选和录入，不再让老师直接管理模板编号；姓名输入沿用 LVGL `textarea + keyboard`。
- 识别页只在点击开始识别后提示放置手指，识别成功后显示学生姓名、班级、学号和模板 ID。
- 删除页面向已绑定学生列表，删除 AS608 模板成功后同步解除本地绑定。
- Fingerprint App launcher 图标改为本地静态 PNG/LVGL 资源，并同步更新中文字体子集。
- SD 卡名单优先级修正：开机先加载 SPIFFS 运行时主表，再发现 `/sdcard/students.csv` 时合并导入并保留同学号已有模板绑定；主表不存在时优先用 SD 卡工作表初始化，再用 SPIFFS 示例名单兜底。
- 默认启用 `CONFIG_EXAMPLE_ENABLE_SD_CARD=y`；SD 卡挂载失败只跳过 SD 名单导入和 Video Player，不阻断核心 App 启动。

### Testing

- [OK] `idf.py build` 通过，生成 `build/esp_brookesia_demo.bin` 和 `build/storage.bin`。
- [OK] SD 卡修正后再次 `idf.py build` 通过；app binary `0x5fcae0`，factory app 分区剩余 `0x303520` 字节（33%）。
- [OK] `build/compile_commands.json` 包含 `main.cpp`、`student_store.c`、`img_app_fingerprint.c`、`fingerprint_font_20.c`。
- [OK] 字体覆盖检查通过：Fingerprint App、示例名单和 launcher 相关中文字符 175 个，缺失 `NONE`。
- [OK] `git diff --check` 通过；只打印 CRLF 换行提示。

### Git Commits

| Hash | Message |
|------|---------|
| `3d52b75` | feat: 实现学生指纹模板绑定 |

### Status

[OK] **Completed**

### Next Steps

- 在真实 ESP32-P4 板卡上插入包含 `/sdcard/students.csv` 的 SD 卡，验证 SD 挂载、名单合并导入、录入、识别和删除流程。

## Session 7: 接入 MQ-2 烟雾与可燃气体检测
**Date**: 2026-06-22
**Task**: 接入 MQ-2 烟雾与可燃气体检测
**Branch**: `main`

### Summary

完成 MQ-2 模块的 ESP-IDF 原生移植，第一版按已确认范围仅接入 `DO` 数字告警输出，不引入 Arduino 依赖，也不提前做 `AO` 模拟量和 ppm 标定。实现了独立 `mq2` 组件、启动配置和轮询采样服务，并在真机上通过拧电位器验证了 `NORMAL` / `ALARM` 状态切换与防抖效果。

### Main Changes

- 新增 `components/mq2` 组件，提供 `mq2_service_init()`、`mq2_service_deinit()` 和 `mq2_service_get_snapshot()`。
- 增加 MQ-2 运行配置：`DO GPIO`、告警有效电平、预热时间、确认采样次数、采样周期和任务栈大小。
- 在 `main/main.cpp` 中按 `Kconfig` 开关启动 MQ-2 服务，并把默认配置写入 `sdkconfig.defaults`。
- 采用 3 次连续采样确认切换状态，避免阈值附近抖动导致的频繁误切换。
- 查阅并参考了 GitHub 上的 MQ-2 Arduino 库思路，但移植实现保持为 ESP-IDF 风格服务。

### Testing

- [OK] `idf.py build` 通过，`mq2` 组件成功进入构建并完成链接。
- [OK] 真机串口日志验证通过：预热结束后可输出 `NORMAL` / `ALARM` 状态切换日志。
- [OK] 通过调节 MQ-2 电位器，确认阈值附近会抖动，防抖后状态能稳定回落到 `NORMAL`。
- [OK] DHT11 真机硬件验证通过，串口日志稳定输出温湿度采样，例如 `temperature=27.0C humidity=51.0%`。

### Status

[OK] **Completed**

### Next Steps

- 后续如需接入更精细的烟雾/气体识别，再扩展 `AO` 模拟量采样和标定流程。


## Session 7: 环境检测 App 与 DHT11 RMT 完工检查

**Date**: 2026-06-22
**Task**: 环境检测 App 与 DHT11 RMT 完工检查
**Branch**: `main`

### Summary

完成 DHT11 RMT 读取替换、环境检测 App 仪表盘 UI 重设计，并通过 idf.py build；记录未提交状态下的完工检查结果。

### Main Changes

?????????????????

?????
- ? DHT11 ???? vendor busy-wait/critical section ????? RMT RX ????????????????????? LVGL/MIPI ?????
- ?? DHT11 ? MQ-2 ???????? sdkconfig ? sdkconfig.defaults ????????
- ???? App ??????????? UI???????????????? ?C ????????????????????
- ?????????????????????/???????????????????????????????

?????
- ?? frontend/backend Trellis spec ?????????? ESP_Brookesia app ?????LVGL ??????CMake ?????????????????
- ??? idf.py build???????? build/esp_brookesia_demo.bin?
- ??? components/dht11/vendor/esp-idf-lib-dht ??????? components/dht11/CMakeLists.txt ????? vendor dht.c??? DHT11 ??? esp_driver_rmt?
- ?????????? journal ?? --no-commit ???????????????????????

?????
- ???????? App ??????????????
- ???? DHT11 RMT ?????????? timeout/checksum?????? RMT ???????????


### Git Commits

(No commits - planning session)

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 8: 闪屏原因补充：DHT11 旧读取路径阻塞显示刷新

**Date**: 2026-06-22
**Task**: 闪屏原因补充：DHT11 旧读取路径阻塞显示刷新
**Branch**: `main`

### Summary

补充记录环境检测页面偶发闪蓝屏根因：旧 DHT11 vendor 读取在 critical section 内忙等 GPIO，干扰 LVGL/MIPI 显示刷新；已改用 RMT RX。

### Main Changes

本条补充记录环境检测页面偶发闪蓝屏的排查结论。

闪屏原因：
- 问题最终定位到 DHT11 旧读取路径，不是 MQ-2，也不是 DHT11 供电异常。
- 旧实现调用 vendor `dht_read_float_data()`，读取 DHT11 40 bit 数据时在 critical section 内用 `esp_rom_delay_us()` 忙等 GPIO 电平变化。
- 这会在采样期间短时间关中断/禁止抢占，容易干扰 LVGL/MIPI 显示刷新链路，因此页面表现为偶发闪蓝屏。

排查依据：
- 关闭 MQ-2 后问题仍可疑。
- 打开 MQ-2、关闭 DHT11 后蓝屏消失，因此 MQ-2 服务基本排除。
- DHT11 供电确认无问题，根因集中在旧 DHT11 GPIO 忙等读取时序实现。

处理方案：
- 将 DHT11 读取改为 ESP-IDF RMT RX 捕获脉冲宽度。
- RMT 由硬件记录高低电平持续时间，CPU 只解析采样结果，不再用全局临界区忙等 GPIO。
- 该方案保留 DHT11 与 MQ-2 同时启用，并减少对显示刷新链路的干扰。


### Git Commits

(No commits - planning session)

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 9: 完善课堂课表与设置应用交互

**Date**: 2026-06-23
**Task**: 完善课堂课表与设置应用交互
**Branch**: `main`

### Summary

新增教室课表应用与专用图标，修复中文 launcher 字体覆盖；统一课堂课表和设置页键盘悬浮逻辑；修复设置页 Wi-Fi 连接成功后 SNTP 越界和后台任务访问 LVGL 的重启问题；默认关闭 Music 和 Video app，并完成构建与刷机验证。

### Main Changes

- 新增教室课表 App，补齐专用 launcher 图标与中文字体子集。
- 统一课堂课表与设置页的悬浮键盘交互，输入框点击切换显示/隐藏，完成键收起键盘。
- 修复设置页 Wi-Fi 连接成功后的 SNTP 越界和后台任务直接访问 LVGL 导致的重启。
- 默认关闭 Music Player 和 Video Player，并保留可选开关。

### Git Commits

| Hash | Message |
|------|---------|
| `6f17c13` | (see git log) |

### Testing

- [OK] `idf.py build`
- [OK] `idf.py -p COM3 flash`
- [OK] 设备端已成功烧录并硬复位

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 10: 真实课表服务器中间层接入真实 EAMS 教室占用

**Date**: 2026-07-05
**Task**: 真实课表服务器中间层
**Branch**: `main`

### Summary

完成本地 Python 课表中间层的真实 EAMS 教室资源 provider 验证。服务端可通过人工登录后的未提交会话配置访问 WebVPN/EAMS 教室资源页面，解析指定教室当天占用节次，并按 ESP32 课表 App 现有 JSON 合同输出。

### Main Changes

- 新增 `tools/real_classroom_schedule_server.py`，支持 `/health`、`/classroom-schedule/today`、token 校验、缓存降级、fixture provider、manual-session provider 和 `eams-room-occupancy` provider。
- 新增脱敏 fixture：`tools/fixtures/classroom_schedule_fixture.json`。
- 新增运行说明：`tools/real_classroom_schedule_server.txt`，记录本地运行、自测、人工会话导入和真实 EAMS provider 用法。
- 已确认真实 EAMS 页面可解析 `尔雅103` 在 `2026-07-05` 的占用节次，当前输出为“占用节次”，不是课程名/教师名明细。
- 私密配置保存在 `.local-secrets/eams-session.json`，不提交账号、密码、Cookie、JSESSIONID、token 或完整 WebVPN 会话 URL。

### Testing

- [OK] `python -m py_compile tools/real_classroom_schedule_server.py`
- [OK] `python tools/real_classroom_schedule_server.py --self-test --fixture tools/fixtures/classroom_schedule_fixture.json`
- [OK] `python tools/real_classroom_schedule_server.py --probe-eams-provider --eams-login-file .local-secrets/eams-login.json --session-file .local-secrets/eams-session.json --upstream-timeout-seconds 12`
- [OK] 本地 HTTP 服务 `eams-room-occupancy` provider：`/health` 返回 200，`/classroom-schedule/today` 返回真实占用 JSON，错误 token 返回 401。
- [OK] `git diff --check`

### Status

[OK] **Implementation verified**

### Next Steps

- 如需课程名、教师名和班级明细，需要继续寻找 EAMS 对应课程排课明细入口；当前教室资源页只能稳定提供占用节次。


## Session 11: 真实 EAMS 课程明细解析与占用降级

**Date**: 2026-07-05
**Task**: 真实课表服务器中间层
**Branch**: `main`

### Summary

补充真实 EAMS `stdSyllabus!search.action` 全校开课查询解析：当目标教室和日期能在全校开课结果中匹配到课程排课时，服务输出课程名、教师和教学班；若只有教室资源占用表有记录，则继续降级输出占用节次。

### Main Changes

- `eams-room-occupancy` provider 现在优先分页读取全校开课查询，解析 `contents[lessonId]` 中的教师、星期、节次、周次和教室。
- 新增课程明细解析逻辑，将全校开课表格中的课程名和教学班映射为固件 JSON 的 `name` 和 `group`。
- 保留教室资源占用表兜底，覆盖 EAMS 无课程明细但教室确实被占用的情况。
- 新增 `stdSyllabus` 解析自测，覆盖 HTML 属性顺序、周次匹配、单双周和无明细降级边界。

### Testing

- [OK] `python -m py_compile tools/real_classroom_schedule_server.py`
- [OK] `尔雅103` / `2026-06-29` 真实 probe 返回课程明细：`组织行为学`、`符萌萌`、`班级:会计25-3 会计25-4 会计25-5`。
- [OK] `尔雅103` / `2026-07-05` 真实 probe 降级返回 `第5-7节占用`；已确认教室占用页单元格无详情链接、tooltip 或隐藏明细。

### Status

[OK] **Course detail support added with occupancy fallback**

### Next Steps

- 如后续需要解释非课程占用来源，需要继续寻找 EAMS 的考试、临时借用或活动占用明细入口；当前学生侧菜单未暴露可解析的对应详情接口。


## Session 12: 真实课表中间层完工检查与云服务器暂停点

**Date**: 2026-07-05
**Task**: 真实课表服务器中间层
**Branch**: `main`

### Summary

完成真实课表服务器中间层的本地完工检查，并定位 ESP32 联网后仍显示“离线缓存”的原因：设备已联网，但固件默认请求 `10.34.88.246:8080`，该地址当前 `/health` 与课表接口访问超时，导致课表 App 按设计回退到本地缓存。

### Main Changes

- 复查课表 App 刷新逻辑：`hasNetworkIp()` 只判断 STA 是否拿到 IP；HTTP 请求失败时会调用 `loadCachedSchedule()` 并显示“离线缓存”。
- 确认当前固件默认配置仍为 `CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_HOST="10.34.88.246"`、端口 `8080`、token `change-me`。
- 从本机访问 `http://10.34.88.246:8080/health` 和 `/classroom-schedule/today` 均超时，说明当前问题是服务器地址/公网访问/端口放行未完成，不是 JSON 合同或本地缓存逻辑错误。
- 清理 `python -m py_compile` 生成的 `tools/__pycache__`，避免提交临时产物。

### Testing

- [OK] `python -B -X utf8 tools/real_classroom_schedule_server.py --self-test --fixture tools/fixtures/classroom_schedule_fixture.json`
- [OK] `git diff --check`
- [OK] `git status --short --untracked-files=all` 确认仅剩任务相关新增文件、journal 更新和既有 `.vscode/settings.json` 本地改动。

### Status

[OK] **Local implementation verified; cloud deployment remains**

### Next Steps

- 明天在云服务器上以 `--host 0.0.0.0 --port 8080` 启动真实课表服务，并确认安全组/防火墙放行 TCP 8080。
- 用云服务器公网 IP 或域名从本机验证 `/health` 和 `/classroom-schedule/today`。
- 将固件课表服务器 host/token 配置切到云服务器公网地址和一致 token，重新构建、刷机，并确认 ESP32 不再回退“离线缓存”。

## Session 13: 真实课表中间层告警接收验证与兼容性收口

**Date**: 2026-07-08
**Task**: 真实课表服务器中间层
**Branch**: `main`

### Summary

继续推进真实课表服务器中间层，重点验证新增的 `POST /parent-call-alert/trigger` 告警接收端，并修复本机进程级 curl 验证时发现的 JSON body 兼容性问题。

### Main Changes

- `tools/real_classroom_schedule_server.py` 的告警请求体解码从 `utf-8` 调整为 `utf-8-sig`，兼容 Windows/脚本生成的带 BOM JSON 文件。
- 自测新增带 BOM 告警 JSON 请求，防止后续再次出现同类兼容性问题。
- 重新确认告警接口仍只记录 `reason` 和字段长度，不记录完整 `detail` / `message`、`X-Alert-Token` 或原始请求体。

### Testing

- [OK] `python -B -X utf8 tools/real_classroom_schedule_server.py --self-test --fixture tools/fixtures/classroom_schedule_fixture.json`
- [OK] 本机真实进程级验证 `/health` 返回 200。
- [OK] 本机真实进程级验证 `/classroom-schedule/today` 成功课表响应、错误 token `401 unauthorized`。
- [OK] 本机真实进程级验证 `/parent-call-alert/trigger` 成功返回 `accepted`，错误 `X-Alert-Token` 返回 `401 unauthorized`。
- [OK] `git diff --check`

### Status

[OK] **Local middleware and alert receiver verified**

### Next Steps

- 云服务器上启动中间层并配置公网可访问 host/port、`SCHEDULE_API_TOKEN`、`PARENT_CALL_ALERT_API_TOKEN`、安全组/防火墙。
- 固件端把课表 host/token 和家长电话告警 HTTP transport token 切到云端一致配置后，重新构建刷机并做硬件联调。

## Session 14: Windows 本机课表服务双击启动与登录自启动

**Date**: 2026-07-14
**Task**: 真实课表服务器中间层
**Branch**: `main`

### Summary

将真实课表中间层从云端部署暂停点切换为 Windows 局域网优先方案，复用桌面已有批处理配置，补齐自动 IP、EAMS 会话检查、双击启动和登录自启动。

### Main Changes

- 新增 `tools/windows/start_classroom_schedule_server.ps1`，集中处理 Python、物理网卡 IPv4、配置文件、EAMS 会话、端口和服务启动。
- 修复隧道虚拟网卡抢占默认路由的问题：排除 `Meta Tunnel` 后正确选择 WLAN `10.96.111.246`。
- EAMS 探测改为独立隐藏子进程并设置总超时；失败时清理子进程，不输出 Cookie、JSESSIONID 或 token。
- 新增桌面 `.bat` 模板、本地配置示例和可逆的 Windows Startup 快捷方式安装器。
- 更新 OneDrive 桌面现有启动脚本和配置；本地敏感 token 原值保持不变。

### Testing

- [OK] 两个 PowerShell 脚本通过语法解析。
- [OK] fixture `--check-only` 自动识别 WLAN 地址 `10.96.111.246`。
- [OK] 隔离端口启动完整链路，`/health` 返回成功，`A101` 返回 3 条 fixture 课程。
- [OK] Windows Startup 快捷方式存在，目标、`--autostart` 参数和最小化窗口配置正确。
- [LIMITED] 真实 EAMS 会话探测在总时限内超时，未把网络超时误判为会话过期。

### Status

[OK] **Windows local launcher ready; EAMS reachability still needs confirmation**

### Next Steps

- 在浏览器确认 WebVPN/EAMS 当前是否可访问；若会话已过期，人工登录后更新 `.local-secrets/eams-session.json`。
- 再次双击桌面启动器或运行 `--check-only`，确认 `Session: valid`。
- 服务成功运行后，把 ESP32 课表 host 设置为 `10.96.111.246`，进行构建、刷机和硬件联调。


## Session 10: 修复课表读取与楼宇中文显示

**Date**: 2026-07-16
**Task**: 修复课表读取与楼宇中文显示
**Branch**: `main`

### Summary

恢复真实 EAMS 课表链路，统一查询日期与响应校验，扩展 13 个楼宇映射，修复课程和 dropdown 中文字形及箭头显示，并完成构建、COM3 刷写和设备验收。

### Main Changes

(Add details)

### Git Commits

| Hash | Message |
|------|---------|
| `2ea0f30` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 11: 整合课表服务器启动与 EAMS 会话续期

**Date**: 2026-07-17
**Task**: 整合课表服务器启动与 EAMS 会话续期
**Branch**: `main`

### Summary

实现桌面单入口会话检查与可见 Edge 续期，人工完成滑块后原子更新 storage state，安全重启受管服务器；修复 token 对齐并完成构建、刷机和真实课表验证。

### Main Changes

(Add details)

### Git Commits

| Hash | Message |
|------|---------|
| `f35559c` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 12: 归档课表动态查询相关任务

**Date**: 2026-07-17
**Task**: 归档课表动态查询相关任务
**Branch**: `main`

### Summary

完成课表动态楼宇与教室目录、精确真实查询、固件选择状态和陈旧响应保护的质量收口；服务端自测、mock 日志脱敏、字体覆盖和固件构建通过，并按用户要求归档三个课表相关任务。

### Main Changes

(Add details)

### Git Commits

| Hash | Message |
|------|---------|
| `97b0db7` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete
