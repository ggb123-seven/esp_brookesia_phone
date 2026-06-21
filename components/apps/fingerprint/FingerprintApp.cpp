/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "FingerprintApp.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "sdkconfig.h"

#define FINGERPRINT_WORKER_STACK_SIZE      (5120)
#define FINGERPRINT_WORKER_PRIORITY        (5)
#define FINGERPRINT_COMMAND_QUEUE_LEN      (4)
#define FINGERPRINT_CLOSE_WAIT_MS          (30000)
#define FINGERPRINT_UI_LOCK_WAIT_MS        (200)

#define FINGERPRINT_COLOR_BG               0x101820
#define FINGERPRINT_COLOR_PANEL            0x1B2730
#define FINGERPRINT_COLOR_PANEL_SOFT       0x22323D
#define FINGERPRINT_COLOR_PRIMARY          0x36D399
#define FINGERPRINT_COLOR_ACCENT           0x60A5FA
#define FINGERPRINT_COLOR_WARN             0xFBBF24
#define FINGERPRINT_COLOR_ERROR            0xF87171
#define FINGERPRINT_COLOR_TEXT             0xF8FAFC
#define FINGERPRINT_COLOR_MUTED            0x94A3B8
#define FINGERPRINT_FONT_CN                (&fingerprint_font_20)

LV_FONT_DECLARE(fingerprint_font_20);
LV_IMG_DECLARE(img_app_fingerprint);

static const char *TAG = "FingerprintApp";

static const char *store_status_to_text(student_store_status_t status)
{
    switch (status) {
    case STUDENT_STORE_STATUS_OK:
        return "正常";
    case STUDENT_STORE_STATUS_STUDENT_NOT_FOUND:
        return "学生不存在";
    case STUDENT_STORE_STATUS_PAGE_ID_UNBOUND:
        return "模板未绑定";
    case STUDENT_STORE_STATUS_DUPLICATE_STUDENT_ID:
        return "学号重复";
    case STUDENT_STORE_STATUS_PAGE_ID_OCCUPIED:
        return "模板已占用";
    case STUDENT_STORE_STATUS_STORE_FULL:
        return "名单已满";
    case STUDENT_STORE_STATUS_PARSE_ERROR:
        return "名单格式错误";
    case STUDENT_STORE_STATUS_STORAGE_ERROR:
        return "名单存储错误";
    default:
        return "未知错误";
    }
}

FingerprintApp::FingerprintApp():
    ESP_Brookesia_PhoneApp("指纹识别", &img_app_fingerprint, true),
    _busy(false),
    _closing(false),
    _service_ready(false),
    _store_ready(false),
    _command_queue(NULL),
    _worker_done(NULL),
    _worker_task(NULL),
    _page(PAGE_MAIN),
    _student_count(0),
    _has_selected_student(false),
    _root(NULL),
    _status_label(NULL),
    _detail_label(NULL),
    _result_label(NULL),
    _student_list(NULL),
    _selected_label(NULL),
    _action_btn(NULL),
    _filter_ta(NULL),
    _keyboard(NULL)
{
    memset(&_selected_student, 0, sizeof(_selected_student));
}

FingerprintApp::~FingerprintApp()
{
}

bool FingerprintApp::init(void)
{
    return true;
}

bool FingerprintApp::run(void)
{
    _closing = false;
    _busy = false;
    _page = PAGE_MAIN;
    buildUi();

    if (!initService()) {
        showMainPage();
        setStatusText("模块离线", "AS608 指纹模块初始化失败。", FINGERPRINT_COLOR_ERROR);
        setControlsEnabled(false);
        return true;
    }

    if (!initStudentStore()) {
        showMainPage();
        setStatusText("名单错误", "学生名单加载失败，请检查 SPIFFS 名单文件。", FINGERPRINT_COLOR_ERROR);
        setControlsEnabled(false);
        as608_service_deinit();
        _service_ready = false;
        return true;
    }

    if (!startWorker()) {
        showMainPage();
        setStatusText("任务错误", "指纹后台任务启动失败。", FINGERPRINT_COLOR_ERROR);
        setControlsEnabled(false);
        student_store_deinit();
        _store_ready = false;
        as608_service_deinit();
        _service_ready = false;
        return true;
    }

    showMainPage();
    return true;
}

