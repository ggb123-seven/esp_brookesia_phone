/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_video_init.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"

#include "esp_lcd_touch_gt911.h"

#include "app_video.h"
#include "app_pedestrian_detect.h"
#include "app_humanface_detect.h"
#include "Camera.hpp"
#include "ui/ui.h"

#define CAMERA_INIT_TASK_WAIT_MS            (5000)
#define CAMERA_DETECT_TASK_STACK_SIZE       (8 * 1024)
#define CAMERA_DETECT_TASK_PRIORITY         (5)
#define CAMERA_DETECT_RESULT_MAX            (10)
#define CAMERA_DETECT_TASK_STOP_WAIT_MS     (10000)
#define CAMERA_BMP_HEADER_SIZE              (54)
#define CAMERA_BMP_BYTES_PER_PIXEL          (3)
#define CAMERA_PHOTO_PATH_MAX               (128)

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
#define CAMERA_PHOTO_DIR                    CONFIG_BSP_SD_MOUNT_POINT "/camera"
#endif

typedef enum {
    CAMERA_EVENT_TASK_RUN = BIT(0),
    CAMERA_EVENT_DELETE = BIT(1),
    CAMERA_EVENT_PED_DETECT = BIT(2),
    CAMERA_EVENT_HUMAN_DETECT = BIT(3),
} camera_event_id_t;

LV_IMG_DECLARE(img_app_camera);

static const char *TAG = "Camera";

static size_t data_cache_line_size = 0;
static EventGroupHandle_t camera_event_group;

Camera *Camera::_active_camera = NULL;

static void camera_put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void camera_put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void camera_draw_green_points(uint16_t *buffer, uint32_t width, uint32_t height,
                                     const int *keypoints)
{
    const uint16_t green = 0x07E0;

    for (int point = 0; point < 5; ++point)
    {
        const int center_x = keypoints[point * 2];
        const int center_y = keypoints[point * 2 + 1];

        for (int offset_x = -3; offset_x <= 3; ++offset_x)
        {
            for (int offset_y = -3; offset_y <= 3; ++offset_y)
            {
                const int x = center_x + offset_x;
                const int y = center_y + offset_y;
                if (x >= 0 && y >= 0 && x < (int)width && y < (int)height)
                {
                    buffer[(size_t)y * width + x] = green;
                }
            }
        }
    }
}

