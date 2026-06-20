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

#define FINGERPRINT_WORKER_STACK_SIZE      (4096)
#define FINGERPRINT_WORKER_PRIORITY        (5)
#define FINGERPRINT_COMMAND_QUEUE_LEN      (4)
#define FINGERPRINT_CLOSE_WAIT_MS          (30000)
#define FINGERPRINT_UI_LOCK_WAIT_MS        (200)
#define FINGERPRINT_TEMPLATE_MAX           (299)

#define FINGERPRINT_COLOR_BG               0x111820
#define FINGERPRINT_COLOR_PANEL            0x1D2A33
#define FINGERPRINT_COLOR_PRIMARY          0x36D399
#define FINGERPRINT_COLOR_WARN             0xFBBF24
#define FINGERPRINT_COLOR_ERROR            0xF87171
#define FINGERPRINT_COLOR_TEXT             0xF8FAFC
#define FINGERPRINT_COLOR_MUTED            0x94A3B8
#define FINGERPRINT_FONT_CN                (&fingerprint_font_20)

LV_FONT_DECLARE(fingerprint_font_20);

static const char *TAG = "FingerprintApp";

static uint8_t s_icon_data[32 * 32 * LV_IMG_PX_SIZE_ALPHA_BYTE];
static lv_img_dsc_t s_icon_dsc = {
    .header = {
        .cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
        .always_zero = 0,
        .reserved = 0,
        .w = 32,
        .h = 32,
    },
    .data_size = sizeof(s_icon_data),
    .data = s_icon_data,
};
static bool s_icon_ready;

FingerprintApp::FingerprintApp():
    ESP_Brookesia_PhoneApp("指纹识别", getLauncherIcon(), true),
    _busy(false),
    _closing(false),
    _service_ready(false),
    _command_queue(NULL),
    _worker_done(NULL),
    _worker_task(NULL),
    _root(NULL),
    _status_label(NULL),
    _detail_label(NULL),
    _page_label(NULL),
    _score_label(NULL),
    _as608_status_label(NULL),
    _spinbox(NULL),
    _identify_btn(NULL),
    _enroll_btn(NULL),
    _delete_btn(NULL),
    _inc_btn(NULL),
    _dec_btn(NULL)
{
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
    buildUi();

    if (!initService()) {
        setButtonsEnabled(false);
        return true;
    }

    if (!startWorker()) {
        setStatusText("任务错误", "指纹后台任务启动失败。", FINGERPRINT_COLOR_ERROR);
        setButtonsEnabled(false);
        as608_service_deinit();
        _service_ready = false;
        return true;
    }

    setStatusText("准备就绪", "请放置手指进行识别，或录入新的指纹模板。", FINGERPRINT_COLOR_PRIMARY);
    setButtonsEnabled(true);

    return true;
}

bool FingerprintApp::back(void)
{
    notifyCoreClosed();

    return true;
}