bool FingerprintApp::back(void)
{
    if (_page != PAGE_MAIN && !_busy) {
        showMainPage();
        return true;
    }

    notifyCoreClosed();
    return true;
}

bool FingerprintApp::close(void)
{
    if (!stopWorker()) {
        return false;
    }

    if (_store_ready) {
        student_store_deinit();
        _store_ready = false;
    }

    if (_service_ready) {
        esp_err_t err = as608_service_deinit();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to deinit AS608 service: %s", esp_err_to_name(err));
            return false;
        }
        _service_ready = false;
    }

    _root = NULL;
    _status_label = NULL;
    _detail_label = NULL;
    _result_label = NULL;
    _student_list = NULL;
    _selected_label = NULL;
    _action_btn = NULL;
    _filter_ta = NULL;
    _keyboard = NULL;
    _student_count = 0;
    _has_selected_student = false;

    return true;
}

bool FingerprintApp::initService(void)
{
    const as608_service_config_t config = {
        .port = {
            .uart_num = (uart_port_t)CONFIG_EXAMPLE_AS608_UART_NUM,
            .tx_io = CONFIG_EXAMPLE_AS608_TX_GPIO,
            .rx_io = CONFIG_EXAMPLE_AS608_RX_GPIO,
            .baud_rate = CONFIG_EXAMPLE_AS608_BAUD_RATE,
            .rx_buffer_size = AS608_PORT_DEFAULT_RX_BUFFER_SIZE,
            .read_timeout_ms = CONFIG_EXAMPLE_AS608_READ_TIMEOUT_MS,
        },
        .address = CONFIG_EXAMPLE_AS608_ADDRESS,
    };

    esp_err_t err = as608_service_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init AS608 service: %s", esp_err_to_name(err));
        _service_ready = false;
        return false;
    }

    _service_ready = true;
    return true;
}

bool FingerprintApp::initStudentStore(void)
{
    esp_err_t err = student_store_init(STUDENT_STORE_DEFAULT_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init student store: %s", esp_err_to_name(err));
        _store_ready = false;
        return false;
    }

    _store_ready = true;
    return true;
}

bool FingerprintApp::startWorker(void)
{
    if (_worker_task != NULL) {
        return true;
    }

    _command_queue = xQueueCreate(FINGERPRINT_COMMAND_QUEUE_LEN, sizeof(Command));
    if (_command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return false;
    }

    _worker_done = xSemaphoreCreateBinary();
    if (_worker_done == NULL) {
        ESP_LOGE(TAG, "Failed to create worker done semaphore");
        vQueueDelete(_command_queue);
        _command_queue = NULL;
        return false;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(workerTask, "Fingerprint", FINGERPRINT_WORKER_STACK_SIZE,
                                            this, FINGERPRINT_WORKER_PRIORITY, &_worker_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create worker task");
        vSemaphoreDelete(_worker_done);
        vQueueDelete(_command_queue);
        _worker_done = NULL;
        _command_queue = NULL;
        _worker_task = NULL;
        return false;
    }

    return true;
}

bool FingerprintApp::stopWorker(void)
{
    if (_worker_task == NULL) {
        return true;
    }

    _closing = true;
    setControlsEnabled(false);

    const Command command = {
        .type = COMMAND_EXIT,
        .page_id = 0,
        .student_id = {0},
    };
    if (xQueueSend(_command_queue, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send worker exit command");
        return false;
    }

    if (xSemaphoreTake(_worker_done, pdMS_TO_TICKS(FINGERPRINT_CLOSE_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for fingerprint worker to stop");
        return false;
    }

    vSemaphoreDelete(_worker_done);
    vQueueDelete(_command_queue);
    _worker_done = NULL;
    _command_queue = NULL;
    _worker_task = NULL;
    _busy = false;
    _closing = false;

    return true;
}

bool FingerprintApp::postCommand(CommandType type, uint16_t page_id, const char *student_id)
{
    if (!_service_ready || !_store_ready || _command_queue == NULL || _busy) {
        return false;
    }

    _busy = true;
    setControlsEnabled(false);

    Command command = {
        .type = type,
        .page_id = page_id,
        .student_id = {0},
    };
    if (student_id != NULL) {
        snprintf(command.student_id, sizeof(command.student_id), "%s", student_id);
    }

    if (xQueueSend(_command_queue, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
        _busy = false;
        setControlsEnabled(true);
        setStatusText("操作失败", "后台任务队列已满，请稍后再试。", FINGERPRINT_COLOR_ERROR);
        return false;
    }

    return true;
}

void FingerprintApp::buildUi(void)
{
    lv_area_t area = getVisualArea();
    const lv_coord_t width = area.x2 - area.x1;
    const lv_coord_t height = area.y2 - area.y1;

    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root, width, height);
    lv_obj_align(_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(FINGERPRINT_COLOR_BG), 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 22, 0);
    lv_obj_set_style_pad_row(_root, 12, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
}

void FingerprintApp::clearPage(void)
{
    if (_root != NULL) {
        lv_obj_clean(_root);
    }

    _status_label = NULL;
    _detail_label = NULL;
    _result_label = NULL;
    _student_list = NULL;
    _selected_label = NULL;
    _action_btn = NULL;
    _filter_ta = NULL;
    _keyboard = NULL;
    _has_selected_student = false;
}

void FingerprintApp::createHeader(const char *title, bool show_back)
{
    lv_obj_t *header = lv_obj_create(_root);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 52);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_pad_column(header, 14, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (show_back) {
        lv_obj_t *back_btn = createButton(header, "<", backEventCb, 64, 44);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(FINGERPRINT_COLOR_PANEL_SOFT), 0);
    }

    lv_obj_t *label = lv_label_create(header);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FINGERPRINT_COLOR_TEXT), 0);
}

lv_obj_t *FingerprintApp::createButton(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_coord_t width,
                                       lv_coord_t height)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(FINGERPRINT_COLOR_ACCENT), 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, FINGERPRINT_FONT_CN, 0);
    lv_obj_center(label);

    return button;
}

