# 修复课表读取与楼宇中文显示：技术设计

## 边界

- 固件侧只负责读取当前 STA IP 状态、构造 HTTP 请求、解析既有 JSON 合同、维护匹配查询条件的缓存并更新 LVGL。
- Windows 服务继续由现有启动脚本和 `real_classroom_schedule_server.py` 提供，不改变真实 EAMS 数据合同。
- 本任务允许调整本机未提交构建配置和设备 NVS 中的服务器地址，但不把局域网地址或真实 token 写入提交源码。

## 电脑端真实课表验收

1. 使用持久化 Edge profile 打开 WebVPN/EAMS；过期会话由用户在有界面浏览器中重新登录并完成验证码。
2. 登录后从 EAMS 教室课表页面查询指定教室和日期，提取真实课程字段。
3. 在电脑端以清晰表格展示查询日期、教室、节次、时间、课程、教师和班级，先由用户确认。
4. 用户确认后更新本地会话配置并切换 Windows 中间层到 `eams-room-occupancy`，再让设备请求同一组数据。
5. fixture 只保留为接口回归测试，不参与真实内容验收。

## 诊断数据流

1. Windows 启动器选择物理 WLAN 地址并监听 `0.0.0.0:8080`。
2. curl 先验证 `/health`，再按设备实际 `classroom` 和 `date` 验证课表接口。
3. App 刷新时先确认 STA 已取得 IP，再记录不含 token 的请求目标信息。
4. HTTP 完成后记录 `esp_err_t`、HTTP status 和响应长度；解析失败时记录失败阶段，不记录响应正文。
5. 解析后同时记录请求日期、响应日期和课程数；响应教室或日期不匹配时拒绝展示。
6. 只有服务器 host、教室和日期都与当前查询匹配的缓存才能作为离线回退。
7. EAMS 返回 3xx 登录重定向时立即终止分页扫描并返回会话不可用，不能继续占用请求锁；固件对 502/503 显示重新登录 EAMS 的明确提示。

## Fixture 合同

- 保留 `A101/B202` 以兼容现有中间层自测。
- 增加固件当前可录入的 `尔雅楼103` 记录，复用既有脱敏课程字段。
- fixture provider 继续以请求 `date` 生成响应日期，因此切换日期可直接验证界面、请求和响应是否统一。

## 配置处理

- 运行时请求 host 仍以 App/NVS 保存值为准，Kconfig 仅作为首次默认值。
- 本次刷写前把本机构建配置默认 host 更新为当前 WLAN 地址；若设备 NVS 仍保存旧地址，在 App 中保存当前地址或按必要的最小方式清理该命名空间配置。
- 不把 token 原文输出到终端、日志或 Trellis 文档；只比较是否配置、长度或相等性。

## 字体与 dropdown

- 继续复用 `classroom_schedule_font_20`，不扩大到全量 CJK 字库。
- 固件楼宇参数表与真实 EAMS 教室占用页保持一致，包含 13 个地点；Windows 中间层分别维护显示名、building id 和上游 `buildingname` 参数，并允许本地会话配置覆盖。不能假设上游参数等于显示名，例如知行楼对应“育龙主楼”，博文楼、中和楼和致远楼对应字面值 `null`。
- 显式给 dropdown 的 `LV_PART_MAIN`、弹出 list 的 `LV_PART_MAIN` 和 `LV_PART_SELECTED` 应用本地字体。
- dropdown 的 `LV_PART_INDICATOR` 使用包含 `LV_SYMBOL_DOWN` 的 `lv_font_montserrat_20`，避免中文子集字体继承到箭头后显示缺字方框。
- 在 dropdown 打开事件中重新取得 list 并应用字体，覆盖 LVGL 主题或控件生命周期导致的样式丢失。
- 字体覆盖检查从 C/C++ 字符串、楼宇配置、fixture/mock JSON 中收集中文字符，并与字体文件中的 Unicode 字形注释集合比较。

## 兼容性与回滚

- 不修改 HTTP query 参数或 JSON 字段，现有 mock 和真实服务保持兼容。
- 新增日志只包含 host、port、path、教室、日期、状态和长度，不包含 token/query 完整 URL。
- 若 dropdown 事件样式处理引发 UI 回归，可独立回滚该事件回调，不影响联网修复。
- 若学校调整 EAMS building id，优先通过未跟踪的会话配置覆盖；确认长期变更后再同步固件楼宇表和服务器默认映射。
- 若本机 IP 再次变化，用户仍可通过 App 的服务器输入框更新 NVS，不依赖重新编译。
