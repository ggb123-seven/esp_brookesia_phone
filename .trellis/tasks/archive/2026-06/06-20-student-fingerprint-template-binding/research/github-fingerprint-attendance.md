# GitHub 指纹考勤调研

## 调研时间

2026-06-21

## 下载位置

候选项目已浅克隆到短临时目录，避免当前仓库深路径触发 Git pack 写入问题：

```text
%TEMP%\fp-gh-research
```

这些下载仅作为调研输入，不直接纳入本仓库源码。后续若确实复制第三方代码，必须保留许可证并单独确认兼容性。

## 候选项目

### `heronet/esp32s3-attendance`

- URL: https://github.com/heronet/esp32s3-attendance
- 本地 commit: `8cbc447`
- 许可：仓库 `LICENSE` 是 GNU GPL。不能直接复制进当前 Apache/Espressif 风格工程；只能参考思路。
- 有价值点：
  - 使用 SPIFFS 保存本地考勤 CSV。
  - 考勤记录字段类似 `date,student_id,status,synced`，适合作为后续考勤记录文件参考。
  - 有离线记录后批量同步 Google Sheets 的流程。
- 不适合直接移植：
  - Arduino/PlatformIO 项目，不是 ESP-IDF 组件。
  - 其 `addAttendance()` 直接把 `fingerprintID` 当 `studentId`，没有独立学生档案映射层。
  - GPL 许可证不适合复制代码。

### `damilarelekanadekeye/EMBEDDED-An-IoT-Based-Fingerprint-Attendance-System-for-Educational-Institutions`

- URL: https://github.com/damilarelekanadekeye/EMBEDDED-An-IoT-Based-Fingerprint-Attendance-System-for-Educational-Institutions
- 本地 commit: `5ad35c1`
- 许可：MIT。
- 有价值点：
  - 明确面向教育机构指纹考勤。
  - 有 `fingerprintID -> matric number` 的映射思路。
  - 有 Firebase 端注册学生记录、删除学生记录和按课程记录考勤的业务流程。
  - 使用 EEPROM 保存用户签到状态，说明离线状态缓存是必要边界。
- 不适合直接移植：
  - Arduino + Firebase + 16x2 LCD 架构，与当前 ESP-IDF/LVGL 离线优先目标不同。
  - 学号映射是硬编码数组，不适合当前需要 SPIFFS/SD 卡名单导入的目标。
  - 加密和云端报表超出当前子任务范围。

### `JOYBOY-3/UniversalFingerprint`

- URL: https://github.com/JOYBOY-3/UniversalFingerprint
- 本地 commit: `2d5f5f9`
- 许可：MIT。
- 有价值点：
  - 提供指纹模板容量、数据库扫描、空槽查找、模板 ID 校验和错误枚举等封装思路。
  - 对当前任务的启发是：本地学生绑定层应区分“学生表记录”和“传感器模板槽位”，并显式处理槽位占用、空槽、重复 ID。
- 不适合直接移植：
  - 基于 Arduino `Adafruit_Fingerprint`，当前项目已有 C 版 `components/as608` 和 vendor driver。
  - 当前子任务主要是学生元数据绑定，不需要替换底层 AS608 driver。
- 可后续拆分：
  - 如果需要自动选择空模板槽、读取 AS608 容量或扫描已占用模板，应另开任务扩展 `as608_service`，不要混入学生绑定接口第一版。

### `JOYBOY-3/A.R.I.S.E-Firmware`

- URL: https://github.com/JOYBOY-3/A.R.I.S.E-Firmware
- 本地 commit: `a94009c`
- 许可：README 声称 MIT，但当前浅克隆根目录没有 `LICENSE` 文件；直接复制代码前需要进一步确认。
- 有价值点：
  - 提到 AS608/R307 兼容、串口管理命令、离线队列和 roll ID 映射。
  - README 中“每个学生两个指纹”的设计可作为后续多模板扩展参考。
- 不适合当前直接移植：
  - Arduino 单文件固件形态，和当前 Brookesia/LVGL App 生命周期不匹配。
  - 第一版 PRD 已收敛为一个学生一个模板 ID，多模板暂缓。

## 结论

第一版不应下载第三方工程并整体移植。更稳的做法是：

1. 继续使用本仓库已有 `components/as608`，不替换底层 driver。
2. 新增 `components/student_store`，实现 ESP-IDF 风格的学生档案和模板 ID 绑定服务。
3. 学生表使用 CSV 持久化，字段包含 `student_id,name,class_name,fingerprint_page_id,enabled`。
4. 借鉴开源项目的业务边界：
   - 不把模板 ID 直接等同于学生 ID。
   - 明确区分未绑定、重复学生、重复模板、存储失败。
   - 为未来考勤记录保留 `status/synced` 等字段，但当前子任务只做学生绑定。
5. 如后续需要自动分配空模板槽或扫描 AS608 模板库，再扩展 `as608_service`，参考 `UniversalFingerprint` 的容量/槽位管理思路重新用本项目 C API 实现。

## 影响当前规划的决策

- 许可安全：只把 MIT 项目的设计思路写入规划；不复制 GPL 项目的源码。
- 存储格式：CSV 比 JSON 更贴近已调研项目和老师批量名单维护场景，也更容易用 Excel 编辑。
- 第一版范围：做学生绑定服务和 Fingerprint App 显示/绑定集成，不做云端同步、加密、报表或多模板。
