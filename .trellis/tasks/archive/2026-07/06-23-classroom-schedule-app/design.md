# 教室今日课程联网展示 App 技术设计

## Scope

当前任务只实现 ESP32-P4 端 Brookesia Phone App。云服务器真实抓取学校 WebVPN/EAMS 的逻辑不在本任务内；本任务仅依赖一个已约定的课表 JSON 接口，开发和验证阶段可用模拟服务。

## Architecture

- App 目录：`components/apps/classroom_schedule/`
- App 类：`ClassroomScheduleApp : public ESP_Brookesia_PhoneApp`
- 生命周期：实现 `init()`、`run()`、`back()`、`close()`。
- 后台请求：App 打开、点击刷新或定时刷新时，通过 FreeRTOS worker task 请求课表接口，避免阻塞 LVGL。
- UI 更新：worker task 解析结果后，通过 `esp_lv_adapter_lock()` 更新 LVGL 对象。
- 持久化：使用 NVS 保存教室标识和缓存元信息。
- 缓存：最近一次成功获取的课表 JSON 本体保存到 SPIFFS 文件；请求失败时读取 SPIFFS 缓存展示，并标注离线缓存状态。
- 配置：服务器 IP、端口、接口路径和 token 通过 `Kconfig` 编译期配置，避免在源码中硬编码。

## Data Flow

1. App 启动时读取 NVS 中保存的教室标识。
2. 如果没有教室标识，显示教室设置状态。
3. 如果已有教室标识，显示加载状态并启动 worker task 请求接口。
4. 请求携带 `classroom` 和 `token`，并按当前日期查询今日课表。
5. worker task 接收 JSON，解析为本地课程数组。
6. 请求成功后更新 SPIFFS 缓存和 NVS 缓存元信息。
7. UI 展示日期、教室名、刷新时间、当前/下一节状态和全天课程列表。
8. 请求失败时，如果存在缓存，显示缓存课表并标注“离线缓存/上次更新”；如果没有缓存，显示可恢复错误状态，允许用户修改教室或重新刷新。

刷新策略：

- App 打开并已有教室标识时自动刷新一次。
- 用户点击刷新按钮时立即刷新。
- App 保持打开时，每 10 分钟自动刷新一次。
- 如果上一轮请求仍在进行，新的刷新请求应被忽略或合并，避免并发请求同一组 UI 状态。

## JSON Contract

MVP 接口建议返回：

```json
{
  "date": "2026-06-23",
  "classroom": "A101",
  "classroom_name": "博学楼 A101",
  "updated_at": "2026-06-23T07:30:00+08:00",
  "courses": [
    {
      "start": "08:00",
      "end": "09:40",
      "name": "高等数学",
      "teacher": "张老师",
      "group": "软件工程 1 班"
    }
  ]
}
```

必需字段：`date`、`classroom`、`courses[].start`、`courses[].end`、`courses[].name`。

可选字段：`classroom_name`、`updated_at`、`courses[].teacher`、`courses[].group`。

## UI Design

UI 设计按“教室显示屏”场景处理，目标是让路过的人一眼看到当前教室今天什么时间段上什么课。

- 顶部信息区：标题“教室课表”、当前教室名称/标识、日期、刷新按钮。
- 当前状态区：突出显示“当前课程”“下一节课”“今日无课”或错误状态。
- 课程列表区：按时间顺序显示每节课，左侧为时间段，右侧为课程名，次要行显示教师/班级等可选信息。
- 设置入口：提供修改教室标识的输入框和保存按钮。
- 加载状态：显示正在获取课表，禁用重复刷新。
- 缓存状态：显示最近一次成功课表，并用明显标签说明“离线缓存”和上次更新时间。
- 错误状态：展示未联网、请求失败、认证失败、解析失败、无数据等中文提示，并保留刷新/修改教室操作。

## Visual Direction

- 使用手写 LVGL UI，避免引入新的生成 UI 工具链。
- 保持 1024x600 面板可读性：大号当前状态，中等字号列表，触控按钮高度不小于现有 App 常用尺寸。
- 中文文本必须使用 App 本地或项目可复用中文字体子集。
- 颜色不延续单一深蓝/紫色主题；建议采用深色背景、浅色文字、绿色/黄色/红色状态色，并用中性面板区分信息层级。

## State Model

- `NO_CLASSROOM`：未配置教室，提示输入。
- `IDLE`：已有教室，等待刷新。
- `LOADING`：正在请求课表。
- `READY`：有有效课表数据。
- `CACHED`：当前显示最近一次成功课表，刷新请求失败或离线。
- `EMPTY`：接口成功但当天无课。
- `ERROR`：请求、认证、解析或配置错误。
- `CLOSING`：App 关闭中，worker 不再更新 UI。

## Implementation Boundaries

- 设备端只解析约定 JSON，不解析学校 WebVPN/EAMS HTML。
- token 不写入运行日志，不输出完整 URL。
- Kconfig 默认值不得包含真实 token。
- SPIFFS 缓存文件只保存服务器返回的简化课表 JSON，不保存学校账号、Cookie 或 WebVPN 会话。
- worker close path 必须可等待退出，避免关闭 App 后访问已释放 UI 对象。
- 如果接口暂时只支持 HTTP，ESP32 端仍需携带 token；后续有域名/HTTPS 后可升级传输安全。

## Tradeoffs

- 服务器中间接口降低 ESP32 端复杂度，但需要服务器常驻。
- 设备端输入教室标识提升复用性，但增加 NVS 和输入 UI。
- SPIFFS 缓存比 NVS 更适合保存 JSON 本体，但需要处理文件读写失败路径。
- MVP 不做服务器真实抓取，能先把 App、接口合同和设备端状态机打稳。
