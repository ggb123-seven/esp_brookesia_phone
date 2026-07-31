/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "app_video.h"
#include "esp_video_init.h"

class Camera: public ESP_Brookesia_PhoneApp
{
public:
    Camera(uint16_t hor_res, uint16_t ver_res);
    ~Camera();

    bool run(void);
    bool pause(void);
    bool resume(void);
    bool back(void);
    bool close(void);

    bool init(void) override;

private:
    enum camera_detect_mode_t
    {
        DETECT_MODE_NORMAL = 0,
        DETECT_MODE_PEDESTRIAN,
        DETECT_MODE_FACE,
    };

    struct camera_detect_result_t
    {
        int box[4];
        int keypoints[10];
        bool has_keypoints;
    };

    static void taskCameraInit(void *arg);
    static void cameraDetectTask(void *arg);
    static void cameraVideoFrameOperation(uint8_t *camera_buf, uint8_t camera_buf_index,
                                          uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                          size_t camera_buf_len);
    static void onScreenCameraShotBtnClick(lv_event_t *e);
    static void onScreenCameraShotAlbumClick(lv_event_t *e);
    static void onModeSwitchClick(lv_event_t *e);

    bool startDetectTask(void);
    void stopDetectTask(void);
    void setModeUi(camera_detect_mode_t mode, const char *status_text = nullptr);
    void clearDetectResults(void);

    uint16_t _hor_res;
    uint16_t _ver_res;
    uint8_t *_img_album_buffer;
    SemaphoreHandle_t _camera_init_sem;
    esp_err_t _camera_init_result;
    int _camera_ctlr_handle;
    bool _camera_stream_running;
    lv_img_dsc_t _img_refresh_dsc;
    lv_img_dsc_t _img_album_dsc;
    lv_img_dsc_t _img_photo_dsc;
    lv_obj_t *_img_album;
    lv_obj_t *_mode_switch_label;
    uint8_t *_detect_frame_buffer;
    size_t _detect_frame_size;
    SemaphoreHandle_t _detect_frame_free_sem;
    SemaphoreHandle_t _detect_frame_ready_sem;
    SemaphoreHandle_t _detect_results_mutex;
    SemaphoreHandle_t _detect_task_done_sem;
    TaskHandle_t _detect_task_handle;
    camera_detect_result_t _detect_results[10];
    size_t _detect_result_count;
    camera_detect_mode_t _detect_result_mode;
    uint8_t *_cam_buffer[EXAMPLE_CAM_BUF_NUM];
    size_t _cam_buffer_size[EXAMPLE_CAM_BUF_NUM];

    static Camera *_active_camera;
};