static esp_err_t camera_write_all(FILE *file, const void *data, size_t size)
{
    if (fwrite(data, 1, size, file) != size)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
static esp_err_t camera_ensure_photo_dir(void)
{
    struct stat st;
    if (stat(CAMERA_PHOTO_DIR, &st) == 0)
    {
        if (S_ISDIR(st.st_mode))
        {
            return ESP_OK;
        }

        ESP_LOGE(TAG, "Camera photo path exists but is not a directory: %s", CAMERA_PHOTO_DIR);
        return ESP_FAIL;
    }

    if (mkdir(CAMERA_PHOTO_DIR, 0775) != 0)
    {
        ESP_LOGE(TAG, "Failed to create camera photo directory %s: errno=%d", CAMERA_PHOTO_DIR, errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}
#endif

static esp_err_t camera_save_rgb565_bmp_to_sd(const uint8_t *frame, uint32_t width,
                                             uint32_t height, char *out_path,
                                             size_t out_path_len)
{
    if (frame == NULL || width == 0 || height == 0 || out_path == NULL || out_path_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_EXAMPLE_ENABLE_SD_CARD
    (void)width;
    (void)height;
    snprintf(out_path, out_path_len, "%s", "SD card disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t err = camera_ensure_photo_dir();
    if (err != ESP_OK)
    {
        return err;
    }

    int path_len = snprintf(out_path, out_path_len, "%s/photo_%lld_%08" PRIx32 ".bmp",
                            CAMERA_PHOTO_DIR, (long long)esp_timer_get_time(), esp_random());
    if (path_len < 0 || (size_t)path_len >= out_path_len)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t row_stride = (width * CAMERA_BMP_BYTES_PER_PIXEL + 3U) & ~3U;
    const uint32_t pixel_data_size = row_stride * height;
    const uint32_t file_size = CAMERA_BMP_HEADER_SIZE + pixel_data_size;

    uint8_t header[CAMERA_BMP_HEADER_SIZE] = {0};
    header[0] = 'B';
    header[1] = 'M';
    camera_put_le32(&header[2], file_size);
    camera_put_le32(&header[10], CAMERA_BMP_HEADER_SIZE);
    camera_put_le32(&header[14], 40);
    camera_put_le32(&header[18], width);
    camera_put_le32(&header[22], height);
    camera_put_le16(&header[26], 1);
    camera_put_le16(&header[28], 24);
    camera_put_le32(&header[34], pixel_data_size);

    FILE *file = fopen(out_path, "wb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open camera photo file %s: errno=%d", out_path, errno);
        return ESP_FAIL;
    }

    uint8_t *row = (uint8_t *)heap_caps_malloc(row_stride, MALLOC_CAP_8BIT);
    if (row == NULL)
    {
        fclose(file);
        remove(out_path);
        return ESP_ERR_NO_MEM;
    }

    err = camera_write_all(file, header, sizeof(header));
    for (int32_t y = (int32_t)height - 1; err == ESP_OK && y >= 0; --y)
    {
        const uint16_t *src = (const uint16_t *)frame + ((size_t)y * width);
        for (uint32_t x = 0; x < width; ++x)
        {
            uint16_t pixel = src[x];
            uint8_t r = (uint8_t)(((pixel >> 11) & 0x1F) << 3);
            uint8_t g = (uint8_t)(((pixel >> 5) & 0x3F) << 2);
            uint8_t b = (uint8_t)((pixel & 0x1F) << 3);

            row[x * CAMERA_BMP_BYTES_PER_PIXEL + 0] = (uint8_t)(b | (b >> 5));
            row[x * CAMERA_BMP_BYTES_PER_PIXEL + 1] = (uint8_t)(g | (g >> 6));
            row[x * CAMERA_BMP_BYTES_PER_PIXEL + 2] = (uint8_t)(r | (r >> 5));
        }

        if (row_stride > width * CAMERA_BMP_BYTES_PER_PIXEL)
        {
            memset(row + width * CAMERA_BMP_BYTES_PER_PIXEL, 0,
                   row_stride - width * CAMERA_BMP_BYTES_PER_PIXEL);
        }

        err = camera_write_all(file, row, row_stride);
    }

    heap_caps_free(row);

    if (fclose(file) != 0 && err == ESP_OK)
    {
        err = ESP_FAIL;
    }

    if (err != ESP_OK)
    {
        remove(out_path);
    }

    return err;
#endif
}

Camera::Camera(uint16_t hor_res, uint16_t ver_res):
    ESP_Brookesia_PhoneApp("Camera", &img_app_camera, false),  // auto_resize_visual_area
    _hor_res(hor_res),
    _ver_res(ver_res),
    _img_album_buffer(NULL),
    _camera_init_sem(NULL),
    _camera_init_result(ESP_FAIL),
    _camera_ctlr_handle(-1),
    _camera_stream_running(false),
    _img_album(NULL),
    _mode_switch_label(NULL),
    _detect_frame_buffer(NULL),
    _detect_frame_size(0),
    _detect_frame_free_sem(NULL),
    _detect_frame_ready_sem(NULL),
    _detect_results_mutex(NULL),
    _detect_task_done_sem(NULL),
    _detect_task_handle(NULL),
    _detect_results{},
    _detect_result_count(0),
    _detect_result_mode(DETECT_MODE_NORMAL),
    _cam_buffer{},
    _cam_buffer_size{}
{
}

Camera::~Camera()
{
}

bool Camera::run(void)
{
    xEventGroupClearBits(camera_event_group,
                         CAMERA_EVENT_TASK_RUN | CAMERA_EVENT_DELETE |
                         CAMERA_EVENT_PED_DETECT | CAMERA_EVENT_HUMAN_DETECT);

    // Build the LVGL object tree before camera frames are allowed to update it.
    ui_camera_init();

    _img_album_buffer = (uint8_t *)heap_caps_aligned_alloc(
        128, _img_refresh_dsc.data_size, MALLOC_CAP_SPIRAM);
    if (_img_album_buffer == NULL)
    {
        ESP_LOGE(TAG, "Allocate memory for album buffer failed");
        return false;
    }

    lv_img_dsc_t img_dsc = {
        .header = {
            .cf = LV_IMG_CF_TRUE_COLOR,
            .always_zero = 0,
            .reserved = 0,
            .w = _hor_res,
            .h = _ver_res,
        },
        .data_size = _img_refresh_dsc.data_size,
        .data = (const uint8_t *)_img_album_buffer,
    };
    memcpy(&_img_album_dsc, &img_dsc, sizeof(lv_img_dsc_t));

    lv_obj_refr_size(ui_PanelCameraShotAlbum);
    lv_obj_clear_flag(ui_PanelCameraShotAlbum, LV_OBJ_FLAG_CLICKABLE);

    _img_album = lv_imgbtn_create(ui_PanelCameraShotAlbum);
    lv_obj_add_flag(_img_album, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(_img_album, 100, 100);
    lv_obj_center(_img_album);
    lv_obj_add_event_cb(_img_album, onScreenCameraShotAlbumClick, LV_EVENT_CLICKED, this);

    memcpy(&_img_photo_dsc, &img_dsc, sizeof(lv_img_dsc_t));
    memcpy(_img_album_buffer, _img_refresh_dsc.data, _img_refresh_dsc.data_size);
    lv_obj_set_width(ui_ImageCameraPhotoImage, _hor_res);
    lv_obj_set_height(ui_ImageCameraPhotoImage, _ver_res);
    lv_img_set_src(ui_ImageCameraPhotoImage, &_img_photo_dsc);

    lv_obj_add_event_cb(ui_ButtonCameraShotBtn, onScreenCameraShotBtnClick, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(ui_PanelCameraShotTitle, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *mode_switch_btn = lv_btn_create(ui_ImageCameraShotImage);
    lv_obj_set_style_bg_color(mode_switch_btn, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_size(mode_switch_btn, 130, 50);
    lv_obj_align(mode_switch_btn, LV_ALIGN_TOP_RIGHT, -150, 0);
    lv_obj_add_event_cb(mode_switch_btn, onModeSwitchClick, LV_EVENT_CLICKED, this);

    _mode_switch_label = lv_label_create(mode_switch_btn);
    lv_obj_set_style_text_font(_mode_switch_label, &lv_font_montserrat_16,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(_mode_switch_label, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    setModeUi(DETECT_MODE_NORMAL);

    _active_camera = this;
    if (_camera_init_sem == NULL)
    {
        _camera_init_sem = xSemaphoreCreateBinary();
        if (_camera_init_sem == NULL)
        {
            ESP_LOGE(TAG, "Create camera init semaphore failed");
            _active_camera = NULL;
            heap_caps_free(_img_album_buffer);
            _img_album_buffer = NULL;
            return false;
        }

        _camera_init_result = ESP_FAIL;
        BaseType_t created = xTaskCreatePinnedToCore(
            taskCameraInit, "Camera Init", 4096, this, 2, NULL, 0);
        if (created != pdPASS)
        {
            ESP_LOGE(TAG, "Create camera init task failed");
            vSemaphoreDelete(_camera_init_sem);
            _camera_init_sem = NULL;
            _active_camera = NULL;
            heap_caps_free(_img_album_buffer);
            _img_album_buffer = NULL;
            return false;
        }
    }

    if (xSemaphoreTake(_camera_init_sem, pdMS_TO_TICKS(CAMERA_INIT_TASK_WAIT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Camera init timeout");
        _active_camera = NULL;
        heap_caps_free(_img_album_buffer);
        _img_album_buffer = NULL;
        return false;
    }

    vSemaphoreDelete(_camera_init_sem);
    _camera_init_sem = NULL;

    if (_camera_init_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera stream init failed: %s", esp_err_to_name(_camera_init_result));
        _active_camera = NULL;
        heap_caps_free(_img_album_buffer);
        _img_album_buffer = NULL;
        return false;
    }

    _camera_stream_running = true;
    if (!startDetectTask())
    {
        app_video_stream_task_stop(_camera_ctlr_handle);
        app_video_stream_wait_stop();
        _camera_stream_running = false;
        _active_camera = NULL;
        heap_caps_free(_img_album_buffer);
        _img_album_buffer = NULL;
        return false;
    }

    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    return true;
}

bool Camera::pause(void)
{
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);

    return true;
}

bool Camera::resume(void)
{
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);

    return true;
}

bool Camera::back(void)
{
    notifyCoreClosed();

    return true;
}

bool Camera::close(void)
{
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_DELETE);
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    xEventGroupClearBits(camera_event_group,
                         CAMERA_EVENT_PED_DETECT | CAMERA_EVENT_HUMAN_DETECT);

    if (_detect_frame_ready_sem != NULL)
    {
        xSemaphoreGive(_detect_frame_ready_sem);
    }

    if (_camera_stream_running)
    {
        esp_err_t err = app_video_stream_task_stop(_camera_ctlr_handle);
        if (err == ESP_OK)
        {
            err = app_video_stream_wait_stop();
        }

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Stop camera stream failed: %s", esp_err_to_name(err));
        }
        _camera_stream_running = false;
    }

    stopDetectTask();
    _active_camera = NULL;

    if (_img_album_buffer != NULL)
    {
        heap_caps_free(_img_album_buffer);
        _img_album_buffer = NULL;
    }

    return true;
}

bool Camera::startDetectTask(void)
{
    _detect_frame_free_sem = xSemaphoreCreateBinary();
    _detect_frame_ready_sem = xSemaphoreCreateBinary();
    _detect_results_mutex = xSemaphoreCreateMutex();
    _detect_task_done_sem = xSemaphoreCreateBinary();
    if (_detect_frame_free_sem == NULL || _detect_frame_ready_sem == NULL ||
        _detect_results_mutex == NULL || _detect_task_done_sem == NULL)
    {
        ESP_LOGE(TAG, "Create camera detection synchronization objects failed");
        stopDetectTask();
        return false;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        cameraDetectTask, "Camera Detect", CAMERA_DETECT_TASK_STACK_SIZE, this,
        CAMERA_DETECT_TASK_PRIORITY, &_detect_task_handle, 1);
    if (created != pdPASS)
    {
        ESP_LOGE(TAG, "Create camera detection task failed");
        _detect_task_handle = NULL;
        stopDetectTask();
        return false;
    }

    return true;
}

void Camera::stopDetectTask(void)
{
    if (_detect_task_handle != NULL)
    {
        xEventGroupSetBits(camera_event_group, CAMERA_EVENT_DELETE | CAMERA_EVENT_TASK_RUN);
        if (_detect_frame_ready_sem != NULL)
        {
            xSemaphoreGive(_detect_frame_ready_sem);
        }

        if (_detect_task_done_sem == NULL ||
            xSemaphoreTake(_detect_task_done_sem,
                           pdMS_TO_TICKS(CAMERA_DETECT_TASK_STOP_WAIT_MS)) != pdTRUE)
        {
            ESP_LOGE(TAG, "Camera detection task stop timeout");
            vTaskDelete(_detect_task_handle);
            delete_pedestrian_detect();
            delete_humanface_detect();
        }
        _detect_task_handle = NULL;
    }

    if (_detect_frame_buffer != NULL)
    {
        heap_caps_free(_detect_frame_buffer);
        _detect_frame_buffer = NULL;
        _detect_frame_size = 0;
    }

    if (_detect_frame_free_sem != NULL)
    {
        vSemaphoreDelete(_detect_frame_free_sem);
        _detect_frame_free_sem = NULL;
    }
    if (_detect_frame_ready_sem != NULL)
    {
        vSemaphoreDelete(_detect_frame_ready_sem);
        _detect_frame_ready_sem = NULL;
    }
    if (_detect_results_mutex != NULL)
    {
        vSemaphoreDelete(_detect_results_mutex);
        _detect_results_mutex = NULL;
    }
    if (_detect_task_done_sem != NULL)
    {
        vSemaphoreDelete(_detect_task_done_sem);
        _detect_task_done_sem = NULL;
    }

    _detect_result_count = 0;
    _detect_result_mode = DETECT_MODE_NORMAL;
}

void Camera::setModeUi(camera_detect_mode_t mode, const char *status_text)
{
    if (_mode_switch_label == NULL)
    {
        return;
    }

    if (status_text != NULL)
    {
        lv_label_set_text(_mode_switch_label, status_text);
    }
    else if (mode == DETECT_MODE_PEDESTRIAN)
    {
        lv_label_set_text(_mode_switch_label, "Pedestrian\n   Detect");
    }
    else if (mode == DETECT_MODE_FACE)
    {
        lv_label_set_text(_mode_switch_label, "    Face\n   Detect");
    }
    else
    {
        lv_label_set_text(_mode_switch_label, "  Normal\n   Detect");
    }

    if (mode == DETECT_MODE_NORMAL)
    {
        lv_obj_clear_flag(ui_ButtonCameraShotBtn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_PanelCameraShotControlBg, LV_OBJ_FLAG_HIDDEN);
        if (_img_album != NULL)
        {
            lv_obj_clear_flag(_img_album, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else
    {
        lv_obj_add_flag(ui_ButtonCameraShotBtn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelCameraShotControlBg, LV_OBJ_FLAG_HIDDEN);
        if (_img_album != NULL)
        {
            lv_obj_add_flag(_img_album, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void Camera::clearDetectResults(void)
{
    if (_detect_results_mutex != NULL &&
        xSemaphoreTake(_detect_results_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        _detect_result_count = 0;
        _detect_result_mode = DETECT_MODE_NORMAL;
        xSemaphoreGive(_detect_results_mutex);
    }
}

bool Camera::init(void)
{
    camera_event_group = xEventGroupCreate();
    if (camera_event_group == NULL)
    {
        ESP_LOGE(TAG, "Create camera event group failed");
        return false;
    }

    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);

    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
    esp_err_t ret = app_video_main(i2c_bus_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "video main init failed with error 0x%x", ret);
        return false;
    }

    // Open the video device
    _camera_ctlr_handle = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT_RGB565);
    if (_camera_ctlr_handle < 0)
    {
        ESP_LOGE(TAG, "video cam open failed");

        if (ESP_OK == i2c_master_probe(i2c_bus_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) ||
            ESP_OK == i2c_master_probe(i2c_bus_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100))
        {
            ESP_LOGI(TAG, "gt911 touch found");
        }
        else
        {
            ESP_LOGE(TAG, "Touch not found");
        }

        return false;
    }

    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (ret != ESP_OK || data_cache_line_size == 0)
    {
        ESP_LOGE(TAG, "Get camera buffer alignment failed: %s", esp_err_to_name(ret));
        return false;
    }

    const size_t camera_buffer_size =
        (size_t)_hor_res * _ver_res * BSP_LCD_BITS_PER_PIXEL / 8;
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++)
    {
        _cam_buffer[i] = (uint8_t *)heap_caps_aligned_alloc(
            data_cache_line_size, camera_buffer_size, MALLOC_CAP_SPIRAM);
        if (_cam_buffer[i] == NULL)
        {
            ESP_LOGE(TAG, "Allocate camera buffer %d failed", i);
            for (int allocated = 0; allocated < i; ++allocated)
            {
                heap_caps_free(_cam_buffer[allocated]);
                _cam_buffer[allocated] = NULL;
                _cam_buffer_size[allocated] = 0;
            }
            return false;
        }

        _cam_buffer_size[i] = camera_buffer_size;
    }

    // Register the video frame operation callback
    ret = app_video_register_frame_operation_cb(cameraVideoFrameOperation);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Register camera frame callback failed: %s", esp_err_to_name(ret));
        return false;
    }

    lv_img_dsc_t img_dsc = {
        .header = {
            .cf = LV_IMG_CF_TRUE_COLOR,
            .always_zero = 0,
            .reserved = 0,
            .w = _hor_res,
            .h = _ver_res,
        },
        .data_size = _cam_buffer_size[0],
        .data = (const uint8_t *)_cam_buffer[0],
    };

    memcpy(&_img_refresh_dsc, &img_dsc, sizeof(lv_img_dsc_t));

    return true;
}

void Camera::taskCameraInit(void *arg)
{
    Camera *app = static_cast<Camera *>(arg);
    app->_camera_init_result = app_video_set_bufs(
        app->_camera_ctlr_handle, EXAMPLE_CAM_BUF_NUM, (const void **)app->_cam_buffer);

    if (app->_camera_init_result == ESP_OK)
    {
        app->_camera_init_result = app_video_stream_task_start(app->_camera_ctlr_handle, 0);
    }

    xSemaphoreGive(app->_camera_init_sem);

    vTaskDelete(NULL);
}

void Camera::onScreenCameraShotAlbumClick(lv_event_t *e)
{
    lv_obj_invalidate(ui_ImageCameraPhotoImage);
}

void Camera::onScreenCameraShotBtnClick(lv_event_t *e)
{
    Camera *camera = (Camera *)e->user_data;

    if (camera == NULL)
    {
        return;
    }

    lv_obj_add_flag(camera->_img_album, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_PanelCameraShotAlbum, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_src(camera->_img_album, &camera->_img_album_dsc);

    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    memcpy(camera->_img_album_buffer, camera->_img_refresh_dsc.data,
           camera->_img_refresh_dsc.data_size);
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);

    char photo_path[CAMERA_PHOTO_PATH_MAX] = {0};
    esp_err_t save_err = camera_save_rgb565_bmp_to_sd(
        camera->_img_album_buffer, camera->_hor_res, camera->_ver_res,
        photo_path, sizeof(photo_path));
    if (save_err == ESP_OK)
    {
        ESP_LOGI(TAG, "Saved camera photo to %s", photo_path);
    }
    else if (save_err == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW(TAG, "Skip camera photo save: SD card support is disabled");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to save camera photo to SD card: %s", esp_err_to_name(save_err));
    }
}

void Camera::onModeSwitchClick(lv_event_t *e)
{
    Camera *camera = static_cast<Camera *>(lv_event_get_user_data(e));
    if (camera == NULL)
    {
        return;
    }

    EventBits_t bits = xEventGroupGetBits(camera_event_group);
    xEventGroupClearBits(camera_event_group,
                         CAMERA_EVENT_PED_DETECT | CAMERA_EVENT_HUMAN_DETECT);

    if (bits & CAMERA_EVENT_PED_DETECT)
    {
        xEventGroupSetBits(camera_event_group, CAMERA_EVENT_HUMAN_DETECT);
        camera->setModeUi(DETECT_MODE_FACE);
    }
    else if (bits & CAMERA_EVENT_HUMAN_DETECT)
    {
        camera->clearDetectResults();
        camera->setModeUi(DETECT_MODE_NORMAL);
    }
    else
    {
        xEventGroupSetBits(camera_event_group, CAMERA_EVENT_PED_DETECT);
        camera->setModeUi(DETECT_MODE_PEDESTRIAN);
    }
}

void Camera::cameraDetectTask(void *arg)
{
    Camera *app = static_cast<Camera *>(arg);
    camera_detect_mode_t loaded_mode = DETECT_MODE_NORMAL;

    while (true)
    {
        EventBits_t bits = xEventGroupGetBits(camera_event_group);
        if (bits & CAMERA_EVENT_DELETE)
        {
            break;
        }

        camera_detect_mode_t requested_mode = DETECT_MODE_NORMAL;
        if (bits & CAMERA_EVENT_PED_DETECT)
        {
            requested_mode = DETECT_MODE_PEDESTRIAN;
        }
        else if (bits & CAMERA_EVENT_HUMAN_DETECT)
        {
            requested_mode = DETECT_MODE_FACE;
        }

        if (!(bits & CAMERA_EVENT_TASK_RUN) || requested_mode == DETECT_MODE_NORMAL)
        {
            if (loaded_mode != DETECT_MODE_NORMAL)
            {
                delete_pedestrian_detect();
                delete_humanface_detect();
                loaded_mode = DETECT_MODE_NORMAL;
                app->clearDetectResults();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (app->_detect_frame_buffer == NULL)
        {
            app->_detect_frame_size = app->_img_refresh_dsc.data_size;
            app->_detect_frame_buffer = (uint8_t *)heap_caps_aligned_alloc(
                data_cache_line_size, app->_detect_frame_size, MALLOC_CAP_SPIRAM);
            if (app->_detect_frame_buffer == NULL)
            {
                ESP_LOGE(TAG, "Allocate on-demand detection frame failed");
                xEventGroupClearBits(camera_event_group,
                                     CAMERA_EVENT_PED_DETECT | CAMERA_EVENT_HUMAN_DETECT);
                if (esp_lv_adapter_lock(100) == ESP_OK)
                {
                    app->setModeUi(DETECT_MODE_NORMAL, "AI unavailable");
                    esp_lv_adapter_unlock();
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            xSemaphoreGive(app->_detect_frame_free_sem);
        }

        if (loaded_mode != requested_mode)
        {
            delete_pedestrian_detect();
            delete_humanface_detect();
            app->clearDetectResults();

            bool loaded = false;
            if (requested_mode == DETECT_MODE_PEDESTRIAN)
            {
                loaded = get_pedestrian_detect() != NULL;
            }
            else
            {
                loaded = get_humanface_detect() != NULL;
            }

            if (!loaded)
            {
                ESP_LOGE(TAG, "Load camera detection model failed");
                xEventGroupClearBits(camera_event_group,
                                     CAMERA_EVENT_PED_DETECT | CAMERA_EVENT_HUMAN_DETECT);
                if (esp_lv_adapter_lock(100) == ESP_OK)
                {
                    app->setModeUi(DETECT_MODE_NORMAL, "AI unavailable");
                    esp_lv_adapter_unlock();
                }
                loaded_mode = DETECT_MODE_NORMAL;
                continue;
            }

            loaded_mode = requested_mode;
            ESP_LOGI(TAG, "%s detection model loaded on demand",
                     loaded_mode == DETECT_MODE_FACE ? "Face" : "Pedestrian");
        }

        if (xSemaphoreTake(app->_detect_frame_ready_sem, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            continue;
        }

        bits = xEventGroupGetBits(camera_event_group);
        camera_detect_mode_t frame_mode = DETECT_MODE_NORMAL;
        if (bits & CAMERA_EVENT_PED_DETECT)
        {
            frame_mode = DETECT_MODE_PEDESTRIAN;
        }
        else if (bits & CAMERA_EVENT_HUMAN_DETECT)
        {
            frame_mode = DETECT_MODE_FACE;
        }

        if ((bits & CAMERA_EVENT_DELETE) || frame_mode != loaded_mode)
        {
            xSemaphoreGive(app->_detect_frame_free_sem);
            continue;
        }

        std::list<dl::detect::result_t> results;
        if (loaded_mode == DETECT_MODE_PEDESTRIAN)
        {
            results = app_pedestrian_detect((uint16_t *)app->_detect_frame_buffer,
                                            app->_hor_res, app->_ver_res);
        }
        else
        {
            results = app_humanface_detect((uint16_t *)app->_detect_frame_buffer,
                                           app->_hor_res, app->_ver_res);
        }

        if (xSemaphoreTake(app->_detect_results_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            app->_detect_result_count = 0;
            app->_detect_result_mode = loaded_mode;
            for (const auto &result : results)
            {
                if (app->_detect_result_count >= CAMERA_DETECT_RESULT_MAX ||
                    result.box.size() < 4)
                {
                    break;
                }

                camera_detect_result_t &output =
                    app->_detect_results[app->_detect_result_count];
                bool valid_box = false;
                for (int index = 0; index < 4; ++index)
                {
                    output.box[index] = result.box[index];
                    valid_box = valid_box || result.box[index] != 0;
                }
                if (!valid_box)
                {
                    continue;
                }

                output.has_keypoints = loaded_mode == DETECT_MODE_FACE &&
                                       result.keypoint.size() >= 10;
                bool valid_keypoints = false;
                if (output.has_keypoints)
                {
                    for (int index = 0; index < 10; ++index)
                    {
                        output.keypoints[index] = result.keypoint[index];
                        valid_keypoints = valid_keypoints || result.keypoint[index] != 0;
                    }
                    output.has_keypoints = valid_keypoints;
                }

                ++app->_detect_result_count;
            }
            xSemaphoreGive(app->_detect_results_mutex);
        }

        xSemaphoreGive(app->_detect_frame_free_sem);
    }

    delete_pedestrian_detect();
    delete_humanface_detect();
    app->clearDetectResults();
    ESP_LOGI(TAG, "Camera detection task exit");
    xSemaphoreGive(app->_detect_task_done_sem);
    vTaskDelete(NULL);
}

void Camera::cameraVideoFrameOperation(uint8_t *camera_buf, uint8_t camera_buf_index,
                                       uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                       size_t camera_buf_len)
{
    (void)camera_buf_index;

    Camera *app = _active_camera;
    if (app == NULL || camera_event_group == NULL)
    {
        return;
    }

    xEventGroupWaitBits(camera_event_group, CAMERA_EVENT_TASK_RUN,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    EventBits_t current_bits = xEventGroupGetBits(camera_event_group);
    camera_detect_mode_t current_mode = DETECT_MODE_NORMAL;
    if (current_bits & CAMERA_EVENT_PED_DETECT)
    {
        current_mode = DETECT_MODE_PEDESTRIAN;
    }
    else if (current_bits & CAMERA_EVENT_HUMAN_DETECT)
    {
        current_mode = DETECT_MODE_FACE;
    }

    if (current_mode != DETECT_MODE_NORMAL &&
        app->_detect_frame_buffer != NULL &&
        camera_buf_len >= app->_detect_frame_size &&
        xSemaphoreTake(app->_detect_frame_free_sem, 0) == pdTRUE)
    {
        memcpy(app->_detect_frame_buffer, camera_buf, app->_detect_frame_size);
        xSemaphoreGive(app->_detect_frame_ready_sem);
    }

    if (current_mode != DETECT_MODE_NORMAL &&
        xSemaphoreTake(app->_detect_results_mutex, 0) == pdTRUE)
    {
        if (app->_detect_result_mode == current_mode)
        {
            uint16_t *rgb_buffer = reinterpret_cast<uint16_t *>(camera_buf);
            for (size_t index = 0; index < app->_detect_result_count; ++index)
            {
                const camera_detect_result_t &result = app->_detect_results[index];
                draw_rectangle_rgb(rgb_buffer, camera_buf_hes, camera_buf_ves,
                                   result.box[0], result.box[1],
                                   result.box[2], result.box[3],
                                   0, 0, 255, 0, 0, 3);
                if (current_mode == DETECT_MODE_FACE && result.has_keypoints)
                {
                    camera_draw_green_points(rgb_buffer, camera_buf_hes,
                                             camera_buf_ves, result.keypoints);
                }
            }
        }
        xSemaphoreGive(app->_detect_results_mutex);
    }

    // Update display if not in delete state
    if (!(current_bits & CAMERA_EVENT_DELETE) && (esp_lv_adapter_lock(100) == ESP_OK))
    {
        if (ui_ImageCameraShotImage)
        {
            lv_canvas_set_buffer(ui_ImageCameraShotImage, camera_buf,
                               camera_buf_hes, camera_buf_ves,
                               LV_IMG_CF_TRUE_COLOR);
        }
        lv_refr_now(NULL);
        esp_lv_adapter_unlock();
    }
}