lv_obj_t *FingerprintApp::createPanel(lv_obj_t *parent, lv_coord_t height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, height);
    lv_obj_set_style_bg_color(panel, lv_color_hex(FINGERPRINT_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    return panel;
}

void FingerprintApp::showMainPage(void)
{
    _page = PAGE_MAIN;
    clearPage();
    createHeader("指纹识别", false);

    lv_obj_t *status_panel = createPanel(_root, 112);
    _status_label = lv_label_create(status_panel);
    lv_label_set_text(_status_label, "准备就绪");
    lv_obj_set_style_text_font(_status_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(FINGERPRINT_COLOR_PRIMARY), 0);

    _detail_label = lv_label_create(status_panel);
    lv_label_set_text(_detail_label, "可选择学生录入、识别学生指纹或删除学生指纹。");
    lv_obj_set_width(_detail_label, LV_PCT(100));
    lv_label_set_long_mode(_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_detail_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_detail_label, lv_color_hex(FINGERPRINT_COLOR_MUTED), 0);

    lv_obj_t *button_row = lv_obj_create(_root);
    lv_obj_set_width(button_row, LV_PCT(100));
    lv_obj_set_flex_grow(button_row, 1);
    lv_obj_set_style_bg_opa(button_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button_row, 0, 0);
    lv_obj_set_style_pad_all(button_row, 0, 0);
    lv_obj_set_style_pad_column(button_row, 18, 0);
    lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    createButton(button_row, "导入学生指纹", openEnrollEventCb, 290, 152);
    createButton(button_row, "识别学生指纹", openIdentifyEventCb, 290, 152);
    createButton(button_row, "删除学生指纹", openDeleteEventCb, 290, 152);

    setControlsEnabled(_service_ready && _store_ready && !_busy);
}

void FingerprintApp::showEnrollPage(void)
{
    _page = PAGE_ENROLL;
    createStudentFlowPage("导入学生指纹", false);
    setStatusText("选择学生", "输入姓名筛选已导入名单，选择学生后开始录入。", FINGERPRINT_COLOR_PRIMARY);
}

void FingerprintApp::showIdentifyPage(void)
{
    _page = PAGE_IDENTIFY;
    clearPage();
    createHeader("识别学生指纹", true);

    lv_obj_t *status_panel = createPanel(_root, 132);
    _status_label = lv_label_create(status_panel);
    lv_label_set_text(_status_label, "等待开始");
    lv_obj_set_style_text_font(_status_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(FINGERPRINT_COLOR_PRIMARY), 0);

    _detail_label = lv_label_create(status_panel);
    lv_label_set_text(_detail_label, "点击开始识别后，再把手指放到 AS608 指纹模块上。");
    lv_obj_set_width(_detail_label, LV_PCT(100));
    lv_label_set_long_mode(_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_detail_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_detail_label, lv_color_hex(FINGERPRINT_COLOR_MUTED), 0);

    lv_obj_t *result_panel = createPanel(_root, 180);
    _result_label = lv_label_create(result_panel);
    lv_label_set_text(_result_label, "暂无识别结果。");
    lv_obj_set_width(_result_label, LV_PCT(100));
    lv_label_set_long_mode(_result_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_result_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_result_label, lv_color_hex(FINGERPRINT_COLOR_TEXT), 0);

    createButton(_root, "开始识别", startIdentifyEventCb, LV_PCT(100), 72);
    setControlsEnabled(!_busy);
}

void FingerprintApp::showDeletePage(void)
{
    _page = PAGE_DELETE;
    createStudentFlowPage("删除学生指纹", true);
    setStatusText("选择学生", "只显示已绑定指纹的学生，选择后删除模板和本地绑定。", FINGERPRINT_COLOR_WARN);
}

void FingerprintApp::createStudentFlowPage(const char *title, bool bound_records)
{
    clearPage();
    createHeader(title, true);

    lv_obj_t *status_panel = createPanel(_root, 76);
    _status_label = lv_label_create(status_panel);
    lv_label_set_text(_status_label, "准备就绪");
    lv_obj_set_style_text_font(_status_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(FINGERPRINT_COLOR_PRIMARY), 0);

    _detail_label = lv_label_create(status_panel);
    lv_label_set_text(_detail_label, "正在加载学生名单。");
    lv_obj_set_width(_detail_label, LV_PCT(100));
    lv_label_set_long_mode(_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_detail_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_detail_label, lv_color_hex(FINGERPRINT_COLOR_MUTED), 0);

    _filter_ta = lv_textarea_create(_root);
    lv_obj_set_width(_filter_ta, LV_PCT(100));
    lv_obj_set_height(_filter_ta, 48);
    lv_textarea_set_one_line(_filter_ta, true);
    lv_textarea_set_placeholder_text(_filter_ta, "输入学生姓名筛选");
    lv_obj_set_style_text_font(_filter_ta, FINGERPRINT_FONT_CN, 0);
    lv_obj_add_event_cb(_filter_ta, filterEventCb, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *content = lv_obj_create(_root);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_column(content, 14, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);

    _student_list = lv_obj_create(content);
    lv_obj_set_width(_student_list, LV_PCT(62));
    lv_obj_set_height(_student_list, LV_PCT(100));
    lv_obj_set_style_bg_color(_student_list, lv_color_hex(FINGERPRINT_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(_student_list, 0, 0);
    lv_obj_set_style_radius(_student_list, 8, 0);
    lv_obj_set_style_pad_all(_student_list, 10, 0);
    lv_obj_set_style_pad_row(_student_list, 8, 0);
    lv_obj_set_flex_flow(_student_list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *selected_panel = lv_obj_create(content);
    lv_obj_set_width(selected_panel, LV_PCT(38));
    lv_obj_set_height(selected_panel, LV_PCT(100));
    lv_obj_set_style_bg_color(selected_panel, lv_color_hex(FINGERPRINT_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(selected_panel, 0, 0);
    lv_obj_set_style_radius(selected_panel, 8, 0);
    lv_obj_set_style_pad_all(selected_panel, 16, 0);
    lv_obj_set_style_pad_row(selected_panel, 12, 0);
    lv_obj_set_flex_flow(selected_panel, LV_FLEX_FLOW_COLUMN);

    _selected_label = lv_label_create(selected_panel);
    lv_label_set_text(_selected_label, "未选择学生");
    lv_obj_set_width(_selected_label, LV_PCT(100));
    lv_label_set_long_mode(_selected_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_selected_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_selected_label, lv_color_hex(FINGERPRINT_COLOR_TEXT), 0);

    _action_btn = createButton(selected_panel, bound_records ? "确认删除" : "开始录入",
                               bound_records ? startDeleteEventCb : startEnrollEventCb, LV_PCT(100), 64);
    lv_obj_add_state(_action_btn, LV_STATE_DISABLED);

    _keyboard = lv_keyboard_create(_root);
    lv_obj_set_width(_keyboard, LV_PCT(100));
    lv_obj_set_height(_keyboard, 180);
    lv_keyboard_set_textarea(_keyboard, _filter_ta);
    lv_obj_add_event_cb(_keyboard, keyboardEventCb, LV_EVENT_CLICKED, this);

    refreshStudentList(bound_records);
}

void FingerprintApp::loadStudentsForList(void)
{
    _student_count = 0;
    if (!_store_ready) {
        return;
    }

    student_store_status_t status = STUDENT_STORE_STATUS_OK;
    size_t count = 0;
    esp_err_t err = student_store_list(_students, STUDENT_STORE_MAX_RECORDS, &count, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to list students: %s (%s)", esp_err_to_name(err), student_store_status_to_text(status));
        return;
    }
    _student_count = (count > STUDENT_STORE_MAX_RECORDS) ? STUDENT_STORE_MAX_RECORDS : count;
}

bool FingerprintApp::recordMatchesFilter(const student_store_record_t &record, bool bound_records,
                                         const char *filter) const
{
    bool is_bound = record.fingerprint_page_id != STUDENT_STORE_NO_PAGE_ID;
    if (bound_records != is_bound || !record.enabled) {
        return false;
    }
    if (filter == NULL || filter[0] == '\0') {
        return true;
    }
    return strstr(record.name, filter) != NULL || strstr(record.student_id, filter) != NULL ||
           strstr(record.class_name, filter) != NULL;
}

void FingerprintApp::refreshStudentList(bool bound_records)
{
    if (_student_list == NULL) {
        return;
    }

    lv_obj_clean(_student_list);
    loadStudentsForList();

    const char *filter = (_filter_ta != NULL) ? lv_textarea_get_text(_filter_ta) : "";
    size_t visible_count = 0;

    for (size_t i = 0; i < _student_count; ++i) {
        if (!recordMatchesFilter(_students[i], bound_records, filter)) {
            continue;
        }

        lv_obj_t *row = lv_btn_create(_student_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 72);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(FINGERPRINT_COLOR_PANEL_SOFT), 0);
        lv_obj_set_user_data(row, (void *)i);
        lv_obj_add_event_cb(row, studentRowEventCb, LV_EVENT_CLICKED, this);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text_fmt(label, "%s\n%s  %s", _students[i].name, _students[i].student_id,
                              _students[i].class_name);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(label, FINGERPRINT_FONT_CN, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(FINGERPRINT_COLOR_TEXT), 0);
        lv_obj_center(label);
        ++visible_count;
    }

    if (visible_count == 0) {
        lv_obj_t *label = lv_label_create(_student_list);
        lv_label_set_text(label, bound_records ? "没有匹配的已绑定学生。" : "未找到学生，请先导入或更新名单。");
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(label, FINGERPRINT_FONT_CN, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(FINGERPRINT_COLOR_MUTED), 0);
    }
}

void FingerprintApp::selectStudent(size_t index)
{
    if (index >= _student_count) {
        return;
    }
    _selected_student = _students[index];
    _has_selected_student = true;
    refreshSelectedPanel();
}

void FingerprintApp::refreshSelectedPanel(void)
{
    if (_selected_label == NULL || _action_btn == NULL) {
        return;
    }

    if (!_has_selected_student) {
        lv_label_set_text(_selected_label, "未选择学生");
        lv_obj_add_state(_action_btn, LV_STATE_DISABLED);
        return;
    }

    if (_selected_student.fingerprint_page_id == STUDENT_STORE_NO_PAGE_ID) {
        lv_label_set_text_fmt(_selected_label, "姓名：%s\n学号：%s\n班级：%s\n模板：未绑定",
                              _selected_student.name, _selected_student.student_id,
                              _selected_student.class_name);
    } else {
        lv_label_set_text_fmt(_selected_label, "姓名：%s\n学号：%s\n班级：%s\n模板：%u",
                              _selected_student.name, _selected_student.student_id,
                              _selected_student.class_name, _selected_student.fingerprint_page_id);
    }

    if (!_busy) {
        lv_obj_clear_state(_action_btn, LV_STATE_DISABLED);
    }
}

void FingerprintApp::setControlsEnabled(bool enabled)
{
    if (_root != NULL) {
        setObjectTreeEnabled(_root, enabled);
    }
    if (_action_btn != NULL && !_has_selected_student) {
        lv_obj_add_state(_action_btn, LV_STATE_DISABLED);
    }
}

void FingerprintApp::setObjectTreeEnabled(lv_obj_t *obj, bool enabled)
{
    if (obj == NULL) {
        return;
    }

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        if (enabled) {
            lv_obj_clear_state(obj, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(obj, LV_STATE_DISABLED);
        }
    }

    uint32_t child_count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_count; ++i) {
        setObjectTreeEnabled(lv_obj_get_child(obj, i), enabled);
    }
}

void FingerprintApp::setStatusText(const char *state, const char *detail, uint32_t color)
{
    if (_status_label != NULL) {
        lv_label_set_text(_status_label, state);
        lv_obj_set_style_text_color(_status_label, lv_color_hex(color), 0);
    }
    if (_detail_label != NULL) {
        lv_label_set_text(_detail_label, detail);
    }
}

void FingerprintApp::setResultText(const char *text, uint32_t color)
{
    if (_result_label != NULL) {
        lv_label_set_text(_result_label, text);
        lv_obj_set_style_text_color(_result_label, lv_color_hex(color), 0);
    }
}

void FingerprintApp::showResult(const OperationResult &result)
{
    char detail[192];

    if (result.err != ESP_OK) {
        snprintf(detail, sizeof(detail), "%s，模块状态：%s。", esp_err_to_name(result.err),
                 statusToText(result.status));
        if (result.status == AS608_STATUS_NO_FINGERPRINT) {
            setStatusText("未检测到手指", detail, FINGERPRINT_COLOR_WARN);
        } else if ((result.status == AS608_STATUS_NOT_FOUND) || (result.status == AS608_STATUS_NOT_MATCH)) {
            setStatusText("未匹配到指纹", detail, FINGERPRINT_COLOR_WARN);
        } else {
            setStatusText("操作失败", detail, FINGERPRINT_COLOR_ERROR);
        }
        setResultText(detail, FINGERPRINT_COLOR_WARN);
        return;
    }

    switch (result.type) {
    case COMMAND_IDENTIFY:
        if (result.has_record) {
            snprintf(detail, sizeof(detail), "姓名：%s\n学号：%s\n班级：%s\n模板：%u\n分数：%u",
                     result.record.name, result.record.student_id, result.record.class_name,
                     result.page_id, result.score);
            setStatusText("识别成功", "已匹配到学生档案。", FINGERPRINT_COLOR_PRIMARY);
            setResultText(detail, FINGERPRINT_COLOR_TEXT);
        } else {
            snprintf(detail, sizeof(detail), "匹配到模板 %u，得分 %u，但该模板未绑定学生。", result.page_id,
                     result.score);
            setStatusText("模板未绑定", detail, FINGERPRINT_COLOR_WARN);
            setResultText(detail, FINGERPRINT_COLOR_WARN);
        }
        break;
    case COMMAND_ENROLL:
        if (result.store_err == ESP_OK && result.has_record) {
            snprintf(detail, sizeof(detail), "已为 %s 绑定模板 %u，得分 %u。", result.record.name,
                     result.page_id, result.score);
            setStatusText("录入成功", detail, FINGERPRINT_COLOR_PRIMARY);
            setResultText(detail, FINGERPRINT_COLOR_TEXT);
            refreshStudentList(false);
            _has_selected_student = false;
            refreshSelectedPanel();
        } else {
            snprintf(detail, sizeof(detail), "模板 %u 已录入，但绑定失败：%s。", result.page_id,
                     store_status_to_text(result.store_status));
            setStatusText("绑定失败", detail, FINGERPRINT_COLOR_ERROR);
            setResultText(detail, FINGERPRINT_COLOR_ERROR);
        }
        break;
    case COMMAND_DELETE:
        if (result.store_err == ESP_OK) {
            snprintf(detail, sizeof(detail), "已删除模板 %u，并解除本地绑定。", result.page_id);
            setStatusText("删除成功", detail, FINGERPRINT_COLOR_PRIMARY);
            setResultText(detail, FINGERPRINT_COLOR_TEXT);
            refreshStudentList(true);
            _has_selected_student = false;
            refreshSelectedPanel();
        } else {
            snprintf(detail, sizeof(detail), "已删除模板 %u，本地解绑异常：%s。", result.page_id,
                     store_status_to_text(result.store_status));
            setStatusText("删除完成", detail, FINGERPRINT_COLOR_WARN);
            setResultText(detail, FINGERPRINT_COLOR_WARN);
        }
        break;
    default:
        break;
    }
}

void FingerprintApp::updateFromWorker(const OperationResult &result)
{
    _busy = false;

    if (_closing) {
        return;
    }

    if (esp_lv_adapter_lock(pdMS_TO_TICKS(FINGERPRINT_UI_LOCK_WAIT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Skip UI update because LVGL lock timed out");
        return;
    }

    showResult(result);
    setControlsEnabled(_service_ready && _store_ready && !_busy && !_closing);
    lv_refr_now(NULL);
    esp_lv_adapter_unlock();
}

void FingerprintApp::updateEnrollHint(as608_service_enroll_event_t event)
{
    if (_closing || _root == NULL) {
        return;
    }

    if (esp_lv_adapter_lock(pdMS_TO_TICKS(FINGERPRINT_UI_LOCK_WAIT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Skip enroll hint because LVGL lock timed out");
        return;
    }

    switch (event) {
    case AS608_SERVICE_ENROLL_PUT_FINGER:
        setStatusText("录入中", "请把手指放到 AS608 指纹模块上。", FINGERPRINT_COLOR_WARN);
        break;
    case AS608_SERVICE_ENROLL_PUT_FINGER_AGAIN:
        setStatusText("再次放置", "请抬起手指，再放同一个手指。", FINGERPRINT_COLOR_WARN);
        break;
    case AS608_SERVICE_ENROLL_FEATURE_OK:
        setStatusText("正在保存", "特征采集完成，正在保存模板。", FINGERPRINT_COLOR_WARN);
        break;
    default:
        setStatusText("录入失败", "指纹采集失败，请重新尝试。", FINGERPRINT_COLOR_ERROR);
        break;
    }

    lv_refr_now(NULL);
    esp_lv_adapter_unlock();
}

void FingerprintApp::workerTask(void *arg)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(arg);
    Command command;

    while (xQueueReceive(app->_command_queue, &command, portMAX_DELAY) == pdTRUE) {
        if (command.type == COMMAND_EXIT) {
            break;
        }

        OperationResult result = {
            .type = command.type,
            .err = ESP_FAIL,
            .store_err = ESP_OK,
            .page_id = command.page_id,
            .score = 0,
            .status = AS608_STATUS_UNKNOWN,
            .store_status = STUDENT_STORE_STATUS_OK,
            .record = {},
            .has_record = false,
        };

        switch (command.type) {
        case COMMAND_IDENTIFY:
            result.err = as608_service_identify(&result.page_id, &result.score, &result.status);
            if (result.err == ESP_OK) {
                result.store_err = student_store_get_by_page_id(result.page_id, &result.record,
                                                                &result.store_status);
                result.has_record = result.store_err == ESP_OK;
            }
            break;
        case COMMAND_ENROLL:
            result.err = as608_service_enroll(enrollStatusCb, app, &result.page_id, &result.score,
                                              &result.status);
            if (result.err == ESP_OK) {
                result.store_err = student_store_bind_page_id(command.student_id, result.page_id,
                                                              &result.store_status);
                if (result.store_err == ESP_OK) {
                    result.store_err = student_store_get_by_id(command.student_id, &result.record,
                                                               &result.store_status);
                    result.has_record = result.store_err == ESP_OK;
                }
            }
            break;
        case COMMAND_DELETE:
            result.store_err = student_store_get_by_id(command.student_id, &result.record, &result.store_status);
            result.has_record = result.store_err == ESP_OK;
            result.err = as608_service_delete_template(command.page_id, &result.status);
            if (result.err == ESP_OK) {
                result.store_err = student_store_unbind_page_id(command.page_id, &result.store_status);
            }
            break;
        default:
            break;
        }

        app->updateFromWorker(result);
    }

    xSemaphoreGive(app->_worker_done);
    vTaskDelete(NULL);
}

void FingerprintApp::openEnrollEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app != NULL && !app->_busy) {
        app->showEnrollPage();
    }
}

void FingerprintApp::openIdentifyEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app != NULL && !app->_busy) {
        app->showIdentifyPage();
    }
}

void FingerprintApp::openDeleteEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app != NULL && !app->_busy) {
        app->showDeletePage();
    }
}

void FingerprintApp::backEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app != NULL && !app->_busy) {
        app->showMainPage();
    }
}

