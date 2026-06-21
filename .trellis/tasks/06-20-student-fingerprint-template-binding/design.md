# 学生指纹模板库绑定接口技术设计

## Architecture

新增独立 ESP-IDF 组件 `components/student_store/`，作为学生档案和 AS608 模板 ID 绑定的本地服务层。它不依赖 LVGL，也不直接调用 AS608 协议；Fingerprint App 和后续考勤 App 都通过它查询和修改学生绑定关系。

推荐文件结构：

```text
components/student_store/
|-- CMakeLists.txt
|-- include/student_store.h
`-- student_store.c
```

`components/apps` 在 CMake `REQUIRES` 中稳定声明 `student_store` 依赖。Fingerprint App 后续只包含 `student_store.h`，不直接读写 CSV。

## Data Model

第一版学生记录：

```text
student_id: string, 设备本地唯一主键
name: string
class_name: string
fingerprint_page_id: uint16_t, 未绑定时使用 STUDENT_STORE_NO_PAGE_ID
enabled: bool
```

固定约束：

- 一个学生最多绑定一个 AS608 模板 ID。
- 一个 AS608 模板 ID 最多绑定一个学生。
- 模板 ID 范围第一版沿用现有 Fingerprint App 和 AS608 basic search 范围：`0..299`。
- 默认目标容量 60 人，组件实现预留到约 128 人，超过后返回容量错误。

## Storage

运行时主表存放在：

```text
/spiffs/students.csv
```

推荐 CSV header：

```csv
student_id,name,class_name,fingerprint_page_id,enabled
```

导入源：

- `/sdcard/students.csv`：老师现场维护的学生名单工作表，SD 卡启用并挂载时作为优先导入源。
- `/spiffs/students_import.csv`：随固件 SPIFFS image 打包的名单种子文件，只在没有 SD 卡名单时兜底初始化。

主表已存在时，服务先加载 `/spiffs/students.csv` 中的既有绑定，再发现 `/sdcard/students.csv` 时按 `student_id` 合并导入，保留同学号既有模板绑定。主表不存在时，服务优先用 SD 卡工作表初始化，其次使用 SPIFFS 种子文件；导入源都不存在时创建只有 header 的空表。所有修改先更新内存表，再完整重写主表。写入时建议使用临时文件再 rename，降低写坏主表的风险。

导入合并规则：

- 以 `student_id` 为唯一键。
- 新 CSV 中存在的学生会更新 `name`、`class_name`、`enabled` 等基础信息。
- 如果新 CSV 没有提供 `fingerprint_page_id` 或字段为空，且主表中同 `student_id` 已有绑定，则保留旧绑定。
- 如果新 CSV 显式提供 `fingerprint_page_id`，按导入值处理，并执行重复模板 ID 检查。
- 新 CSV 中不存在的旧学生从主表移除；其旧模板 ID 不会被自动删除出 AS608 模块，只是不再绑定到学生。

## API Contract

公共 API 使用 `esp_err_t` 表达系统级成败，并用 `student_store_status_t` 输出业务原因，模式类似当前 `as608_service` 返回 `esp_err_t` 同时输出 `as608_status_t`。

建议接口：

```c
#define STUDENT_STORE_NO_PAGE_ID UINT16_MAX

typedef enum {
    STUDENT_STORE_STATUS_OK = 0,
    STUDENT_STORE_STATUS_STUDENT_NOT_FOUND,
    STUDENT_STORE_STATUS_PAGE_ID_UNBOUND,
    STUDENT_STORE_STATUS_DUPLICATE_STUDENT_ID,
    STUDENT_STORE_STATUS_PAGE_ID_OCCUPIED,
    STUDENT_STORE_STATUS_STORE_FULL,
    STUDENT_STORE_STATUS_PARSE_ERROR,
    STUDENT_STORE_STATUS_STORAGE_ERROR,
} student_store_status_t;

typedef struct {
    char student_id[24];
    char name[32];
    char class_name[32];
    uint16_t fingerprint_page_id;
    bool enabled;
} student_store_record_t;

