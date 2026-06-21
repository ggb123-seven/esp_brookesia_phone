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