void FingerprintApp::startIdentifyEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app == NULL) {
        return;
    }

    app->setStatusText("正在识别", "请把手指放到 AS608 指纹模块上。", FINGERPRINT_COLOR_WARN);
    app->setResultText("正在等待指纹。", FINGERPRINT_COLOR_MUTED);
    app->postCommand(COMMAND_IDENTIFY, 0, NULL);
}

void FingerprintApp::startEnrollEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app == NULL || !app->_has_selected_student) {
        return;
    }

    app->setStatusText("准备录入", "请按提示连续放置同一个手指两次。", FINGERPRINT_COLOR_WARN);
    app->setResultText("正在准备录入。", FINGERPRINT_COLOR_MUTED);
    app->postCommand(COMMAND_ENROLL, 0, app->_selected_student.student_id);
}

void FingerprintApp::startDeleteEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app == NULL || !app->_has_selected_student ||
        app->_selected_student.fingerprint_page_id == STUDENT_STORE_NO_PAGE_ID) {
        return;
    }

    app->setStatusText("正在删除", "正在删除选中学生的指纹模板。", FINGERPRINT_COLOR_WARN);
    app->setResultText("正在删除模板并清理本地绑定。", FINGERPRINT_COLOR_MUTED);
    app->postCommand(COMMAND_DELETE, app->_selected_student.fingerprint_page_id,
                     app->_selected_student.student_id);
}

