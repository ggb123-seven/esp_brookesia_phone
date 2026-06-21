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
#include "student_store.h"

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
        PAGE_MAIN,
        PAGE_ENROLL,
        PAGE_IDENTIFY,
        PAGE_DELETE,
    } PageMode;

    typedef enum {
        COMMAND_IDENTIFY,
        COMMAND_ENROLL,
        COMMAND_DELETE,
        COMMAND_EXIT,
    } CommandType;

    typedef struct {
        CommandType type;
        uint16_t page_id;
        char student_id[STUDENT_STORE_STUDENT_ID_LEN];
    } Command;

    typedef struct {
        CommandType type;
        esp_err_t err;
        esp_err_t store_err;
        uint16_t page_id;
        uint16_t score;
        as608_status_t status;
        student_store_status_t store_status;
        student_store_record_t record;
        bool has_record;
    } OperationResult;

    bool initService(void);
    bool initStudentStore(void);
    bool startWorker(void);
    bool stopWorker(void);
    bool postCommand(CommandType type, uint16_t page_id, const char *student_id);

    void buildUi(void);
    void clearPage(void);
    void showMainPage(void);
    void showEnrollPage(void);
    void showIdentifyPage(void);
    void showDeletePage(void);
    void createHeader(const char *title, bool show_back);
    lv_obj_t *createButton(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_coord_t width,
                           lv_coord_t height);
    lv_obj_t *createPanel(lv_obj_t *parent, lv_coord_t height);
    void createStudentFlowPage(const char *title, bool bound_records);
    void refreshStudentList(bool bound_records);
    void selectStudent(size_t index);
    void refreshSelectedPanel(void);
    bool recordMatchesFilter(const student_store_record_t &record, bool bound_records, const char *filter) const;
    void setControlsEnabled(bool enabled);
    void setObjectTreeEnabled(lv_obj_t *obj, bool enabled);
    void setStatusText(const char *state, const char *detail, uint32_t color);
    void setResultText(const char *text, uint32_t color);
    void showResult(const OperationResult &result);
    void updateFromWorker(const OperationResult &result);
    void updateEnrollHint(as608_service_enroll_event_t event);
    void loadStudentsForList(void);

    static void workerTask(void *arg);
    static void openEnrollEventCb(lv_event_t *e);
    static void openIdentifyEventCb(lv_event_t *e);
    static void openDeleteEventCb(lv_event_t *e);
    static void backEventCb(lv_event_t *e);
    static void startIdentifyEventCb(lv_event_t *e);
    static void startEnrollEventCb(lv_event_t *e);
    static void startDeleteEventCb(lv_event_t *e);
    static void studentRowEventCb(lv_event_t *e);
    static void filterEventCb(lv_event_t *e);
    static void keyboardEventCb(lv_event_t *e);
    static void enrollStatusCb(as608_service_enroll_event_t event, void *user_ctx);
    static const char *statusToText(as608_status_t status);

    volatile bool _busy;
    volatile bool _closing;
    bool _service_ready;
    bool _store_ready;
    QueueHandle_t _command_queue;
    SemaphoreHandle_t _worker_done;
    TaskHandle_t _worker_task;

    PageMode _page;
    student_store_record_t _students[STUDENT_STORE_MAX_RECORDS];
    size_t _student_count;
    student_store_record_t _selected_student;
    bool _has_selected_student;

    lv_obj_t *_root;
    lv_obj_t *_status_label;
    lv_obj_t *_detail_label;
    lv_obj_t *_result_label;
    lv_obj_t *_student_list;
    lv_obj_t *_selected_label;
    lv_obj_t *_action_btn;
    lv_obj_t *_filter_ta;
    lv_obj_t *_keyboard;
};
