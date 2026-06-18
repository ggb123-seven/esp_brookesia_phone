# 从 STM32 裸机到 ESP32-P4、FreeRTOS、LVGL、显示屏和摄像头

本文面向已经做过 STM32 裸机开发，但还没有接触过操作系统、ESP32、ESP-IDF、LVGL、显示屏和摄像头接口的开发者。

目标不是把所有 API 背下来，而是先建立正确的整体模型：这个项目为什么这样分目录、程序从哪里开始跑、任务和中断怎么配合、LVGL 是怎么把控件画到屏幕上的、摄像头数据又是怎么从传感器进入内存并显示出来的。

本文结合当前工程讲解，主要路径如下：

```text
.
|-- main/                         # 系统启动、板级初始化、安装应用
|-- components/apps/              # 屏幕上的各个 App：相机、设置、音乐、2048 等
|-- components/human_face_detect/ # 人脸检测模型封装
|-- components/pedestrian_detect/ # 行人检测模型封装
|-- spiffs/                       # 打包进 SPIFFS 分区的媒体资源
|-- managed_components/           # ESP-IDF 组件管理器下载的依赖
|-- sdkconfig.defaults            # 工程默认配置
|-- partitions.csv                # Flash 分区表
`-- CMakeLists.txt                # ESP-IDF 工程入口
```

---

## 1. 先把思维从 STM32 裸机切换过来

### 1.1 STM32 裸机常见模型

如果你之前主要写 STM32 裸机，脑子里大概率是这个模型：

```text
上电复位
  |
  v
SystemInit()
  |
  v
main()
  |
  +-- 初始化时钟/GPIO/UART/SPI/I2C/DMA/定时器
  |
  v
while (1) {
    轮询按键
    处理串口数据
    刷新状态机
    喂狗
}

中断：
USARTx_IRQHandler()
TIMx_IRQHandler()
DMAx_IRQHandler()
```

这种模型里，`while (1)` 是主世界，中断是少量异步入口。你通常会自己规划：

- 哪个外设什么时候初始化
- 哪个中断优先级更高
- 哪个变量要不要加 `volatile`
- 哪个状态机每 1 ms 跑一次
- 哪些代码不能在中断里做

### 1.2 ESP-IDF + FreeRTOS 的模型

ESP-IDF 工程不是从你写的 `main()` 开始，而是从 ESP-IDF 的启动流程开始。系统会先做芯片启动、Flash、PSRAM、调度器等初始化，然后调用你提供的：

```cpp
extern "C" void app_main(void)
```

在当前项目里入口是：

```text
main/main.cpp
```

ESP-IDF 默认运行 FreeRTOS。你不再只有一个永远运行的 `while (1)`，而是可以创建多个任务：

```text
任务 A：UI 刷新
任务 B：Wi-Fi 扫描
任务 C：摄像头采集
任务 D：AI 检测
任务 E：视频播放
```

FreeRTOS 调度器会按照优先级、阻塞状态、时间片等规则切换任务。

你需要从“一个超级循环”切换成“多个协作任务”：

```text
裸机思维：
    我在 while(1) 里依次调用每个模块。

RTOS 思维：
    每个长期工作放在自己的 task 里。
    task 不忙等，没事就阻塞等待事件/信号量/队列。
    共享资源要加锁或通过队列传递。
```

### 1.3 当前项目里的典型任务

你可以在代码里看到这些 FreeRTOS 入口：

```text
components/apps/setting/Setting.cpp
  - euiRefresTask
  - wifiScanTask
  - wifiConnectTask

components/apps/camera/Camera.cpp
  - taskCameraInit
  - camera_dectect_task

components/apps/camera/app_video.c
  - video_stream_task

components/apps/video_player/esp_lvgl_simple_player/esp_lvgl_simple_player.c
  - show_video_task
