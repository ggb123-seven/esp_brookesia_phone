# 教室今日课程联网展示 App 实现计划

## Preconditions

- 当前任务只实现 ESP32 App，不实现云服务器真实抓取学校系统。
- 进入实现前需将任务状态切到 `in_progress`。
- 实现前读取相关规范：backend/frontend directory、error handling、quality、logging、component/state 指南。

## Implementation Checklist

1. 配置与构建接入
   - 在 `components/apps/Kconfig.projbuild` 增加 `CONFIG_EXAMPLE_ENABLE_APP_CLASSROOM_SCHEDULE`。
   - 增加服务器 IP、端口、接口路径、token、自动刷新周期、请求超时等 Kconfig 配置。
   - 在 `components/apps/CMakeLists.txt` 保持依赖稳定，补充 `esp_http_client`、`json` 等需要的组件依赖。
   - 将新 App header 加入 `components/apps/apps.h`，并在 `main/main.cpp` 按配置安装 App。

2. App 骨架
   - 新建 `components/apps/classroom_schedule/ClassroomScheduleApp.hpp/.cpp`。
   - 实现 `ESP_Brookesia_PhoneApp` 生命周期：`init()`、`run()`、`back()`、`close()`。
   - 添加 App 图标资产或先复用/生成一个轻量图标资产，确保 launcher 可显示。

3. 本地状态与持久化
   - 使用 NVS 保存教室标识和缓存元信息。
   - 使用 SPIFFS 文件保存最近一次成功课表 JSON。
   - 增加失败路径：NVS 打不开、SPIFFS 读写失败、缓存不存在时都显示可恢复状态。

4. HTTP 请求与 JSON 解析
   - 用 `esp_http_client` 在 worker task 中请求课表接口。
   - 请求 URL 按 Kconfig 的 IP、端口、路径拼接，携带 `classroom` 和 `token`。
   - 不在日志中输出完整 URL 或 token。
   - 用 cJSON 解析约定字段，限制课程数量和字符串长度，避免 unbounded allocation。
   - 解析成功后更新内存模型和 SPIFFS 缓存。

5. UI 实现
   - 手写 LVGL UI：顶部信息区、当前状态区、课程列表区、教室设置输入区、刷新按钮。
   - 覆盖状态：未配置教室、加载中、有课、无课、离线缓存、错误。
   - 中文文本使用有字形覆盖的 LVGL 字体；新增文案后检查字体子集。
   - 后台任务更新 UI 必须使用 `esp_lv_adapter_lock()`。

6. 刷新与关闭
   - App 打开已有教室时自动刷新一次。
   - 手动刷新按钮触发刷新。
   - 使用 LVGL timer 或 App 内定时机制每 10 分钟触发刷新。
   - 防止并发刷新：已有请求在跑时忽略或合并新请求。
   - `close()` 停止 timer，通知 worker 退出并等待完成，再清空 UI 指针。

7. 字体与中文显示
   - 复用现有中文字体方案，或生成 App 本地字体子集。
   - 确认 launcher 标题和 App 内中文文案均有字形覆盖。
   - 不依赖默认 Brookesia/LVGL 字体显示中文。

## Validation

- 运行 `idf.py build`。
- 用模拟 HTTP JSON 服务验证：
  - 正常有课。
  - 今日无课。
  - 请求超时或服务不可达。
  - token 错误/认证失败响应。
  - JSON 缺字段或格式错误。
  - 请求失败时读取 SPIFFS 缓存。
- 硬件验证：
  - 打开 App。
  - 输入教室并保存。
  - 自动刷新和手动刷新。
  - 返回、关闭、重新打开。
  - 断网后显示缓存或错误。

## Risky Files

- `components/apps/CMakeLists.txt`：依赖变更需保持 ESP-IDF configure 期稳定。
- `components/apps/Kconfig.projbuild`：真实 token 不得进入默认值。
- `main/main.cpp`：launcher 中文字体覆盖逻辑不能被破坏。
- 中文字体源文件：新增文案后必须检查字形覆盖和构建体积。

## Rollback Points

- 如果 HTTP/JSON 解析接入导致构建问题，可先保留 App UI 和模拟内存数据，回滚联网层。
- 如果字体体积或生成问题阻塞，可先复用现有中文字体并记录后续拆分字体子集。
- 如果 SPIFFS 缓存读写不稳定，可先保留 NVS 教室保存和在线显示，缓存作为后续独立修复点。
