# 修复课表读取与楼宇中文显示：实施清单

## 1. 运行环境基线

- [x] 在持久化 Edge profile 中恢复 WebVPN/EAMS 登录会话。
- [x] 电脑端查询并展示真实教室课表，等待用户确认内容。
- [x] 用户确认后再把服务 provider 从 fixture 切回 `eams-room-occupancy`。
- [x] 安全核对固件与 Windows 配置的 host、port、path、token 一致性，不输出 token。
- [x] 启动 Windows 课表服务，确认监听 `0.0.0.0:8080`。
- [x] 使用 curl 验证 `/health` 及当前教室/日期查询。
- [x] 检查对应 Windows 防火墙入站规则。

## 2. COM3 联调与根因定位

- [x] 启动 COM3 monitor，抓取 Wi-Fi GOT_IP 和打开课表 App 的日志。
- [x] 确认设备运行时 host、教室、日期、HTTP status、JSON 解析和缓存选择。
- [x] 将根因限制在配置、连通性、鉴权、HTTP、JSON 或缓存中的具体阶段。
- [x] 对比界面选中日期、请求快照日期和服务器响应日期。

## 3. 固件修复

- [x] 在 `ClassroomScheduleApp.cpp` 增加不泄露 token 的请求与结果诊断日志。
- [x] 根据日志修复实际发现的配置、HTTP、JSON 或缓存问题，保持既有接口合同。
- [x] EAMS 会话重定向时快速失败，避免分页扫描长期占锁；固件区分上游会话不可用与 JSON 格式错误。
- [x] 给 fixture 增加固件可选择的 `尔雅楼103` 课程，验证切换日期后响应日期随请求变化。
- [x] 按真实 EAMS 教室占用页扩展固件楼宇表和服务器 building id 映射，覆盖全部 13 个地点。
- [x] 为 dropdown 主控件、弹出 list 和选中项显式应用本地中文字体，并在打开事件中刷新样式。
- [x] 为 dropdown 箭头单独应用包含 `LV_SYMBOL_DOWN` 的 LVGL 字体，消除右侧缺字方框。
- [x] 保持集中楼宇参数表上方说明与实际楼宇列表一致。

## 4. 验证

- [x] 运行字体覆盖检查，确认源码与 mock/fixture 中文字符 `missing=0`。
- [x] 运行服务自测和 curl 正常/鉴权错误检查，确认日志不泄露 token。
- [x] 抽查至少一个非尔雅楼教室的真实 EAMS 请求，确认 building id 路由正确。
- [x] 运行 `git diff --check`，排除 `build/` 和 `.codex/config.toml`。
- [x] 运行 `idf.py build`。
- [x] 将固件刷写到 COM3，并再次抓取运行日志。
- [x] 请用户验证课表内容、dropdown 收起/展开显示、返回和重开 App。

## 5. 用户确认后

- [ ] 更新 Trellis journal/必要规范。
- [ ] 只提交本任务相关源码和 Trellis 记录，排除 `.codex/config.toml`。
- [ ] 不执行 GitHub 推送。

## 风险文件与回滚点

- `components/apps/classroom_schedule/ClassroomScheduleApp.cpp`：联网和 UI 共用生命周期，改动后必须验证关闭/重开及 worker 停止。
- `components/apps/classroom_schedule/classroom_schedule_font_20.c`：只有覆盖检查发现真实缺字时才重新生成，不手工编辑字形数组。
- `sdkconfig` 与设备 NVS：属于本机构建/运行配置，不作为可移植源码提交。
- `main/CMakeLists.txt`、`.trellis/workspace/ZQYuan/journal-1.md` 已有用户改动，除非确认与本任务相关，否则不覆盖。
