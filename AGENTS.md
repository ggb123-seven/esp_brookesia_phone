<!-- TRELLIS:START -->
# Trellis Instructions

These instructions are for AI assistants working in this project.

This project is managed by Trellis. The working knowledge you need lives under `.trellis/`:

- `.trellis/workflow.md` — development phases, when to create tasks, skill routing
- `.trellis/spec/` — package- and layer-scoped coding guidelines (read before writing code in a given layer)
- `.trellis/workspace/` — per-developer journals and session traces
- `.trellis/tasks/` — active and archived tasks (PRDs, research, jsonl context)

If a Trellis command is available on your platform (e.g. `/trellis:finish-work`, `/trellis:continue`), prefer it over manual steps. Not every platform exposes every command.

If you're using Codex or another agent-capable tool, additional project-scoped helpers may live in:
- `.agents/skills/` — reusable Trellis skills
- `.codex/agents/` — optional custom subagents

Managed by Trellis. Edits outside this block are preserved; edits inside may be overwritten by a future `trellis update`.

<!-- TRELLIS:END -->

## Local Working Tree Hygiene

- Treat `build/` as generated ESP-IDF output. Ignore it when reviewing source
  changes, status, diffs, and commit plans unless the user explicitly asks to
  inspect build artifacts.
- Delete temporary research, download, or experiment files after use unless
  they are intentionally kept as project documentation or source inputs.

## 固件验证与提交流程

- 每次完成代码修改并确认 `idf.py build` 构建验证无误后，直接执行刷机更新固件。
- 刷机完成后询问用户设备现象是否正常；用户确认正常后，再简要总结本次改动。
- 用户确认现象正常后，将本次改动提交到本地 Git，并继续推送到 GitHub。
- 如果推送到 GitHub 失败，自动重试推送，最多重试 10 次；若 10 次后仍失败，明确告知失败原因和本地提交状态。

## 新模块与外设移植规则

- 每次向当前代码框架移植新的模块或外设前，先在 GitHub 搜索相关优秀实现或官方/社区示例；优先下载到临时位置研究，使用后删除临时文件，除非用户要求保留为项目资料。
- 若找到的代码可以直接适配 ESP-IDF 和当前项目结构，则在理解其许可证和接口后再移植；若无法直接移植，则只借鉴初始化流程、时序处理、错误恢复和测试方法等思路。
- 每移植一个外设，都需要同步编写一个最小测试函数或验证入口，用于独立确认硬件通信、初始化和关键读写路径；通常采用串口日志或串口命令协议暴露测试结果。
- 涉及 UART/SPI/I2C 等持续收发、较大数据搬运或高频数据流时，优先采用 DMA 或驱动层异步机制，避免长时间忙等阻塞 UI 和系统任务。
- 涉及脉冲宽度、单总线时序、红外/超声波/温湿度类时序采集或输出时，优先采用 ESP-IDF RMT 外设；如果该场景可结合 Ringbuffer，则优先使用 RMT + Ringbuffer；不要用长时间关中断或 busy-wait GPIO 的方式实现关键时序。

## Trellis 任务命名与记录语言

- 后续新建或更新 Trellis task 时，任务标题、PRD 标题、journal/session 标题和面向用户的任务说明优先使用简体中文。
- 任务目录 slug、命令参数、代码标识符和已有英文专有名词可以保留英文；需要展示给用户时，同时给出清晰的中文名称。
- 现有英文 task 名称在汇报时按中文含义表达，例如 `00-bootstrap-guidelines` 写作“初始化项目开发规范”。

## 中文 UI 文本显示

- 简体中文 UI 文本必须使用包含对应字形的 LVGL 字体渲染。不要依赖
  Brookesia/LVGL 默认字体显示中文标签；缺少字形覆盖时会出现乱码、缺字或方框。
- Fingerprint App 当前使用应用本地字体
  `components/apps/fingerprint/fingerprint_font_20.c`，并在
  `FingerprintApp.cpp` 中将 `fingerprint_font_20` 应用于应用内 UI 文本。
- Launcher 标题 `指纹识别` 也依赖这套字体：`main/main.cpp` 会在 Brookesia
  stylesheet 激活前，用 `fingerprint_font_20` 覆盖 22px 默认字体。调整
  launcher/app 注册代码时要保留这个覆盖逻辑。
- 新增或修改中文文案时，需要同步更新或重新生成 fingerprint 字体子集，确保每个新增
  中文字符都被包含；完工前用 `idf.py build` 和字体覆盖检查验证。