void FingerprintApp::studentRowEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    lv_obj_t *target = lv_event_get_target(e);
    if (app == NULL || target == NULL) {
        return;
    }

    size_t index = (size_t)lv_obj_get_user_data(target);
    app->selectStudent(index);
}

void FingerprintApp::filterEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app == NULL) {
        return;
    }

    if (app->_page == PAGE_ENROLL) {
        app->refreshStudentList(false);
    } else if (app->_page == PAGE_DELETE) {
        app->refreshStudentList(true);
    }
}

void FingerprintApp::keyboardEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    lv_obj_t *target = lv_event_get_target(e);
    if (app == NULL || target == NULL || app->_filter_ta == NULL) {
        return;
    }

    lv_keyboard_set_textarea(target, app->_filter_ta);
    if (lv_keyboard_get_selected_btn(target) == 39) {
        lv_obj_clear_state(app->_filter_ta, LV_STATE_FOCUSED);
    }
}

void FingerprintApp::enrollStatusCb(as608_service_enroll_event_t event, void *user_ctx)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(user_ctx);
    if (app != NULL) {
        app->updateEnrollHint(event);
    }
}

const char *FingerprintApp::statusToText(as608_status_t status)
{
    switch (status) {
    case AS608_STATUS_OK:
        return "正常";
    case AS608_STATUS_NO_FINGERPRINT:
        return "无手指";
    case AS608_STATUS_NOT_MATCH:
        return "不匹配";
    case AS608_STATUS_NOT_FOUND:
        return "未找到";
    case AS608_STATUS_LIB_FULL:
        return "库已满";
    case AS608_STATUS_LIB_DELETE_ERROR:
        return "删除错误";
    case AS608_STATUS_INPUT_ERROR:
        return "输入错误";
    case AS608_STATUS_IMAGE_TOO_DRY:
        return "图像偏干";
    case AS608_STATUS_IMAGE_TOO_WET:
        return "图像偏湿";
    case AS608_STATUS_IMAGE_TOO_CLUTTER:
        return "图像杂乱";
    case AS608_STATUS_IMAGE_TOO_FEW_FEATURE:
        return "特征太少";
    default:
        return "未知";
    }
}