```

它们不是普通函数调用一次就返回，而是独立任务，通常会循环运行或等待事件。

---

## 2. ESP32-P4 是什么，和常见 STM32 有什么不同

### 2.1 ESP32 不是单一芯片概念

“ESP32”是一个大系列，不同芯片差别很大：

- ESP32：经典双核 MCU，带 Wi-Fi/Bluetooth
- ESP32-S3：带 Wi-Fi/Bluetooth，常用于 AI/USB/LCD 项目
- ESP32-C3/C6：RISC-V，常用于无线连接
- ESP32-P4：更偏应用处理器，适合显示、摄像头、多媒体、AI 加速场景

当前工程目标是：

```text
CONFIG_IDF_TARGET="esp32p4"
```

也就是 ESP32-P4。

### 2.2 当前开发板的重点外设

当前示例运行在 ESP32-P4 Function EV Board 上，重点使用：

- MIPI-DSI：连接 LCD 显示屏
- MIPI-CSI：连接摄像头
- PSRAM：放大图像缓冲、LVGL 缓冲、视频缓冲
- SD 卡：视频播放器读取 MJPEG 文件
- SPIFFS：内置音频、图片等资源
- 音频 Codec：音乐/音效播放
- ESP32-C6 / esp-hosted / esp-wifi-remote：给 P4 提供无线能力

注意：ESP32-P4 本身不是“一个带 Wi-Fi 的普通 ESP32”。在这个开发板设计里，无线能力通过旁边的 ESP32-C6 配合 `esp_hosted`、`esp_wifi_remote` 来提供。

这和 STM32 外挂 Wi-Fi 模块有点类似，只是 ESP-IDF 把它封装成更接近原生 Wi-Fi API 的形式。

---

## 3. ESP-IDF 工程结构

### 3.1 入口文件

当前项目的入口在：

```text
main/main.cpp
```

核心流程大致是：

```cpp
extern "C" void app_main(void)
{
    nvs_flash_init();
    bsp_spiffs_mount();
    bsp_sdcard_mount();      // 可选，取决于 CONFIG_EXAMPLE_ENABLE_SD_CARD
    bsp_extra_codec_init();

    lv_display_t *disp = lvgl_adapter_init(&cfg);
    bsp_display_backlight_on();

    esp_lv_adapter_lock(-1);

    ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone();
    phone->begin();

    phone->installApp(new Calculator());
    phone->installApp(new MusicPlayer());
    phone->installApp(new AppSettings());
    phone->installApp(new Game2048());
    phone->installApp(new Camera(1280, 720));

    esp_lv_adapter_unlock();
}
```

你可以把它理解成 STM32 裸机里的 `main()`，但它运行时 FreeRTOS 已经存在。

### 3.2 ESP-IDF 组件

ESP-IDF 不是一个简单的 `main.c + Makefile` 项目，而是组件化构建。

当前工程里：

```text
main/
components/apps/
components/human_face_detect/
components/pedestrian_detect/
```

这些都是组件或组件目录。

组件通过 `CMakeLists.txt` 声明自己的源码、头文件目录和依赖。

例如 `main/CMakeLists.txt`：

```cmake
idf_component_register(
    SRCS main.cpp lvgl_adapter_init.c
    INCLUDE_DIRS .)
