# 学生指纹模板库绑定接口实施计划

## Checklist

- [x] 新增 `components/student_store/` 组件和 `include/student_store.h`。
- [x] 定义 `student_store_record_t`、`student_store_status_t`、容量/字段长度/未绑定模板常量。
- [x] 实现 CSV 读取：跳过 header，解析 `student_id,name,class_name,fingerprint_page_id,enabled`。
- [x] 实现 CSV 写入：完整重写主表，使用临时文件 + rename。
- [x] 实现内存索引和校验：重复学号、重复模板 ID、模板范围 `0..299`、容量满、字段超长。
- [x] 实现查询接口：全部列表、按学号、按模板 ID。
- [x] 实现修改接口：新增学生、绑定模板、按学生解绑、按模板解绑、删除学生。
- [x] 实现导入接口：优先从 SD 卡 `/sdcard/students.csv` 更新名单，SPIFFS 示例名单只兜底初始化，并按 `student_id` 保留同学号已有模板绑定。
- [x] 更新 `components/apps/CMakeLists.txt`，稳定声明 `student_store` 依赖。
- [x] 将 SD 卡名单工作表作为优先导入源，并让 SD 卡挂载失败时跳过 SD 名单导入和 Video Player，不阻断核心 App 启动。
- [x] 集成 Fingerprint App：
  - [x] App 打开时初始化/加载 `student_store`。
  - [x] 主界面改为三个入口按钮：`导入学生指纹`、`识别学生指纹`、`删除学生指纹`，主界面只保留状态摘要和入口。
  - [x] 为三个入口分别实现独立子页面，并提供返回主页面路径。
  - [x] `导入学生指纹` 页面列出未绑定模板的学生，显示姓名、班级、学号。
  - [x] `导入学生指纹` 页面提供学生姓名输入框，用于筛选/定位学生。
  - [x] 学生姓名输入框第一版只筛选已导入名单；无匹配结果时提示先导入/更新名单，不创建新学生。
  - [x] 学生姓名输入框使用与 Setting App Wi-Fi 密码页一致的 `lv_textarea` + `lv_keyboard` 26 键键盘交互。
  - [x] `识别学生指纹` 页面只在用户点击开始识别后显示放置手指提示，并展示识别结果。
  - [x] `删除学生指纹` 页面面向已绑定学生列表或姓名筛选，不再要求老师直接输入模板编号。
  - [x] App 初始主界面不要提示放置手指；只有点击识别或进入录入步骤后才提示放手指。
  - [x] 识别成功后显示学生姓名、班级、学号；未绑定时显示明确提示。
  - [x] 删除模板成功后解除本地绑定。
  - [x] 录入成功后把 AS608 返回的模板 ID 自动绑定到选中的学生。
- [x] 重构 Fingerprint App launcher 图标：
  - [x] 新增 `components/apps/fingerprint/assets/img_app_fingerprint.png`，风格对齐现有本地 App 图标。
  - [x] 生成 `components/apps/fingerprint/assets/img_app_fingerprint.c`，导出 `img_app_fingerprint`。
  - [x] 在 `FingerprintApp.cpp` 中改用 `LV_IMG_DECLARE(img_app_fingerprint)` 和 `&img_app_fingerprint`。
  - [x] 删除运行时图标绘制逻辑和 `getLauncherIcon()`。
- [x] 如新增中文文案，重新生成或更新 `fingerprint_font_20.c` 并检查字形覆盖。
- [x] 更新或添加示例名单文件，如 `spiffs/students_import.csv`，避免覆盖运行时主表。
- [x] 更新任务文档里的已验证结果。

## Validation Commands

```powershell
idf.py build
```

辅助检查：

```powershell
rg -n "student_store|students.csv|students_import.csv|学号|姓名|班级|未绑定" components spiffs .trellis/tasks/06-20-student-fingerprint-template-binding
rg -n "lv_keyboard_create|lv_textarea_create|lv_keyboard_set_textarea|导入学生指纹|识别学生指纹|删除学生指纹" components/apps/fingerprint components/apps/setting
rg -n "img_app_fingerprint|getLauncherIcon|s_icon_data|LV_IMG_DECLARE\\(img_app_fingerprint\\)" components/apps/fingerprint
rg -n "student_store|fingerprint_font_20" build/compile_commands.json
```

如果更新中文字体子集，执行字体覆盖检查，确保 Fingerprint App 新增中文字符全部存在于 `components/apps/fingerprint/fingerprint_font_20.c`。

## Verified Results

- [x] `idf.py build` 通过，生成 `build/esp_brookesia_demo.bin` 和 `build/storage.bin`。
- [x] `build/compile_commands.json` 包含 `components/student_store/student_store.c`。
- [x] `build/compile_commands.json` 包含 `components/apps/fingerprint/fingerprint_font_20.c`。
- [x] `build/compile_commands.json` 包含 `components/apps/fingerprint/assets/img_app_fingerprint.c`。
- [x] 字体覆盖检查通过：Fingerprint App、示例名单和 launcher 相关中文字符 175 个，缺失 `NONE`。
- [x] SD 卡工作表修正后再次执行 `idf.py build` 通过；`CONFIG_EXAMPLE_ENABLE_SD_CARD=y`，`esp_brookesia_demo.bin` 大小 `0x5fcae0`，factory app 分区剩余 `0x303520` 字节（33%）。
- [x] `build/compile_commands.json` 包含 `main.cpp`、`student_store.c`、`img_app_fingerprint.c`、`fingerprint_font_20.c`。
- [x] `git diff --check` 通过；仅打印 CRLF 换行提示。

## Risky Files

- `components/apps/CMakeLists.txt`
- `components/apps/fingerprint/FingerprintApp.cpp`
- `components/apps/fingerprint/FingerprintApp.hpp`
- `components/apps/fingerprint/assets/img_app_fingerprint.png`
- `components/apps/fingerprint/assets/img_app_fingerprint.c`
- `components/apps/fingerprint/fingerprint_font_20.c`
- `components/student_store/*`
- `main/main.cpp`
- `sdkconfig`
- `sdkconfig.defaults`
- `spiffs/students_import.csv`

## Review Gates

- 不直接复制 GPL 或许可证不完整项目的源码。
- `student_store` 不依赖 LVGL，不调用 AS608 protocol；只负责学生元数据和模板 ID 映射。
- Fingerprint App 不直接读写 CSV；只调用 `student_store` API。
- 存储失败、重复学号、重复模板、未绑定模板必须能被调用方区分。
- 名单重新导入时保留同学号已有绑定，避免老师更新名单后全班重新录指纹。
- Fingerprint App 的主要录入对象是学生姓名/学号，不是模板编号。
- Fingerprint App 主界面必须是三个业务入口：导入学生指纹、识别学生指纹、删除学生指纹。
- 三个业务入口必须分别进入独立子页面，不能继续沿用单页模板编号 spinbox + 三按钮的旧布局。
- 学生姓名输入键盘必须沿用 Setting App 的 LVGL keyboard + textarea 模式。
- 学生姓名输入第一版只用于筛选已导入名单，不能绕过名单导入创建只有姓名、缺学号/班级的学生记录。
- 未点击识别/录入前，UI 不提示用户放置手指。
- 指纹 launcher 图标必须改为本地静态资产，视觉上和 Settings/Calculator/Camera 等 App 图标统一。
- SD 卡未插入或挂载失败时核心功能仍可用，并回退到 SPIFFS 示例名单/本地持久化主表。
- 删除学生档案不会静默删除 AS608 模块模板；删除模板成功后必须清理本地绑定。

## Follow-up Decisions Before Start

- None.