bool FingerprintApp::close(void)
{
    if (!stopWorker()) {
        return false;
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
    _page_label = NULL;
    _score_label = NULL;
    _as608_status_label = NULL;
    _spinbox = NULL;
    _identify_btn = NULL;
    _enroll_btn = NULL;
    _delete_btn = NULL;
    _inc_btn = NULL;
    _dec_btn = NULL;

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
        setStatusText("模块离线", esp_err_to_name(err), FINGERPRINT_COLOR_ERROR);
        _service_ready = false;
        return false;
    }

    _service_ready = true;
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
    setButtonsEnabled(false);

    const Command command = {
        .type = COMMAND_EXIT,
        .page_id = 0,
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

bool FingerprintApp::postCommand(CommandType type, uint16_t page_id)
{
    if (!_service_ready || _command_queue == NULL || _busy) {
        return false;
    }

    _busy = true;
    setButtonsEnabled(false);

    const Command command = {
        .type = type,
        .page_id = page_id,
    };

    if (xQueueSend(_command_queue, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
        _busy = false;
        setButtonsEnabled(true);
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
    lv_obj_set_style_pad_all(_root, 28, 0);
    lv_obj_set_style_pad_row(_root, 20, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, "指纹识别");
    lv_obj_set_style_text_font(title, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FINGERPRINT_COLOR_TEXT), 0);

    lv_obj_t *status_panel = lv_obj_create(_root);
    lv_obj_set_width(status_panel, LV_PCT(100));
    lv_obj_set_height(status_panel, height / 4);
    lv_obj_set_style_bg_color(status_panel, lv_color_hex(FINGERPRINT_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(status_panel, 0, 0);
    lv_obj_set_style_radius(status_panel, 8, 0);
    lv_obj_set_style_pad_all(status_panel, 20, 0);
    lv_obj_set_flex_flow(status_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    _status_label = lv_label_create(status_panel);
    lv_label_set_text(_status_label, "正在启动");
    lv_obj_set_style_text_font(_status_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(FINGERPRINT_COLOR_WARN), 0);

    _detail_label = lv_label_create(status_panel);
    lv_label_set_text(_detail_label, "正在准备 AS608 指纹模块。");
    lv_obj_set_width(_detail_label, LV_PCT(100));
    lv_label_set_long_mode(_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_detail_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_detail_label, lv_color_hex(FINGERPRINT_COLOR_MUTED), 0);

    lv_obj_t *result_row = lv_obj_create(_root);
    lv_obj_set_width(result_row, LV_PCT(100));
    lv_obj_set_height(result_row, 96);
    lv_obj_set_style_bg_opa(result_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(result_row, 0, 0);
    lv_obj_set_style_pad_all(result_row, 0, 0);
    lv_obj_set_style_pad_column(result_row, 16, 0);
    lv_obj_set_flex_flow(result_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(result_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    _page_label = lv_label_create(result_row);
    lv_label_set_text(_page_label, "模板: --");
    lv_obj_set_style_text_font(_page_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_page_label, lv_color_hex(FINGERPRINT_COLOR_TEXT), 0);

    _score_label = lv_label_create(result_row);
    lv_label_set_text(_score_label, "分数: --");
    lv_obj_set_style_text_font(_score_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(FINGERPRINT_COLOR_TEXT), 0);

    _as608_status_label = lv_label_create(result_row);
    lv_label_set_text(_as608_status_label, "状态: --");
    lv_obj_set_style_text_font(_as608_status_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(_as608_status_label, lv_color_hex(FINGERPRINT_COLOR_TEXT), 0);

    lv_obj_t *delete_row = lv_obj_create(_root);
    lv_obj_set_width(delete_row, LV_PCT(100));
    lv_obj_set_height(delete_row, 88);
    lv_obj_set_style_bg_opa(delete_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(delete_row, 0, 0);
    lv_obj_set_style_pad_all(delete_row, 0, 0);
    lv_obj_set_style_pad_column(delete_row, 12, 0);
    lv_obj_set_flex_flow(delete_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(delete_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *delete_label = lv_label_create(delete_row);
    lv_label_set_text(delete_label, "模板编号");
    lv_obj_set_width(delete_label, 180);
    lv_obj_set_style_text_font(delete_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_set_style_text_color(delete_label, lv_color_hex(FINGERPRINT_COLOR_MUTED), 0);

    _dec_btn = lv_btn_create(delete_row);
    lv_obj_set_size(_dec_btn, 64, 64);
    lv_obj_add_event_cb(_dec_btn, spinboxDecrementEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t *dec_label = lv_label_create(_dec_btn);
    lv_label_set_text(dec_label, "-");
    lv_obj_set_style_text_font(dec_label, &lv_font_montserrat_32, 0);
    lv_obj_center(dec_label);

    _spinbox = lv_spinbox_create(delete_row);
    lv_spinbox_set_range(_spinbox, 0, FINGERPRINT_TEMPLATE_MAX);
    lv_spinbox_set_digit_format(_spinbox, 3, 0);
    lv_spinbox_set_value(_spinbox, 0);
    lv_spinbox_set_step(_spinbox, 1);
    lv_obj_set_size(_spinbox, 160, 64);
    lv_obj_set_style_text_font(_spinbox, &lv_font_montserrat_32, 0);

    _inc_btn = lv_btn_create(delete_row);
    lv_obj_set_size(_inc_btn, 64, 64);
    lv_obj_add_event_cb(_inc_btn, spinboxIncrementEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t *inc_label = lv_label_create(_inc_btn);
    lv_label_set_text(inc_label, "+");
    lv_obj_set_style_text_font(inc_label, &lv_font_montserrat_32, 0);
    lv_obj_center(inc_label);

    lv_obj_t *button_row = lv_obj_create(_root);
    lv_obj_set_width(button_row, LV_PCT(100));
    lv_obj_set_height(button_row, 92);
    lv_obj_set_style_bg_opa(button_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button_row, 0, 0);
    lv_obj_set_style_pad_all(button_row, 0, 0);
    lv_obj_set_style_pad_column(button_row, 16, 0);
    lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    _identify_btn = lv_btn_create(button_row);
    lv_obj_set_size(_identify_btn, 210, 72);
    lv_obj_add_event_cb(_identify_btn, identifyEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t *identify_label = lv_label_create(_identify_btn);
    lv_label_set_text(identify_label, "识别指纹");
    lv_obj_set_style_text_font(identify_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_center(identify_label);

    _enroll_btn = lv_btn_create(button_row);
    lv_obj_set_size(_enroll_btn, 210, 72);
    lv_obj_add_event_cb(_enroll_btn, enrollEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t *enroll_label = lv_label_create(_enroll_btn);
    lv_label_set_text(enroll_label, "录入指纹");
    lv_obj_set_style_text_font(enroll_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_center(enroll_label);

    _delete_btn = lv_btn_create(button_row);
    lv_obj_set_size(_delete_btn, 210, 72);
    lv_obj_add_event_cb(_delete_btn, deleteEventCb, LV_EVENT_CLICKED, this);
    lv_obj_t *delete_btn_label = lv_label_create(_delete_btn);
    lv_label_set_text(delete_btn_label, "删除模板");
    lv_obj_set_style_text_font(delete_btn_label, FINGERPRINT_FONT_CN, 0);
    lv_obj_center(delete_btn_label);
}

void FingerprintApp::setButtonsEnabled(bool enabled)
{
    lv_obj_t *controls[] = {
        _identify_btn,
        _enroll_btn,
        _delete_btn,
        _inc_btn,
        _dec_btn,
        _spinbox,
    };

    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        if (controls[i] == NULL) {
            continue;
        }
        if (enabled) {
            lv_obj_clear_state(controls[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(controls[i], LV_STATE_DISABLED);
        }
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

void FingerprintApp::showResult(const OperationResult &result)
{
    char detail[128];

    if (result.err == ESP_OK) {
        switch (result.type) {
        case COMMAND_IDENTIFY:
            snprintf(detail, sizeof(detail), "匹配到模板 %u，得分 %u。", result.page_id, result.score);
            setStatusText("识别成功", detail, FINGERPRINT_COLOR_PRIMARY);
            break;
        case COMMAND_ENROLL:
            snprintf(detail, sizeof(detail), "已录入为模板 %u，得分 %u。", result.page_id, result.score);
            setStatusText("录入成功", detail, FINGERPRINT_COLOR_PRIMARY);
            break;
        case COMMAND_DELETE:
            snprintf(detail, sizeof(detail), "已删除模板 %u。", result.page_id);
            setStatusText("删除成功", detail, FINGERPRINT_COLOR_PRIMARY);
            break;
        default:
            break;
        }
    } else {
        snprintf(detail, sizeof(detail), "%s，模块状态：%s。",
                 esp_err_to_name(result.err), statusToText(result.status));
        if (result.status == AS608_STATUS_NO_FINGERPRINT) {
            setStatusText("未检测到手指", detail, FINGERPRINT_COLOR_WARN);
        } else if ((result.status == AS608_STATUS_NOT_FOUND) || (result.status == AS608_STATUS_NOT_MATCH)) {
            setStatusText("未匹配到指纹", detail, FINGERPRINT_COLOR_WARN);
        } else {
            setStatusText("操作失败", detail, FINGERPRINT_COLOR_ERROR);
        }
    }

    if (_page_label != NULL) {
        if ((result.err == ESP_OK) || (result.page_id > 0)) {
            lv_label_set_text_fmt(_page_label, "模板: %u", result.page_id);
        } else {
            lv_label_set_text(_page_label, "模板: --");
        }
    }
    if (_score_label != NULL) {
        if ((result.err == ESP_OK) || (result.score > 0)) {
            lv_label_set_text_fmt(_score_label, "分数: %u", result.score);
        } else {
            lv_label_set_text(_score_label, "分数: --");
        }
    }
    if (_as608_status_label != NULL) {
        lv_label_set_text_fmt(_as608_status_label, "状态: %s", statusToText(result.status));
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
    setButtonsEnabled(_service_ready && !_busy && !_closing);
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
            .page_id = command.page_id,
            .score = 0,
            .status = AS608_STATUS_UNKNOWN,
        };

        switch (command.type) {
        case COMMAND_IDENTIFY:
            result.err = as608_service_identify(&result.page_id, &result.score, &result.status);
            break;
        case COMMAND_ENROLL:
            result.err = as608_service_enroll(enrollStatusCb, app,
                                              &result.page_id, &result.score, &result.status);
            break;
        case COMMAND_DELETE:
            result.err = as608_service_delete_template(command.page_id, &result.status);
            break;
        default:
            break;
        }

        app->updateFromWorker(result);
    }

    xSemaphoreGive(app->_worker_done);
    vTaskDelete(NULL);
}

void FingerprintApp::identifyEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app == NULL) {
        return;
    }

    app->setStatusText("正在识别", "请把手指放到 AS608 指纹模块上。", FINGERPRINT_COLOR_WARN);
    app->postCommand(COMMAND_IDENTIFY, 0);
}

void FingerprintApp::enrollEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app == NULL) {
        return;
    }

    app->setStatusText("准备录入", "请按提示连续放置同一个手指两次。", FINGERPRINT_COLOR_WARN);
    app->postCommand(COMMAND_ENROLL, 0);
}

void FingerprintApp::deleteEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app == NULL || app->_spinbox == NULL) {
        return;
    }

    uint16_t page_id = (uint16_t)lv_spinbox_get_value(app->_spinbox);
    app->setStatusText("正在删除", "正在删除选中的指纹模板。", FINGERPRINT_COLOR_WARN);
    app->postCommand(COMMAND_DELETE, page_id);
}

void FingerprintApp::spinboxIncrementEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app != NULL && app->_spinbox != NULL) {
        lv_spinbox_increment(app->_spinbox);
    }
}

void FingerprintApp::spinboxDecrementEventCb(lv_event_t *e)
{
    FingerprintApp *app = static_cast<FingerprintApp *>(lv_event_get_user_data(e));
    if (app != NULL && app->_spinbox != NULL) {
        lv_spinbox_decrement(app->_spinbox);
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

const lv_img_dsc_t *FingerprintApp::getLauncherIcon(void)
{
    if (s_icon_ready) {
        return &s_icon_dsc;
    }

    const uint16_t ridge_color = 0x07F4;
    const uint16_t glow_color = 0x5FEA;

    memset(s_icon_data, 0, sizeof(s_icon_data));
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const int dx = x - 16;
            const int dy = y - 16;
            const int radius2 = dx * dx + dy * dy;
            bool ridge = false;

            if (radius2 < 210 && radius2 > 150 && y < 24) {
                ridge = true;
            } else if (radius2 < 130 && radius2 > 92 && y < 26) {
                ridge = true;
            } else if (radius2 < 68 && radius2 > 42 && y < 28) {
                ridge = true;
            } else if (x >= 15 && x <= 17 && y >= 14 && y <= 27) {
                ridge = true;
            } else if (x >= 18 && x <= 22 && y >= 20 && y <= 25) {
                ridge = true;
            }

            if (!ridge) {
                continue;
            }

            const size_t offset = (y * 32 + x) * LV_IMG_PX_SIZE_ALPHA_BYTE;
            uint16_t color = (radius2 < 70) ? glow_color : ridge_color;
#if LV_COLOR_DEPTH == 16
            s_icon_data[offset] = color & 0xFF;
            s_icon_data[offset + 1] = color >> 8;
            s_icon_data[offset + 2] = 0xFF;
#elif LV_COLOR_DEPTH == 32
            s_icon_data[offset] = 0x34;
            s_icon_data[offset + 1] = 0xD3;
            s_icon_data[offset + 2] = 0x99;
            s_icon_data[offset + 3] = 0xFF;
#else
            s_icon_data[offset] = 0xFF;
            s_icon_data[offset + 1] = 0xFF;
#endif
        }
    }

    s_icon_ready = true;
    return &s_icon_dsc;
}