esp_err_t student_store_init(const char *path);
esp_err_t student_store_deinit(void);
esp_err_t student_store_reload(student_store_status_t *status);
esp_err_t student_store_save(student_store_status_t *status);
esp_err_t student_store_import_csv(const char *path, student_store_status_t *status);
esp_err_t student_store_import_csv_merge(const char *path, student_store_status_t *status);
esp_err_t student_store_add(const student_store_record_t *record, student_store_status_t *status);
esp_err_t student_store_get_by_id(const char *student_id, student_store_record_t *record, student_store_status_t *status);
esp_err_t student_store_get_by_page_id(uint16_t page_id, student_store_record_t *record, student_store_status_t *status);
esp_err_t student_store_list(student_store_record_t *records, size_t capacity, size_t *count, student_store_status_t *status);
esp_err_t student_store_bind_page_id(const char *student_id, uint16_t page_id, student_store_status_t *status);
esp_err_t student_store_unbind_student(const char *student_id, uint16_t *old_page_id, student_store_status_t *status);
esp_err_t student_store_unbind_page_id(uint16_t page_id, student_store_status_t *status);
esp_err_t student_store_delete_student(const char *student_id, uint16_t *old_page_id, student_store_status_t *status);
```

错误映射：

- 参数缺失或字段过长：`ESP_ERR_INVALID_ARG`
- 未初始化：`ESP_ERR_INVALID_STATE`
- 学生不存在或模板未绑定：`ESP_ERR_NOT_FOUND`
- 重复学号、模板被占用、容量满：`ESP_ERR_INVALID_STATE`，通过 `status` 区分
- 文件读写失败：`ESP_FAIL` 或 `ESP_ERR_NO_MEM`，通过 `status` 区分

## Data Flow

主界面流程：

1. App 打开后初始化 AS608 service 和 `student_store`，显示三个大按钮：
   - `导入学生指纹`
   - `识别学生指纹`
   - `删除学生指纹`
2. 初始页面只显示模块/名单状态和三个入口，不提示放置手指。
3. 三个入口分别进入独立子页面；子页面结束或点击返回后回到主界面。

UI 页面结构：

- 主页面：顶部显示 `指纹识别` 标题和一行状态摘要，中部使用三个大触控入口。主页面不显示模板编号 spinbox，不显示放置手指提示，也不承载具体录入/删除控件。
- `导入学生指纹` 子页面：顶部标题和返回按钮；中部是学生姓名搜索框和未绑定学生列表；底部或右侧显示选中学生摘要和 `开始录入` 动作。进入录入状态后显示放置手指提示和录入进度。
- `识别学生指纹` 子页面：顶部标题和返回按钮；中部保留大面积状态区域，点击开始识别后才显示放置手指提示；识别成功后展示姓名、班级、学号、模板 ID、分数。
- `删除学生指纹` 子页面：顶部标题和返回按钮；中部是已绑定学生列表和姓名搜索框；选中学生后展示模板 ID，并通过确认动作删除 AS608 模板和本地绑定。

视觉原则：

- 使用简约、大气的 on-device 工具界面：少量页面区块、清晰标题、充足留白、统一圆角和状态色。
- 每个页面只暴露当前流程必须的控件，避免把录入、识别、删除的控件同时放在同一屏。
- 触控目标保持大尺寸，文本必须使用 `fingerprint_font_20` 或更新后的中文字体子集，并检查新增字符覆盖。
- 返回按钮和页面切换逻辑由 `FingerprintApp` 持有，保持 `back()` 与 Brookesia shell 关闭行为一致。

识别流程：

1. 用户点击 `识别学生指纹`。
2. UI 显示“请把手指放到 AS608 指纹模块上”。
3. Fingerprint App 后台任务调用 `as608_service_identify()` 获取 `page_id` 和 `score`。
4. `student_store_get_by_page_id(page_id, ...)` 查询学生档案。
5. 找到学生时 UI 显示姓名、班级、学号、模板 ID、分数。
6. 未绑定时 UI 显示“模板未绑定学生”，不生成考勤记录。

录入绑定流程：

1. 用户点击 `导入学生指纹`。
2. Fingerprint App 从 `student_store_list()` 中列出未绑定模板的学生，显示姓名、班级、学号。
3. 页面提供学生姓名输入框；输入框仅用于筛选/定位已导入名单中的学生。
4. 学生姓名输入使用与 Setting App Wi-Fi 密码页一致的模式：
   - `lv_textarea_create()` 创建输入框。
   - `lv_keyboard_create()` 创建底部 26 键键盘。
   - 通过 `lv_keyboard_set_textarea()` 将键盘绑定到输入框。
   - 参考 `AppSettings::onKeyboardScreenSettingVerificationClickedEventCallback()` 处理确认键。
5. 如果筛选结果为空，UI 显示未找到学生，并提示先更新/导入名单；第一版不提供现场新建学生入口。
6. 老师选择需要录入指纹的学生。
7. UI 进入录入状态后，才显示“请把手指放到 AS608 指纹模块上”。
8. Fingerprint App 调用 `as608_service_enroll()`。
9. 录入成功后调用 `student_store_bind_page_id(student_id, page_id, ...)`。
10. 如果模板 ID 已被其他学生占用，UI 提示冲突，不覆盖。

识别提示流程：

1. App 打开后的准备状态只显示模块/名单状态和三个操作入口，不提示放置手指。
2. 用户点击“识别指纹”后，UI 才显示“请把手指放到 AS608 指纹模块上”。
3. Fingerprint App 后台任务调用 `as608_service_identify()`。
4. 识别完成后恢复结果状态或准备状态。

删除流程：

1. 用户点击 `删除学生指纹`。
2. UI 显示已绑定指纹的学生列表，或提供姓名输入框筛选学生。
3. 老师选择学生并确认删除。
4. Fingerprint App 调用 `as608_service_delete_template(page_id, ...)` 删除 AS608 模板。
5. 删除 AS608 模板成功后调用 `student_store_unbind_page_id(page_id, ...)`。
6. 删除学生档案时只解除本地绑定并返回旧模板 ID；是否继续删除 AS608 模板由 UI 二次确认。

## Launcher Icon

当前 Fingerprint App 图标由 `FingerprintApp.cpp` 中 `getLauncherIcon()` 运行时写入 32x32 像素数据，这和其他本地 App 使用 `assets/img_app_*.png` + LVGL `.c` 静态资源的方式不一致。

重构方案：

1. 新增 `components/apps/fingerprint/assets/img_app_fingerprint.png` 作为源图标。
2. 生成配套 `components/apps/fingerprint/assets/img_app_fingerprint.c`，导出 `img_app_fingerprint`。
3. 在 `FingerprintApp.cpp` 中改为：

```cpp
LV_IMG_DECLARE(img_app_fingerprint);