```

`components/apps/CMakeLists.txt` 更粗暴一些，会递归收集所有 `.c` 和 `.cpp`：

```cmake
file(GLOB_RECURSE APPS_C_SRCS ${APPS_DIR}/*.c)
file(GLOB_RECURSE APPS_CPP_SRCS ${APPS_DIR}/*.cpp)

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS ${APPS_DIR}
    REQUIRES lvgl__lvgl esp_lvgl_adapter esp_event esp_wifi ...)
```

这意味着你在 `components/apps/xxx/` 下新增 `.cpp` 文件后，一般会自动参与编译。

### 3.3 `managed_components/`

这个目录不是你手写业务代码的地方。

它是 ESP-IDF 组件管理器下载的依赖，比如：

```text
managed_components/lvgl__lvgl/
managed_components/espressif__esp-brookesia/
managed_components/espressif__esp_video/
managed_components/espressif__esp_lvgl_adapter/
```

类比到 STM32：

```text
managed_components/ 有点像你从 ST 或第三方拿来的 HAL、BSP、中间件库。
```

一般不要直接改这里。要改行为，优先：

- 改自己工程里的封装代码
- 改 Kconfig 配置
- 改 `idf_component.yml` 依赖版本
- 在自己的组件里加适配层

### 3.4 `sdkconfig` 和 `sdkconfig.defaults`

ESP-IDF 工程大量功能通过 Kconfig 配置。

你会看到：

```text
sdkconfig
sdkconfig.defaults
sdkconfig.old
```

区别：

- `sdkconfig`：当前完整配置，通常很大
- `sdkconfig.defaults`：项目默认配置，适合长期维护
- `sdkconfig.old`：旧配置备份

你通过下面命令打开配置界面：

```bash
idf.py menuconfig
```

当前工程里的关键默认配置在 `sdkconfig.defaults`：

```text
CONFIG_IDF_TARGET="esp32p4"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_250M=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
CONFIG_LV_MEM_CUSTOM=y
CONFIG_LV_MEM_CUSTOM_INCLUDE="esp_heap_caps.h"
CONFIG_CAMERA_SC2336=y
CONFIG_CAMERA_SC2336_MIPI_RAW10_1280X720_30FPS=y
```

### 3.5 Flash 分区

STM32 里你可能直接面对 Flash 地址、扇区、链接脚本。

ESP-IDF 里常通过分区表描述 Flash 布局。

当前 `partitions.csv`：

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, ,        9M,
storage,  data, spiffs,  ,        4M,
```

含义：

- `nvs`：键值存储，保存设置、Wi-Fi 参数等
- `phy_init`：射频相关数据
- `factory`：主程序固件
- `storage`：SPIFFS 文件系统

当前工程在 `main/CMakeLists.txt` 里把 `spiffs/` 打包进 `storage` 分区：

```cmake
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
```

---

## 4. FreeRTOS 基础

### 4.1 Task 是什么

Task 可以理解成“有自己栈空间的函数”，由 RTOS 调度运行。

典型创建方式：

```cpp
xTaskCreatePinnedToCore(
    (TaskFunction_t)camera_dectect_task,
    "Camera Detect",
    1024 * 8,
    this,
    5,
    &_detect_task_handle,
    1
);
```

参数含义：

```text
任务函数        camera_dectect_task
任务名          "Camera Detect"
栈大小          1024 * 8
参数            this
优先级          5
任务句柄        &_detect_task_handle
绑定核心        1
```

和裸机不同的是：你不应该在 task 里写空转死循环。

错误习惯：

```c
while (1) {
    scan();
}
```

更好的习惯：

```c
while (1) {
    wait_event_or_delay();
    do_work();
}
```

当前项目里经常使用：

```cpp
vTaskDelay(pdMS_TO_TICKS(100));
xEventGroupWaitBits(...);
xSemaphoreTake(...);
```

### 4.2 优先级

FreeRTOS 根据优先级调度任务。高优先级任务如果一直不阻塞，低优先级任务就可能跑不到。

所以高优先级任务必须避免长时间占用 CPU。

经验：

- 摄像头、视频、AI 这类实时任务优先级会偏高
- UI 相关任务不能被长时间阻塞
- Wi-Fi 扫描、文件遍历、NVS 读写不要占用 UI 锁太久

### 4.3 栈大小

STM32 裸机里你可能只关心全局栈。

FreeRTOS 每个 task 都有自己的栈。

创建任务时的这个参数很重要：

```cpp
1024 * 8
```

如果任务里有：

- 大数组
- 复杂 C++ 容器
- 图像处理
- printf 较多
- 深层函数调用

栈可能不够。

大图像缓冲不要放栈上，要放堆上，当前项目常用：

```cpp
heap_caps_aligned_alloc(..., MALLOC_CAP_SPIRAM)
```

### 4.4 信号量

信号量可以理解为“事件通知”或“资源计数”。

当前项目里相机初始化用二值信号量：

```cpp
_camera_init_sem = xSemaphoreCreateBinary();

xTaskCreatePinnedToCore(... taskCameraInit ...);

if (xSemaphoreTake(_camera_init_sem, pdMS_TO_TICKS(CAMERA_INIT_TASK_WAIT_MS)) != pdTRUE) {
    ESP_LOGE(TAG, "Camera init timeout");
    return false;
}
```

含义：

```text
主任务：创建相机初始化任务，然后等待信号量
相机初始化任务：初始化完成后 xSemaphoreGive()
主任务：收到信号量，继续运行
```

### 4.5 Event Group

Event Group 是一组 bit 标志，适合表达多个状态。

当前相机代码：

```cpp
typedef enum {
    CAMERA_EVENT_TASK_RUN = BIT(0),
    CAMERA_EVENT_DELETE = BIT(1),
    CAMERA_EVENT_PED_DETECT = BIT(2),
    CAMERA_EVENT_HUMAN_DETECT = BIT(3),
} camera_event_id_t;
```

它可以表达：

- 相机任务是否运行
- app 是否正在关闭
- 当前是否行人检测模式
- 当前是否人脸检测模式

使用方式：

```cpp
xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);

xEventGroupWaitBits(camera_event_group, CAMERA_EVENT_TASK_RUN,
                    pdFALSE, pdTRUE, portMAX_DELAY);
```

这比你在裸机里自己维护多个 `volatile bool` 更清晰。

### 4.6 ISR 和任务的关系

裸机里中断经常直接处理很多事情。

RTOS 里更推荐：

```text
中断里只做很短的事情：
  - 清标志
  - 记录数据
  - 给信号量/队列
  - 请求任务切换

复杂逻辑放到 task 里执行。
```

当前 `app_camera_pipeline.cpp` 里就有 ISR 场景处理：

```cpp
if (xPortInIsrContext()) {
    BaseType_t wakeup = pdFALSE;
    xSemaphoreGiveFromISR(stream->ready_sem, &wakeup);
    if (wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
} else {
    xSemaphoreGive(stream->ready_sem);
}
```

重点：

- ISR 里不能随便调用普通 FreeRTOS API
- ISR 版本 API 通常带 `FromISR`
- ISR 里不能做耗时操作

---

## 5. ESP-IDF 常用能力

### 5.1 日志

ESP-IDF 不推荐裸 `printf` 到处飞，常用：

```cpp
#include "esp_log.h"

static const char *TAG = "Camera";

ESP_LOGI(TAG, "Video Stream Start");
ESP_LOGW(TAG, "Not init");
ESP_LOGE(TAG, "video cam open failed");
```

日志等级：

```text
I：Info，正常状态
W：Warning，可恢复但需要注意
E：Error，失败
```

当前项目里大量使用 `ESP_LOGI/W/E`。

### 5.2 错误码

ESP-IDF 常用：

```cpp
esp_err_t
ESP_OK
ESP_FAIL
ESP_ERR_INVALID_ARG
ESP_ERR_NO_MEM
```

常见写法：

```cpp
esp_err_t err = nvs_flash_init();
ESP_ERROR_CHECK(err);
```

或者：

```cpp
ESP_GOTO_ON_FALSE(stream, ESP_ERR_NO_MEM, err, TAG,
                  "Failed to allocate memory");
```

和 STM32 HAL 的 `HAL_OK/HAL_ERROR` 有点像，但 ESP-IDF 的错误码体系更统一。

### 5.3 NVS

NVS 是 ESP-IDF 的键值存储。适合存：

- 设置项
- 音量
- 亮度
- 游戏最高分
- Wi-Fi 参数

当前项目里：

```text
components/apps/setting/Setting.cpp
components/apps/game_2048/Game_2048.cpp
```

使用 NVS 保存设置。

### 5.4 SPIFFS

SPIFFS 是 Flash 上的小文件系统。

当前项目的资源目录：

```text
spiffs/music/
spiffs/2048/
```

这些文件会打包进 `storage` 分区。

启动时：

```cpp
ESP_ERROR_CHECK(bsp_spiffs_mount());
ESP_LOGI(TAG, "SPIFFS mount successfully");
```

你可以把它理解为：

```text
STM32 内部 Flash 里放了一块小文件系统，
程序启动时把它挂载起来，
之后可以按文件路径读取音频/资源。
```

---

## 6. 内存：内部 SRAM、PSRAM、DMA、Cache

### 6.1 为什么 ESP32-P4 项目里经常看到 PSRAM

显示和摄像头会产生很大的数据。

一帧 1024 x 600 RGB565 图像大小：

```text
1024 * 600 * 2 = 1,228,800 bytes，约 1.17 MB
```

如果是多个缓冲，马上就是几 MB。

普通 MCU 内部 SRAM 往往不够，所以这个项目大量使用 PSRAM。

当前默认配置：

```text
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_250M=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
CONFIG_LV_MEM_CUSTOM=y
CONFIG_LV_MEM_CUSTOM_INCLUDE="esp_heap_caps.h"
```

### 6.2 `heap_caps_*`

ESP-IDF 不只是 `malloc()`，还可以指定内存能力：

```cpp
heap_caps_aligned_alloc(128, size, MALLOC_CAP_SPIRAM);
heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

含义：

- `MALLOC_CAP_SPIRAM`：放到 PSRAM
- `MALLOC_CAP_INTERNAL`：放到内部 RAM
- `MALLOC_CAP_8BIT`：可按 8 bit 访问
- aligned：按指定边界对齐

相机代码里：

```cpp
_cam_buffer[i] = (uint8_t *)heap_caps_aligned_alloc(
    data_cache_line_size,
    _hor_res * _ver_res * BSP_LCD_BITS_PER_PIXEL / 8,
    MALLOC_CAP_SPIRAM
);
```

### 6.3 为什么需要对齐

摄像头、LCD、DMA、PPA、Cache 都可能要求缓冲区按某个边界对齐。

如果你在 STM32 上用过 DMA，就知道：

- 缓冲区地址可能要对齐
- Cache 打开后要注意一致性
- DMA 和 CPU 同时访问内存时要小心

ESP32-P4 上也是类似问题，只是库帮你封装了很多。

当前项目通过：

```cpp
esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
```

拿到对齐要求。

---

## 7. LVGL 是什么

### 7.1 LVGL 不是显示屏驱动

LVGL 是 GUI 库，不是 LCD 芯片驱动。

它负责：

- 按钮
- 标签
- 图片
- 滑条
- 键盘
- 列表
- 样式
- 事件
- 屏幕对象树

它不直接知道你的 LCD 是 SPI、RGB、MIPI-DSI 还是并口。

中间需要“显示驱动适配层”把 LVGL 的绘制结果送到屏幕。

当前项目的链路大致是：

```text
你的 App 创建 LVGL 控件
  |
  v
LVGL 维护对象树、样式、事件和刷新区域
  |
  v
esp_lvgl_adapter
  |
  v
BSP display / esp_lcd
  |
  v
MIPI-DSI
  |
  v
EK79007 LCD
```

### 7.2 LVGL 的对象模型

LVGL 里几乎所有 UI 元素都是 `lv_obj_t *`。

例如计算器：

```cpp
keyboard = lv_btnmatrix_create(lv_scr_act());
lv_btnmatrix_set_map(keyboard, keyboard_map);
lv_obj_set_size(keyboard, _width, keyboard_h);
lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
```

这和裸机直接刷像素不同。

裸机刷屏可能是：

```c
LCD_DrawRect(x, y, w, h, color);
LCD_ShowString(x, y, "hello");
```

LVGL 更像：

```text
创建一个按钮对象
设置按钮大小
设置按钮文字
设置事件回调
LVGL 决定什么时候重绘
```

### 7.3 样式

LVGL 控件有样式属性：

```cpp
lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
lv_obj_set_style_text_color(label, lv_color_make(170, 170, 170), 0);
lv_obj_set_style_radius(obj, 0, 0);
lv_obj_set_style_border_width(obj, 0, 0);
```

可以理解为嵌入式版 CSS，但它是 C API。

### 7.4 事件

LVGL 控件可以注册事件回调：

```cpp
lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, this);
```

回调里拿到事件：

```cpp
void Calculator::keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    Calculator *app = (Calculator *)lv_event_get_user_data(e);
}
```

这和 STM32 按键中断不同。

LVGL 的事件通常是 GUI 层事件：

- 点击
- 值改变
- 屏幕加载
- 绘制某个部分
- 手势

### 7.5 线程安全：非常重要

LVGL 通常不是随便哪个 task 都能直接调用的。

当前项目使用 `esp_lv_adapter_lock()` 保护 LVGL 访问。

比如相机帧回调里更新画面：

```cpp
if (esp_lv_adapter_lock(100) == ESP_OK) {
    lv_canvas_set_buffer(ui_ImageCameraShotImage, camera_buf,
                         camera_buf_hes, camera_buf_ves,
                         LV_IMG_CF_TRUE_COLOR);
    lv_refr_now(NULL);
    esp_lv_adapter_unlock();
}
```

记住：

```text
后台任务、摄像头任务、Wi-Fi 任务里要更新 UI：
    先 lock
    快速更新 LVGL 对象
    立刻 unlock

不要拿着 LVGL 锁做耗时操作。
```

---

## 8. ESP-Brookesia 是什么

当前项目不是单纯 LVGL demo，而是基于 ESP-Brookesia 做了一个类似手机桌面的界面。

你可以把 ESP-Brookesia 理解为：

```text
基于 LVGL 的“手机壳子/桌面框架”
```

它提供：

- Launcher
- 状态栏
- 导航栏
- 最近任务
- App 安装和切换
- App 生命周期

当前代码里：

```cpp
ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone();
phone->begin();
phone->installApp(new Calculator());
```

每个 App 继承：

```cpp
class Calculator: public ESP_Brookesia_PhoneApp
```

典型生命周期：

```cpp
bool init(void) override;
bool run(void);
bool back(void);
bool close(void);
bool pause(void);
bool resume(void);
```

含义：

- `init()`：一次性初始化
- `run()`：打开 App 时创建/显示 UI
- `pause()`：App 进入后台
- `resume()`：App 回到前台
- `back()`：导航返回
- `close()`：关闭并释放资源

写新 App 时，要先理解这个模型，不要像裸机一样所有页面都堆在一个 `while(1)` 里。

---

## 9. 显示屏：从 LVGL 到 LCD

### 9.1 当前显示屏硬件

README 里说明当前屏幕是：

```text
7 英寸
1024 x 600
LCD driver IC: EK79007
接口：MIPI-DSI
```

连接上还涉及：

- 5V
- GND
- PWM 背光
- LCD_RST
- MIPI_DSI FPC

### 9.2 MIPI-DSI 和你熟悉的 SPI LCD 有什么区别

很多 STM32 入门屏是：

- SPI TFT
- FSMC/8080 并口屏
- RGB 并口屏

MIPI-DSI 是更高速、更复杂的显示接口，常见于手机/平板。

你不需要自己按 GPIO 时序写 DSI。ESP-IDF/BSP/esp_lcd 负责底层协议。

你的代码主要面对：

```cpp
bsp_display_new_with_handles()
esp_lv_adapter_register_display()
```

### 9.3 当前项目显示初始化

显示初始化封装在：

```text
main/lvgl_adapter_init.c
```

关键流程：

```cpp
bsp_lcd_handles_t handles = { 0 };
esp_err_t err = bsp_display_new_with_handles(&cfg->hw_cfg, &handles);

const esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
esp_lv_adapter_init(&adapter_cfg);

esp_lv_adapter_display_config_t disp_cfg =
    ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        handles.panel, handles.io, hres, vres, ESP_LV_ADAPTER_ROTATE_0);

lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
```

可以拆成几层：

```text
BSP 创建 LCD panel 和 panel IO
  |
  v
esp_lv_adapter 初始化 LVGL 适配层
  |
  v
注册一个 LVGL display
  |
  v
LVGL 以后刷新时通过这个 display 输出到屏幕
```

### 9.4 触摸

当前项目还初始化了触摸：

```cpp
esp_lcd_touch_handle_t touch = NULL;
err = bsp_touch_new(NULL, &touch);

const esp_lv_adapter_touch_config_t touch_cfg =
    ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch);

lv_indev_t *indev = esp_lv_adapter_register_touch(&touch_cfg);
```

触摸链路：

```text
GT911 触摸芯片
  |
  v
I2C
  |
  v
BSP touch driver
  |
  v
esp_lv_adapter
  |
  v
LVGL input device
  |
  v
LVGL 点击/滑动/手势事件
```

所以你写 App 时一般不用自己读触摸坐标，LVGL 会把触摸转换成控件事件。

### 9.5 背光

当前项目在显示初始化后打开背光：

```cpp
bsp_display_backlight_on();
```

实际硬件上背光可能由 PWM 控制。你写普通 UI 时不需要直接碰 PWM，设置 App 里的亮度控制会通过 BSP/封装处理。

---

## 10. 摄像头：从 MIPI-CSI 到屏幕

### 10.1 当前摄像头硬件

README 里描述当前摄像头：

```text
传感器：SC2336
接口：MIPI-CSI
分辨率配置：1280 x 720
```

默认配置：

```text
CONFIG_CAMERA_SC2336=y
CONFIG_CAMERA_SC2336_MIPI_RAW10_1280X720_30FPS=y
```

### 10.2 摄像头接口层次

摄像头不是简单“读一个 GPIO”。

大概有两条链路：

```text
控制链路：
ESP32-P4 I2C/SCCB -> 摄像头寄存器配置

数据链路：
摄像头 MIPI-CSI -> ESP32-P4 CSI 接收 -> 视频驱动 -> 内存 buffer
```

控制链路用于设置：

- 分辨率
- 帧率
- 曝光
- 翻转
- 输出格式

数据链路用于持续搬运图像帧。

### 10.3 当前项目的摄像头文件

主要代码：

```text
components/apps/camera/Camera.cpp
components/apps/camera/app_video.c
components/apps/camera/app_video.h
components/apps/camera/app_camera_pipeline.cpp
components/apps/camera/app_camera_pipeline.hpp
components/apps/camera/app_pedestrian_detect.cpp
components/apps/camera/app_humanface_detect.cpp
```

职责：

- `Camera.cpp`：Brookesia App、UI、模式切换、启动/停止
- `app_video.c`：摄像头设备打开、格式设置、buffer、视频流任务
- `app_camera_pipeline.cpp`：帧和检测结果之间的队列/同步
- `app_*_detect.cpp`：调用 AI 检测模型

### 10.4 V4L2 风格接口

`app_video.c` 使用了 Linux V4L2 风格的接口：

```c
#include "linux/videodev2.h"
```

你会看到：

- `open()`
- `ioctl()`
- `VIDIOC_QUERYCAP`
- `VIDIOC_G_FMT`
- `VIDIOC_S_FMT`
- `VIDIOC_REQBUFS`
- `VIDIOC_QBUF`
- `VIDIOC_DQBUF`

这和 STM32 裸机自己配置 DCMI/DMA 不一样。ESP-IDF 的 `esp_video` 把摄像头抽象成类似 Linux video device 的模型。

### 10.5 摄像头数据流

当前相机 App 的数据流可以理解为：

```text
SC2336 摄像头
  |
  v
MIPI-CSI 接收
  |
  v
esp_video / V4L2 设备
  |
  v
app_video.c 取帧
  |
  v
camera_video_frame_operation()
  |
  +-- 普通模式：直接给 LVGL canvas 显示
  |
  +-- AI 模式：
        1. 把帧送入 feed_pipeline
        2. AI task 做人脸/行人检测
        3. 检测结果送入 detect_pipeline
        4. 在图像上画框/关键点
        5. 给 LVGL canvas 显示
```

### 10.6 相机 buffer

相机初始化里分配多个帧缓冲：

```cpp
for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
    _cam_buffer[i] = (uint8_t *)heap_caps_aligned_alloc(
        data_cache_line_size,
        _hor_res * _ver_res * BSP_LCD_BITS_PER_PIXEL / 8,
        MALLOC_CAP_SPIRAM
    );
}
```

`EXAMPLE_CAM_BUF_NUM` 在 `app_video.h`：

```c
#define EXAMPLE_CAM_BUF_NUM (4)
```

为什么要多个 buffer？

```text
摄像头正在写第 N 帧
CPU/LVGL 可能正在显示第 N-1 帧
AI 可能正在处理第 N-2 帧
```

多个 buffer 可以减少互相等待。

### 10.7 AI 检测模式

当前相机支持：

- 普通模式
- 行人检测
- 人脸检测

模式通过 event group bit 表示：

```cpp
CAMERA_EVENT_PED_DETECT
CAMERA_EVENT_HUMAN_DETECT
```

检测模型在：

```text
components/pedestrian_detect/
components/human_face_detect/
```

模型文件：

```text
components/pedestrian_detect/models/p4/*.espdl
components/human_face_detect/models/p4/*.espdl
```

它们通过组件自己的 `CMakeLists.txt` 打包。

---

## 11. 音频、SD 卡、SPIFFS 和视频播放

### 11.1 SPIFFS 内置音频

当前有：

```text
spiffs/music/*.mp3
spiffs/2048/*.mp3
```

游戏和音乐 App 可以从 SPIFFS 播放资源。

### 11.2 SD 卡视频

Video Player App 依赖 SD 卡。

README 里要求视频是 MJPEG 格式：

```bash
ffmpeg -i input.mp4 -vcodec mjpeg -q:v 2 -vf "scale=1024:600" -acodec copy output.mjpeg
```

启用 SD 卡：

```text
idf.py menuconfig
Example Configurations -> Enable SD Card
```

对应配置：

```text
CONFIG_EXAMPLE_ENABLE_SD_CARD
```

代码中：

```cpp
#if CONFIG_EXAMPLE_ENABLE_SD_CARD
    ESP_ERROR_CHECK(bsp_sdcard_mount());
    AppVideoPlayer *app_video_player = new AppVideoPlayer();
    phone->installApp(app_video_player);
#endif
```

也就是说：没有启用 SD 卡时，视频播放器 App 不会安装。

---

## 12. 当前 App 的结构

### 12.1 计算器 Calculator

路径：

```text
components/apps/calculator/Calculator.hpp
components/apps/calculator/Calculator.cpp
```

特点：

- 手写 LVGL UI
- 使用 `lv_btnmatrix`
- 使用事件回调处理按键
- 逻辑相对独立，适合初学 LVGL 阅读

推荐先读这个 App。

### 12.2 设置 AppSettings

路径：

```text
components/apps/setting/Setting.hpp
components/apps/setting/Setting.cpp
components/apps/setting/ui/
```

特点：

- 使用 SquareLine 导出的 UI 文件
- 管理 Wi-Fi、亮度、音量、NVS
- 有多个 FreeRTOS task
- 有大量 LVGL 事件回调

适合学习“真实项目里 UI 和后台任务怎么配合”。

### 12.3 相机 Camera

路径：

```text
components/apps/camera/Camera.hpp
components/apps/camera/Camera.cpp
```

特点：

- 涉及摄像头
- 涉及 AI 检测
- 涉及 PSRAM 大 buffer
- 涉及 event group
- 涉及 LVGL canvas 显示实时图像

这是项目里最复杂的一块，建议等你熟悉 FreeRTOS 和 LVGL 基础后再深入。

### 12.4 音乐和视频

路径：

```text
components/apps/music_player/
components/apps/video_player/
```

特点：

- 音频播放
- SPIFFS/SD 卡文件
- 视频解码
- 后台播放任务
- LVGL 控件控制播放状态

---

## 13. 如何新增一个最简单的 App

假设你要新增一个 `HelloApp`。

### 13.1 新建目录

```text
components/apps/hello/
```

### 13.2 新建头文件

```cpp
#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

class HelloApp: public ESP_Brookesia_PhoneApp {
public:
    HelloApp();
    ~HelloApp();

    bool init(void) override;
    bool run(void);
    bool back(void);
    bool close(void);

private:
    lv_obj_t *label;
};
```

### 13.3 新建源文件

```cpp
#include "HelloApp.hpp"

LV_IMG_DECLARE(img_app_hello);

HelloApp::HelloApp():
    ESP_Brookesia_PhoneApp("Hello", &img_app_hello, true),
    label(nullptr)
{
}

HelloApp::~HelloApp()
{
}

bool HelloApp::init(void)
{
    return true;
}

bool HelloApp::run(void)
{
    label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Hello ESP32-P4 + LVGL");
    lv_obj_center(label);
    return true;
}

bool HelloApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool HelloApp::close(void)
{
    label = nullptr;
    return true;
}
```

### 13.4 加到 `apps.h`

```cpp
#include "hello/HelloApp.hpp"
```

### 13.5 在 `main/main.cpp` 安装

```cpp
HelloApp *hello = new HelloApp();
assert(hello != nullptr && "Failed to create hello");
assert((phone->installApp(hello) >= 0) && "Failed to begin hello");
```

### 13.6 图标

真实工程里还需要一个 `img_app_hello` 图标资源。你可以先复制已有 App 图标的生成方式，后续再用 LVGL/SquareLine 工具生成。

---

## 14. 推荐学习路线

不要一上来就啃相机 AI 检测。建议按这个顺序：

### 第 1 阶段：ESP-IDF 基础

目标：

- 能 build/flash/monitor
- 知道 `app_main()` 在哪里
- 知道组件、Kconfig、sdkconfig 是什么
- 会看 ESP_LOG 日志

练习：

1. 运行：

```bash
idf.py build
idf.py -p PORT flash monitor
```

2. 在 `main/main.cpp` 加一条 `ESP_LOGI(TAG, "...")`。
3. 编译烧录，确认日志出现。

### 第 2 阶段：FreeRTOS

目标：

- 会创建 task
- 会 `vTaskDelay`
- 知道任务优先级和栈大小
- 会用信号量或 event group

练习：

1. 新建一个简单 task，每秒打印一次。
2. 用 event group 控制它暂停/继续。
3. 观察不同优先级对运行的影响。

### 第 3 阶段：LVGL

目标：

- 会创建 label、button、slider
- 会注册事件回调
- 知道 UI 更新需要锁
- 会读 `Calculator.cpp`

练习：

1. 在计算器或新 App 里创建一个按钮。
2. 点击按钮后修改 label 文本。
3. 从一个 FreeRTOS task 每秒更新 label，注意加 `esp_lv_adapter_lock()`。

### 第 4 阶段：ESP-Brookesia App 生命周期

目标：

- 知道 `init/run/back/close/pause/resume`
- 会新增一个 App
- 知道如何安装到 phone launcher

练习：

1. 新增 `HelloApp`。
2. 点击图标进入 App。
3. 按返回或关闭后能正常回到桌面。

### 第 5 阶段：显示屏和触摸

目标：

- 知道 LVGL 和 LCD 驱动不是一回事
- 理解 `lvgl_adapter_init.c`
- 知道触摸如何变成 LVGL 事件

练习：

1. 改变某个控件位置和颜色。
2. 添加一个按钮并响应点击。
3. 查看触摸事件对应的 LVGL 回调。

### 第 6 阶段：摄像头

目标：

- 知道 MIPI-CSI 和 SCCB/I2C 的区别
- 知道 frame buffer 在哪里
- 知道 `app_video.c` 怎么启动视频流
- 理解相机普通显示链路

练习：

1. 先只跑普通相机预览。
2. 打印帧尺寸和格式。
3. 观察关闭 App 时任务是否退出。

### 第 7 阶段：AI 检测

目标：

- 知道模型文件在哪里
- 知道检测结果怎么回到图像上
- 理解 `feed_pipeline` 和 `detect_pipeline`

练习：

1. 切换行人检测和人脸检测。
2. 打印检测框数量。
3. 修改绘制框颜色或线宽。

---

## 15. 常见坑

### 15.1 把 LVGL 当成普通线程安全库

错误：

```cpp
// 在任意 task 里直接调用
lv_label_set_text(label, "new text");
```

建议：

```cpp
if (esp_lv_adapter_lock(100) == ESP_OK) {
    lv_label_set_text(label, "new text");
    esp_lv_adapter_unlock();
}
```

### 15.2 大 buffer 放在栈上

错误：

```cpp
uint8_t frame[1024 * 600 * 2];
```

建议：

```cpp
uint8_t *frame = (uint8_t *)heap_caps_aligned_alloc(
    128, 1024 * 600 * 2, MALLOC_CAP_SPIRAM);
```

### 15.3 忘记任务退出

App 关闭时，只释放 buffer，但后台 task 还在跑，就会出现野指针。

正确思路：

```text
设置 delete/stop 标志
等待 task 退出
再释放 buffer
```

当前相机代码就是这种思路。

### 15.4 直接改 `managed_components`

不要把第三方依赖目录当自己的源码改。下次依赖更新或重新拉取可能全丢。

### 15.5 只看 UI 文件，不看生命周期

LVGL 控件创建只是 App 的一部分。你还要看：

- App 什么时候打开
- 什么时候关闭
- 任务什么时候开始
- 任务什么时候停止
- buffer 谁分配谁释放

### 15.6 摄像头和显示分辨率混淆

当前相机可能是 1280 x 720，屏幕是 1024 x 600。

这意味着中间可能存在：

- 缩放
- 裁剪
- 格式转换
- PPA 处理
- LVGL canvas 显示尺寸适配

不要假设摄像头分辨率等于屏幕分辨率。

### 15.7 忘记 menuconfig

ESP-IDF 很多功能不是代码里改一个宏就行，而是 Kconfig 控制。

例如：

- SD 卡视频播放器
- 摄像头翻转
- 模型存储位置
- LVGL 字体
- PSRAM

要养成查看 `sdkconfig.defaults` 和 `Kconfig` 的习惯。

---

## 16. STM32 经验如何迁移

你已有的 STM32 裸机经验非常有用，但要换一种使用方式。

### 16.1 仍然有用的经验

- GPIO/I2C/SPI/UART 基础
- DMA 和 buffer 对齐意识
- 中断里少做事
- 状态机思维
- 看 datasheet 的能力
- Flash/RAM 资源意识
- 调试串口日志
- 分层封装驱动

### 16.2 需要调整的习惯

| STM32 裸机习惯 | 在 ESP-IDF/LVGL 中的调整 |
|---|---|
| 一个 `while(1)` 管全部 | 多 task 分工，阻塞等待事件 |
| 中断里处理较多逻辑 | 中断通知 task，复杂逻辑放 task |
| 全局变量传状态 | 优先用队列、信号量、event group |
| 直接刷屏 | 创建 LVGL 对象，让 LVGL 刷新 |
| 任意地方改 UI | 非 LVGL 任务先拿 LVGL 锁 |
| `malloc` 用得少 | 大 buffer 常用 `heap_caps_*` |
| 外设驱动自己写到底 | 先用 BSP/ESP-IDF driver，再做封装 |
| 手动规划所有启动代码 | ESP-IDF 启动后调用 `app_main()` |

---

## 17. 当前工程阅读顺序

建议你按这个顺序看代码：

1. `README.md`
2. `sdkconfig.defaults`
3. `partitions.csv`
4. `CMakeLists.txt`
5. `main/CMakeLists.txt`
6. `main/main.cpp`
7. `main/lvgl_adapter_init.c`
8. `components/apps/calculator/Calculator.cpp`
9. `components/apps/setting/Setting.hpp`
10. `components/apps/setting/Setting.cpp`
11. `components/apps/camera/Camera.hpp`
12. `components/apps/camera/app_video.h`
13. `components/apps/camera/app_video.c`
14. `components/apps/camera/Camera.cpp`
15. `components/apps/camera/app_camera_pipeline.cpp`

不要从 `managed_components/lvgl__lvgl/src/` 开始看。LVGL 源码很大，初期先看项目如何使用 LVGL。

---

## 18. 调试方法

### 18.1 串口日志

常用：

```bash
idf.py -p PORT monitor
```

退出：

```text
Ctrl-]
```

日志里重点看：

- boot 信息
- PSRAM 是否初始化成功
- SPIFFS/SD 是否 mount 成功
- display/touch 是否初始化成功
- app 是否安装成功
- camera/video 是否启动失败
- NVS 是否读写失败

### 18.2 先定位层级

遇到问题时先判断在哪一层：

```text
编译不过：CMake / include / component dependency / Kconfig
启动崩溃：app_main 初始化 / assert / ESP_ERROR_CHECK
屏幕不亮：供电 / 背光 / BSP display / MIPI-DSI
触摸无效：GT911 / I2C / touch register / LVGL indev
UI 卡住：任务优先级 / LVGL 锁 / 死循环 / 大计算
相机无画面：MIPI-CSI / sensor config / app_video / buffer
检测无结果：模型 / 图像格式 / pipeline / 检测阈值
视频不能播：SD 卡 / MJPEG 格式 / 文件扫描 / buffer
```

### 18.3 不要一次改太多

初学阶段建议：

1. 只改一个 App。
2. 只加一个控件。
3. 只加一条日志。
4. 编译。
5. 烧录。
6. 看现象。

显示、摄像头、RTOS、AI 混在一起时，一次改太多很难定位。

---

## 19. 小结

这个项目可以按下面几个核心模型理解：

```text
ESP-IDF:
    工程、组件、配置、构建、Flash、日志、驱动

FreeRTOS:
    多任务、信号量、event group、任务优先级、任务栈

LVGL:
    控件对象树、样式、事件、刷新、输入设备

ESP-Brookesia:
    手机桌面、App 生命周期、导航/状态栏

显示屏:
    LVGL -> esp_lvgl_adapter -> BSP/esp_lcd -> MIPI-DSI -> LCD

摄像头:
    SC2336 -> MIPI-CSI -> esp_video/V4L2 -> buffer -> LVGL/AI

存储:
    NVS 保存小配置
    SPIFFS 保存内置资源
    SD 卡保存大媒体文件
```

如果你已经会 STM32 裸机，最大的门槛不是 C 语言，而是这三个观念：

1. 不再只有一个主循环，而是多个 FreeRTOS task。
2. 不再直接刷屏，而是操作 LVGL 对象。
3. 不再直接硬怼所有外设寄存器，而是优先使用 ESP-IDF/BSP/组件封装。

等这三个观念顺了，再看相机、视频、AI 检测，就会清楚很多。
