/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "as608_service.h"

class FingerprintApp: public ESP_Brookesia_PhoneApp {
public:
    FingerprintApp();
    ~FingerprintApp();

    bool run(void);
    bool back(void);
    bool close(void);
    bool init(void) override;

private:
    typedef enum {
        COMMAND_IDENTIFY,
        COMMAND_ENROLL,
        COMMAND_DELETE,
        COMMAND_EXIT,
    } CommandType;

    typedef struct {
        CommandType type;
        uint16_t page_id;
    } Command;

    typedef struct {
        CommandType type;
        esp_err_t err;
        uint16_t page_id;
        uint16_t score;
        as608_status_t status;
    } OperationResult;

    bool initService(void);
    bool startWorker(void);
    bool stopWorker(void);
    bool postCommand(CommandType type, uint16_t page_id);
    void buildUi(void);
    void setButtonsEnabled(bool enabled);
    void setStatusText(const char *state, const char *detail, uint32_t color);
    void showResult(const OperationResult &result);
    void updateFromWorker(const OperationResult &result);
    void updateEnrollHint(as608_service_enroll_event_t event);

    static void workerTask(void *arg);
    static void identifyEventCb(lv_event_t *e);
    static void enrollEventCb(lv_event_t *e);
    static void deleteEventCb(lv_event_t *e);
    static void spinboxIncrementEventCb(lv_event_t *e);
    static void spinboxDecrementEventCb(lv_event_t *e);
    static void enrollStatusCb(as608_service_enroll_event_t event, void *user_ctx);
    static const char *statusToText(as608_status_t status);
    static const lv_img_dsc_t *getLauncherIcon(void);

    volatile bool _busy;
    volatile bool _closing;
    bool _service_ready;
    QueueHandle_t _command_queue;
    SemaphoreHandle_t _worker_done;
    TaskHandle_t _worker_task;

    lv_obj_t *_root;
    lv_obj_t *_status_label;
    lv_obj_t *_detail_label;
    lv_obj_t *_page_label;
    lv_obj_t *_score_label;
    lv_obj_t *_as608_status_label;
    lv_obj_t *_spinbox;
    lv_obj_t *_identify_btn;
    lv_obj_t *_enroll_btn;
    lv_obj_t *_delete_btn;
    lv_obj_t *_inc_btn;
    lv_obj_t *_dec_btn;
};