FingerprintApp::FingerprintApp():
    ESP_Brookesia_PhoneApp("指纹识别", &img_app_fingerprint, true),
    ...
{
}
```

4. 删除运行时图标缓冲区、`s_icon_data`、`s_icon_dsc`、`s_icon_ready` 和 `getLauncherIcon()` 相关逻辑。

图标视觉应仿照现有本地 App：

- 圆角方形彩色背景。
- 底部有轻微压暗阴影，和 Settings/Calculator/Camera 等图标保持同一 launcher 观感。
- 中心使用简化白色或浅色指纹符号，线条不要过细，保证 32px launcher 缩放后仍清楚。
- 不使用复杂渐变、文字或照片素材。

## Compatibility

- 不修改 `managed_components/`。
- 不替换现有 `components/as608` vendor driver。
- 不在 UI 回调中做文件 I/O；名单导入、保存和查询应在后台 task 或打开 App 时的短路径中完成。
- 新增中文 UI 文案必须更新 `components/apps/fingerprint/fingerprint_font_20.c` 字体子集。
- 运行时绑定主表仍保存在 SPIFFS，确保断电重启后可恢复；老师维护的名单工作表优先从 SD 卡导入。工程默认启用 SD 卡挂载能力，但挂载失败只跳过 SD 名单导入和 Video Player，不阻断核心 App 启动；SD 卡未挂载时使用 SPIFFS 种子文件兜底。
- Fingerprint App 的“模板库”呈现应转为“学生名单/待录入学生”，模板编号作为结果字段或诊断信息展示，不作为老师的主要录入入口。
- Fingerprint App 内的学生姓名输入键盘应复用 Setting App 的 LVGL keyboard 行为和尺寸思路，而不是引入新键盘组件或第三方输入法。
- 第一版学生姓名输入只用于名单筛选；新增学生必须通过 CSV 名单导入完成，避免在设备端只输入姓名导致缺学号、缺班级或重名冲突。
- Fingerprint App 的 launcher 图标应和其他 App 一样使用 app-local 静态资产，不继续在业务代码里运行时绘制像素图标。

## Trade-offs

- CSV 优先于 JSON：老师可用 Excel 维护名单，字段固定，固件端解析更轻；代价是需要限制逗号/换行或实现简单转义。
- 第一版不支持多模板：实现和 UI 都更简单；代价是不能同时录入左右手，后续可把单个 `fingerprint_page_id` 扩展为模板列表。
- 不复制 GitHub Arduino 库：避免引入 Arduino 依赖、许可证风险和与现有 AS608 driver 的重复；代价是需要自己实现少量 CSV 和索引逻辑。

## Rollback

- 如果 `student_store` 接入 Fingerprint App 后出现问题，可先关闭 UI 中学生显示/绑定入口，保留原有模板 ID 显示。
- `components/student_store` 是独立组件，移除它和 `components/apps` 依赖即可回退到纯 Fingerprint App。
- 不改变 AS608 模板内部数据格式；本地学生表损坏不会影响 AS608 模块内已有模板。
