/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ClassroomScheduleApp.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "nvs.h"
#include "sdkconfig.h"

#define CLASSROOM_SCHEDULE_WORKER_STACK_SIZE       (12288)
#define CLASSROOM_SCHEDULE_WORKER_PRIORITY         (5)
#define CLASSROOM_SCHEDULE_WORKER_CORE             (0)
#define CLASSROOM_SCHEDULE_CLOSE_WAIT_MS           (CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_REQUEST_TIMEOUT_MS + 2000)
#define CLASSROOM_SCHEDULE_UI_LOCK_WAIT_MS         (200)
#define CLASSROOM_SCHEDULE_JSON_MAX_LEN            (8192)
#define CLASSROOM_SCHEDULE_URL_MAX_LEN             (512)
#define CLASSROOM_SCHEDULE_NVS_NAMESPACE           "class_sched"
#define CLASSROOM_SCHEDULE_NVS_KEY_CLASSROOM       "classroom"
#define CLASSROOM_SCHEDULE_NVS_KEY_CACHE_UPDATED   "cache_updated"
#define CLASSROOM_SCHEDULE_CACHE_PATH              "/spiffs/class_schedule.json"

#define CLASSROOM_SCHEDULE_COLOR_BG                0x111827
#define CLASSROOM_SCHEDULE_COLOR_PANEL             0x1F2937
#define CLASSROOM_SCHEDULE_COLOR_PANEL_SOFT        0x263445
#define CLASSROOM_SCHEDULE_COLOR_TEXT              0xF9FAFB
#define CLASSROOM_SCHEDULE_COLOR_MUTED             0xA7B0BD
#define CLASSROOM_SCHEDULE_COLOR_PRIMARY           0x34D399
#define CLASSROOM_SCHEDULE_COLOR_ACCENT            0x38BDF8
#define CLASSROOM_SCHEDULE_COLOR_WARN              0xFBBF24
#define CLASSROOM_SCHEDULE_COLOR_ERROR             0xF87171
#define CLASSROOM_SCHEDULE_FONT_CN                 (&classroom_schedule_font_20)
#define CLASSROOM_SCHEDULE_FONT_TITLE              (&lv_font_montserrat_30)
#define CLASSROOM_SCHEDULE_FONT_STATUS             (&lv_font_montserrat_34)
#define CLASSROOM_SCHEDULE_FONT_TIME               (&lv_font_montserrat_24)

LV_FONT_DECLARE(classroom_schedule_font_20);
LV_IMG_DECLARE(img_app_classroom_schedule);

static const char *TAG = "ClassroomSchedule";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} http_response_buffer_t;

static void copy_string(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0) {
        return;
    }

    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    snprintf(dest, dest_size, "%s", src);
}

static bool copy_json_string(cJSON *object, const char *key, char *dest, size_t dest_size, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        if (required) {
            return false;
        }
        copy_string(dest, dest_size, "");
        return true;
    }

    copy_string(dest, dest_size, item->valuestring);
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL) {
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    http_response_buffer_t *buffer = static_cast<http_response_buffer_t *>(event->user_data);
    const size_t data_len = (size_t)event->data_len;
    if (buffer->len + data_len >= buffer->cap) {
        buffer->overflow = true;
        return ESP_FAIL;
    }

    memcpy(buffer->data + buffer->len, event->data, data_len);
    buffer->len += data_len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

ClassroomScheduleApp::ClassroomScheduleApp():
    ESP_Brookesia_PhoneApp("教室课表", &img_app_classroom_schedule, true),
    _busy(false),
    _closing(false),
    _view_state(VIEW_NO_CLASSROOM),
    _classroom{0},
    _schedule{},
    _has_schedule(false),
    _worker_task(NULL),
    _worker_done(NULL),
    _refresh_timer(NULL),
    _root(NULL),
    _title_label(NULL),
    _classroom_label(NULL),
    _date_label(NULL),
    _updated_label(NULL),
    _status_label(NULL),
    _detail_label(NULL),
    _course_list(NULL),
    _classroom_ta(NULL),
    _keyboard(NULL),
    _refresh_btn(NULL),
    _save_btn(NULL)
{
}

ClassroomScheduleApp::~ClassroomScheduleApp()
{
}

bool ClassroomScheduleApp::init(void)
{
    return true;
}

bool ClassroomScheduleApp::run(void)
{
    _closing = false;
    _busy = false;
    _has_schedule = false;
    loadClassroom();
    buildUi();

    if (_worker_done == NULL) {
        _worker_done = xSemaphoreCreateBinary();
        if (_worker_done == NULL) {
            ESP_LOGE(TAG, "Failed to create worker semaphore");
            renderError("任务错误", "无法创建后台刷新任务同步对象。");
            return true;
        }
    }

    if (_classroom[0] == '\0') {
        setViewState(VIEW_NO_CLASSROOM, "未设置教室", "请输入学校系统中的教室标识，然后保存。", CLASSROOM_SCHEDULE_COLOR_WARN);
    } else {
        startRefresh();
    }

    _refresh_timer = lv_timer_create(refreshTimerCallback, CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_REFRESH_MS, this);
    if (_refresh_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create refresh timer");
    }

    return true;
}

bool ClassroomScheduleApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool ClassroomScheduleApp::close(void)
{
    _closing = true;
    stopRefreshTimer();
    setKeyboardVisible(false);

    if (_worker_task != NULL && _worker_done != NULL) {
        if (xSemaphoreTake(_worker_done, pdMS_TO_TICKS(CLASSROOM_SCHEDULE_CLOSE_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Timed out waiting for schedule worker to stop");
            return false;
        }
    }

    if (_worker_done != NULL) {
        vSemaphoreDelete(_worker_done);
        _worker_done = NULL;
    }

    _worker_task = NULL;
    _busy = false;
    _root = NULL;
    _title_label = NULL;
    _classroom_label = NULL;
    _date_label = NULL;
    _updated_label = NULL;
    _status_label = NULL;
    _detail_label = NULL;
    _course_list = NULL;
    _classroom_ta = NULL;
    _keyboard = NULL;
    _refresh_btn = NULL;
    _save_btn = NULL;
    _closing = false;

    return true;
}

bool ClassroomScheduleApp::loadClassroom(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        _classroom[0] = '\0';
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open schedule NVS: %s", esp_err_to_name(err));
        _classroom[0] = '\0';
        return false;
    }

    size_t len = sizeof(_classroom);
    err = nvs_get_str(handle, CLASSROOM_SCHEDULE_NVS_KEY_CLASSROOM, _classroom, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to load classroom: %s", esp_err_to_name(err));
        }
        _classroom[0] = '\0';
        return false;
    }

    trimClassroom(_classroom);
    return _classroom[0] != '\0';
}

bool ClassroomScheduleApp::saveClassroom(const char *classroom)
{
    if (classroom == NULL || classroom[0] == '\0') {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open schedule NVS for write: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, CLASSROOM_SCHEDULE_NVS_KEY_CLASSROOM, classroom);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save classroom: %s", esp_err_to_name(err));
        return false;
    }

    copy_string(_classroom, sizeof(_classroom), classroom);
    return true;
}

bool ClassroomScheduleApp::loadCacheUpdatedAt(char *updated_at, size_t updated_at_size)
{
    if (updated_at == NULL || updated_at_size == 0) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        updated_at[0] = '\0';
        return false;
    }

    size_t len = updated_at_size;
    err = nvs_get_str(handle, CLASSROOM_SCHEDULE_NVS_KEY_CACHE_UPDATED, updated_at, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        updated_at[0] = '\0';
        return false;
    }

    return updated_at[0] != '\0';
}

bool ClassroomScheduleApp::saveCacheUpdatedAt(const char *updated_at)
{
    if (updated_at == NULL || updated_at[0] == '\0') {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open schedule NVS for cache metadata: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, CLASSROOM_SCHEDULE_NVS_KEY_CACHE_UPDATED, updated_at);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save schedule cache metadata: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

esp_err_t ClassroomScheduleApp::loadCacheJson(char *buffer, size_t buffer_size, size_t *json_len)
{
    if (buffer == NULL || buffer_size == 0 || json_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(CLASSROOM_SCHEDULE_CACHE_PATH, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || (size_t)file_size >= buffer_size) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(file);

    size_t read_len = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);
    if (read_len != (size_t)file_size) {
        return ESP_FAIL;
    }

    buffer[read_len] = '\0';
    *json_len = read_len;
    return ESP_OK;
}

esp_err_t ClassroomScheduleApp::saveCacheJson(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(CLASSROOM_SCHEDULE_CACHE_PATH, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open schedule cache for write: errno=%d", errno);
        return ESP_FAIL;
    }

    const size_t json_len = strlen(json);
    size_t written = fwrite(json, 1, json_len, file);
    fclose(file);
    if (written != json_len) {
        ESP_LOGE(TAG, "Failed to write schedule cache");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ClassroomScheduleApp::fetchScheduleJson(char *buffer, size_t buffer_size, size_t *json_len, int *http_status)
{
    if (buffer == NULL || buffer_size == 0 || json_len == NULL || http_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char date[16];
    char url[CLASSROOM_SCHEDULE_URL_MAX_LEN];
    getToday(date, sizeof(date));
    buildUrl(url, sizeof(url), date);

    http_response_buffer_t response = {
        .data = buffer,
        .len = 0,
        .cap = buffer_size,
        .overflow = false,
    };
    buffer[0] = '\0';
    *json_len = 0;
    *http_status = 0;

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_REQUEST_TIMEOUT_MS;
    config.event_handler = http_event_handler;
    config.user_data = &response;
    config.buffer_size = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    *http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (response.overflow) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (*http_status != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (response.len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    *json_len = response.len;
    return ESP_OK;
}

esp_err_t ClassroomScheduleApp::parseScheduleJson(const char *json, size_t json_len, ScheduleData *data)
{
    if (json == NULL || json_len == 0 || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ScheduleData parsed = {};
    bool ok = copy_json_string(root, "date", parsed.date, sizeof(parsed.date), true) &&
              copy_json_string(root, "classroom", parsed.classroom, sizeof(parsed.classroom), true) &&
              copy_json_string(root, "classroom_name", parsed.classroom_name, sizeof(parsed.classroom_name), false) &&
              copy_json_string(root, "updated_at", parsed.updated_at, sizeof(parsed.updated_at), false);

    cJSON *courses = cJSON_GetObjectItemCaseSensitive(root, "courses");
    if (!ok || !cJSON_IsArray(courses)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int course_count = cJSON_GetArraySize(courses);
    if (course_count < 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    parsed.course_count = 0;
    const int max_count = (course_count > (int)(sizeof(parsed.courses) / sizeof(parsed.courses[0]))) ?
                          (int)(sizeof(parsed.courses) / sizeof(parsed.courses[0])) : course_count;
    for (int i = 0; i < max_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(courses, i);
        if (!cJSON_IsObject(item)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        Course course = {};
        if (!copy_json_string(item, "start", course.start, sizeof(course.start), true) ||
            !copy_json_string(item, "end", course.end, sizeof(course.end), true) ||
            !copy_json_string(item, "name", course.name, sizeof(course.name), true) ||
            !copy_json_string(item, "teacher", course.teacher, sizeof(course.teacher), false) ||
            !copy_json_string(item, "group", course.group, sizeof(course.group), false)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        parsed.courses[parsed.course_count++] = course;
    }

    if (parsed.updated_at[0] == '\0') {
        copy_string(parsed.updated_at, sizeof(parsed.updated_at), parsed.date);
    }
    if (parsed.classroom_name[0] == '\0') {
        copy_string(parsed.classroom_name, sizeof(parsed.classroom_name), parsed.classroom);
    }

    *data = parsed;
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ClassroomScheduleApp::loadCachedSchedule(ScheduleData *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json = static_cast<char *>(calloc(CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, 1));
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = 0;
    esp_err_t err = loadCacheJson(json, CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, &json_len);
    if (err == ESP_OK) {
        err = parseScheduleJson(json, json_len, data);
        if (err == ESP_OK) {
            data->from_cache = true;
            char cached_updated[sizeof(data->updated_at)];
            if (loadCacheUpdatedAt(cached_updated, sizeof(cached_updated))) {
                copy_string(data->updated_at, sizeof(data->updated_at), cached_updated);
            }
        }
    }

    free(json);
    return err;
}

void ClassroomScheduleApp::getToday(char *date, size_t date_size) const
{
    if (date == NULL || date_size == 0) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    snprintf(date, date_size, "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
}

void ClassroomScheduleApp::getNowText(char *text, size_t text_size) const
{
    if (text == NULL || text_size == 0) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    snprintf(text, text_size, "%04d-%02d-%02d %02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min);
}

void ClassroomScheduleApp::buildUrl(char *url, size_t url_size, const char *date) const
{
    char encoded_classroom[160];
    char encoded_token[160];
    char encoded_date[32];
    urlEncode(_classroom, encoded_classroom, sizeof(encoded_classroom));
    urlEncode(CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_TOKEN, encoded_token, sizeof(encoded_token));
    urlEncode(date, encoded_date, sizeof(encoded_date));

    const char *path = CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_API_PATH;
    if (path[0] == '/') {
        snprintf(url, url_size, "http://%s:%d%s?classroom=%s&date=%s&token=%s",
                 CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_HOST,
                 CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_PORT,
                 path, encoded_classroom, encoded_date, encoded_token);
    } else {
        snprintf(url, url_size, "http://%s:%d/%s?classroom=%s&date=%s&token=%s",
                 CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_HOST,
                 CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_PORT,
                 path, encoded_classroom, encoded_date, encoded_token);
    }
}

void ClassroomScheduleApp::urlEncode(const char *input, char *output, size_t output_size) const
{
    if (output == NULL || output_size == 0) {
        return;
    }
    output[0] = '\0';
    if (input == NULL) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; input[i] != '\0' && out + 1 < output_size; ++i) {
        const unsigned char ch = (unsigned char)input[i];
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            output[out++] = (char)ch;
        } else {
            if (out + 3 >= output_size) {
                break;
            }
            snprintf(output + out, output_size - out, "%%%02X", ch);
            out += 3;
        }
    }
    output[out] = '\0';
}

void ClassroomScheduleApp::trimClassroom(char *text) const
{
    if (text == NULL) {
        return;
    }

    char *start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        --end;
    }
    *end = '\0';

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
}

void ClassroomScheduleApp::buildUi(void)
{
    lv_area_t area = getVisualArea();
    const lv_coord_t width = area.x2 - area.x1;
    const lv_coord_t height = area.y2 - area.y1;

    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root, width, height);
    lv_obj_align(_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(CLASSROOM_SCHEDULE_COLOR_BG), 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 18, 0);
    lv_obj_set_style_pad_row(_root, 12, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(_root);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 64);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_pad_column(header, 12, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    _title_label = lv_label_create(header);
    lv_label_set_text(_title_label, "教室课表");
    lv_obj_set_width(_title_label, 150);
    applyLabelStyle(_title_label, CLASSROOM_SCHEDULE_COLOR_TEXT, CLASSROOM_SCHEDULE_FONT_CN);

    _classroom_label = lv_label_create(header);
    lv_obj_set_width(_classroom_label, 250);
    lv_label_set_long_mode(_classroom_label, LV_LABEL_LONG_DOT);
    applyLabelStyle(_classroom_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);

    _date_label = lv_label_create(header);
    lv_obj_set_width(_date_label, 180);
    applyLabelStyle(_date_label, CLASSROOM_SCHEDULE_COLOR_MUTED, &lv_font_montserrat_20);

    _refresh_btn = createButton(header, "刷新", refreshEventCb, 108, 46);

    lv_obj_t *input_row = lv_obj_create(_root);
    lv_obj_set_width(input_row, LV_PCT(100));
    lv_obj_set_height(input_row, 58);
    lv_obj_set_style_bg_color(input_row, lv_color_hex(CLASSROOM_SCHEDULE_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_set_style_radius(input_row, 8, 0);
    lv_obj_set_style_pad_all(input_row, 8, 0);
    lv_obj_set_style_pad_column(input_row, 10, 0);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *input_label = lv_label_create(input_row);
    lv_label_set_text(input_label, "教室标识");
    lv_obj_set_width(input_label, 110);
    applyLabelStyle(input_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);

    _classroom_ta = lv_textarea_create(input_row);
    lv_obj_set_width(_classroom_ta, 420);
    lv_obj_set_height(_classroom_ta, 42);
    lv_textarea_set_one_line(_classroom_ta, true);
    lv_textarea_set_max_length(_classroom_ta, sizeof(_classroom) - 1);
    lv_textarea_set_placeholder_text(_classroom_ta, "请输入教室标识");
    lv_textarea_set_text(_classroom_ta, _classroom);
    lv_obj_set_style_text_font(_classroom_ta, CLASSROOM_SCHEDULE_FONT_CN, 0);
    lv_obj_add_event_cb(_classroom_ta, classroomInputEventCb, LV_EVENT_CLICKED, this);

    _save_btn = createButton(input_row, "保存", saveClassroomEventCb, 108, 42);

    lv_obj_t *status_panel = createPanel(_root, 126);
    _status_label = lv_label_create(status_panel);
    lv_obj_set_width(_status_label, LV_PCT(100));
    lv_label_set_long_mode(_status_label, LV_LABEL_LONG_DOT);
    applyLabelStyle(_status_label, CLASSROOM_SCHEDULE_COLOR_PRIMARY, CLASSROOM_SCHEDULE_FONT_CN);

    _detail_label = lv_label_create(status_panel);
    lv_obj_set_width(_detail_label, LV_PCT(100));
    lv_label_set_long_mode(_detail_label, LV_LABEL_LONG_WRAP);
    applyLabelStyle(_detail_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);

    _updated_label = lv_label_create(status_panel);
    lv_obj_set_width(_updated_label, LV_PCT(100));
    lv_label_set_long_mode(_updated_label, LV_LABEL_LONG_DOT);
    applyLabelStyle(_updated_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);

    _course_list = lv_obj_create(_root);
    lv_obj_set_width(_course_list, LV_PCT(100));
    lv_obj_set_flex_grow(_course_list, 1);
    lv_obj_set_style_bg_color(_course_list, lv_color_hex(CLASSROOM_SCHEDULE_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(_course_list, 0, 0);
    lv_obj_set_style_radius(_course_list, 8, 0);
    lv_obj_set_style_pad_all(_course_list, 10, 0);
    lv_obj_set_style_pad_row(_course_list, 8, 0);
    lv_obj_set_scroll_dir(_course_list, LV_DIR_VER);
    lv_obj_set_flex_flow(_course_list, LV_FLEX_FLOW_COLUMN);

    _keyboard = lv_keyboard_create(_root);
    lv_obj_set_width(_keyboard, LV_PCT(100));
    lv_obj_set_height(_keyboard, 180);
    lv_keyboard_set_textarea(_keyboard, _classroom_ta);
    lv_obj_add_event_cb(_keyboard, keyboardEventCb, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);

    updateClassroomLabel();
}

void ClassroomScheduleApp::setViewState(ViewState state, const char *status, const char *detail, uint32_t color)
{
    _view_state = state;
    if (_status_label != NULL) {
        lv_label_set_text(_status_label, status != NULL ? status : "");
        lv_obj_set_style_text_color(_status_label, lv_color_hex(color), 0);
    }
    if (_detail_label != NULL) {
        lv_label_set_text(_detail_label, detail != NULL ? detail : "");
    }
    updateControls();
}

void ClassroomScheduleApp::renderSchedule(const ScheduleData &data)
{
    _schedule = data;
    _has_schedule = true;

    updateClassroomLabel();
    if (_updated_label != NULL) {
        char text[128];
        snprintf(text, sizeof(text), "%s：%s", data.from_cache ? "离线缓存" : "上次更新",
                 data.updated_at[0] != '\0' ? data.updated_at : data.date);
        lv_label_set_text(_updated_label, text);
    }

    if (data.course_count == 0) {
        renderEmpty(data);
        return;
    }

    int current_index = -1;
    int next_index = -1;
    findCurrentAndNext(data, &current_index, &next_index);
    if (current_index >= 0) {
        char detail[160];
        const Course &course = data.courses[current_index];
        snprintf(detail, sizeof(detail), "%s-%s  %s", course.start, course.end, course.name);
        setViewState(data.from_cache ? VIEW_CACHED : VIEW_READY, "当前课程", detail,
                     data.from_cache ? CLASSROOM_SCHEDULE_COLOR_WARN : CLASSROOM_SCHEDULE_COLOR_PRIMARY);
    } else if (next_index >= 0) {
        char detail[160];
        const Course &course = data.courses[next_index];
        snprintf(detail, sizeof(detail), "%s-%s  %s", course.start, course.end, course.name);
        setViewState(data.from_cache ? VIEW_CACHED : VIEW_READY, "下一节课", detail,
                     data.from_cache ? CLASSROOM_SCHEDULE_COLOR_WARN : CLASSROOM_SCHEDULE_COLOR_ACCENT);
    } else {
        setViewState(data.from_cache ? VIEW_CACHED : VIEW_READY, "今日课程已结束", "当前教室今天剩余时间暂无课程。",
                     data.from_cache ? CLASSROOM_SCHEDULE_COLOR_WARN : CLASSROOM_SCHEDULE_COLOR_MUTED);
    }

    renderCourseList(data, current_index, next_index);
}

void ClassroomScheduleApp::renderCourseList(const ScheduleData &data, int current_index, int next_index)
{
    clearCourseList();

    for (size_t i = 0; i < data.course_count; ++i) {
        const Course &course = data.courses[i];
        const bool is_current = (int)i == current_index;
        const bool is_next = (int)i == next_index;
        const uint32_t panel_color = is_current ? 0x14532D : (is_next ? 0x164E63 : CLASSROOM_SCHEDULE_COLOR_PANEL_SOFT);
        const uint32_t title_color = is_current ? CLASSROOM_SCHEDULE_COLOR_PRIMARY :
                                     (is_next ? CLASSROOM_SCHEDULE_COLOR_ACCENT : CLASSROOM_SCHEDULE_COLOR_TEXT);

        lv_obj_t *row = lv_obj_create(_course_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 78);
        lv_obj_set_style_bg_color(row, lv_color_hex(panel_color), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *time_label = lv_label_create(row);
        char time_text[24];
        snprintf(time_text, sizeof(time_text), "%s-%s", course.start, course.end);
        lv_label_set_text(time_label, time_text);
        lv_obj_set_width(time_label, 145);
        applyLabelStyle(time_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_TIME);

        lv_obj_t *info = lv_obj_create(row);
        lv_obj_set_width(info, 0);
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_height(info, LV_PCT(100));
        lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info, 0, 0);
        lv_obj_set_style_pad_all(info, 0, 0);
        lv_obj_set_style_pad_row(info, 4, 0);
        lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *name_label = lv_label_create(info);
        lv_label_set_text(name_label, course.name);
        lv_obj_set_width(name_label, LV_PCT(100));
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
        applyLabelStyle(name_label, title_color, CLASSROOM_SCHEDULE_FONT_CN);

        lv_obj_t *meta_label = lv_label_create(info);
        char meta[160];
        if (course.teacher[0] != '\0' && course.group[0] != '\0') {
            snprintf(meta, sizeof(meta), "教师：%s  班级：%s", course.teacher, course.group);
        } else if (course.teacher[0] != '\0') {
            snprintf(meta, sizeof(meta), "教师：%s", course.teacher);
        } else if (course.group[0] != '\0') {
            snprintf(meta, sizeof(meta), "班级：%s", course.group);
        } else {
            snprintf(meta, sizeof(meta), "课程信息");
        }
        lv_label_set_text(meta_label, meta);
        lv_obj_set_width(meta_label, LV_PCT(100));
        lv_label_set_long_mode(meta_label, LV_LABEL_LONG_DOT);
        applyLabelStyle(meta_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);
    }
}

void ClassroomScheduleApp::renderEmpty(const ScheduleData &data)
{
    setViewState(data.from_cache ? VIEW_CACHED : VIEW_EMPTY, "今日无课", "当前教室今天没有课程安排。",
                 data.from_cache ? CLASSROOM_SCHEDULE_COLOR_WARN : CLASSROOM_SCHEDULE_COLOR_PRIMARY);
    clearCourseList();
}

void ClassroomScheduleApp::renderError(const char *status, const char *detail)
{
    setViewState(VIEW_ERROR, status, detail, CLASSROOM_SCHEDULE_COLOR_ERROR);
    clearCourseList();
    if (_updated_label != NULL) {
        lv_label_set_text(_updated_label, "");
    }
}

void ClassroomScheduleApp::updateClassroomLabel(void)
{
    if (_classroom_label != NULL) {
        char text[128];
        const char *name = _schedule.classroom_name[0] != '\0' ? _schedule.classroom_name : _classroom;
        snprintf(text, sizeof(text), "教室：%s", name[0] != '\0' ? name : "未设置");
        lv_label_set_text(_classroom_label, text);
    }

    if (_date_label != NULL) {
        char date[16];
        getToday(date, sizeof(date));
        lv_label_set_text(_date_label, date);
    }
}

void ClassroomScheduleApp::updateControls(void)
{
    if (_refresh_btn != NULL) {
        if (_busy || _classroom[0] == '\0') {
            lv_obj_add_state(_refresh_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(_refresh_btn, LV_STATE_DISABLED);
        }
    }
    if (_save_btn != NULL) {
        if (_busy) {
            lv_obj_add_state(_save_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(_save_btn, LV_STATE_DISABLED);
        }
    }
}

void ClassroomScheduleApp::clearCourseList(void)
{
    if (_course_list != NULL) {
        lv_obj_clean(_course_list);
    }
}

void ClassroomScheduleApp::setKeyboardVisible(bool visible)
{
    if (_keyboard != NULL) {
        if (visible) {
            lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!visible && _classroom_ta != NULL) {
        lv_obj_clear_state(_classroom_ta, LV_STATE_FOCUSED);
    }
}

void ClassroomScheduleApp::startRefresh(void)
{
    if (_busy || _closing || _classroom[0] == '\0' || _worker_done == NULL) {
        updateControls();
        return;
    }

    _busy = true;
    if (_worker_done != NULL) {
        xSemaphoreTake(_worker_done, 0);
    }
    setViewState(VIEW_LOADING, "正在获取课表", "正在从服务器读取当前教室今天的课程安排。", CLASSROOM_SCHEDULE_COLOR_ACCENT);
    if (_updated_label != NULL) {
        lv_label_set_text(_updated_label, "");
    }

    BaseType_t ret = xTaskCreatePinnedToCore(refreshTask, "ClassSchedule", CLASSROOM_SCHEDULE_WORKER_STACK_SIZE,
                                            this, CLASSROOM_SCHEDULE_WORKER_PRIORITY, &_worker_task,
                                            CLASSROOM_SCHEDULE_WORKER_CORE);
    if (ret != pdPASS) {
        _busy = false;
        _worker_task = NULL;
        renderError("任务错误", "无法启动课表刷新任务。");
        ESP_LOGE(TAG, "Failed to create refresh task");
    }
}

void ClassroomScheduleApp::stopRefreshTimer(void)
{
    if (_refresh_timer != NULL) {
        lv_timer_del(_refresh_timer);
        _refresh_timer = NULL;
    }
}

void ClassroomScheduleApp::applyLabelStyle(lv_obj_t *label, uint32_t color, const lv_font_t *font)
{
    if (label == NULL) {
        return;
    }

    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

lv_obj_t *ClassroomScheduleApp::createButton(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_coord_t width,
                                             lv_coord_t height)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(CLASSROOM_SCHEDULE_COLOR_ACCENT), 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    applyLabelStyle(label, CLASSROOM_SCHEDULE_COLOR_BG, CLASSROOM_SCHEDULE_FONT_CN);
    lv_obj_center(label);

    return button;
}

lv_obj_t *ClassroomScheduleApp::createPanel(lv_obj_t *parent, lv_coord_t height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, height);
    lv_obj_set_style_bg_color(panel, lv_color_hex(CLASSROOM_SCHEDULE_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    return panel;
}

int ClassroomScheduleApp::parseMinuteOfDay(const char *time_text) const
{
    if (time_text == NULL) {
        return -1;
    }

    int hour = 0;
    int minute = 0;
    if (sscanf(time_text, "%d:%d", &hour, &minute) != 2) {
        return -1;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return -1;
    }
    return hour * 60 + minute;
}

void ClassroomScheduleApp::findCurrentAndNext(const ScheduleData &data, int *current_index, int *next_index) const
{
    if (current_index != NULL) {
        *current_index = -1;
    }
    if (next_index != NULL) {
        *next_index = -1;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    const int now_minute = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    for (size_t i = 0; i < data.course_count; ++i) {
        const int start = parseMinuteOfDay(data.courses[i].start);
        const int end = parseMinuteOfDay(data.courses[i].end);
        if (start < 0 || end < 0) {
            continue;
        }
        if (current_index != NULL && *current_index < 0 && now_minute >= start && now_minute < end) {
            *current_index = (int)i;
        }
        if (next_index != NULL && *next_index < 0 && now_minute < start) {
            *next_index = (int)i;
        }
    }
}

void ClassroomScheduleApp::updateFromWorker(const RefreshResult &result)
{
    _busy = false;

    if (_closing || _root == NULL) {
        return;
    }

    if (esp_lv_adapter_lock(pdMS_TO_TICKS(CLASSROOM_SCHEDULE_UI_LOCK_WAIT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Skip schedule UI update because LVGL lock timed out");
        return;
    }

    if (result.has_data) {
        renderSchedule(result.data);
    } else {
        renderError(result.err == ESP_ERR_INVALID_RESPONSE && (result.http_status == 401 || result.http_status == 403) ?
                    "认证失败" : "请求失败",
                    result.detail);
    }

    updateControls();
    lv_refr_now(NULL);
    esp_lv_adapter_unlock();
}

void ClassroomScheduleApp::refreshTask(void *arg)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(arg);
    if (app == NULL) {
        vTaskDelete(NULL);
        return;
    }

    RefreshResult result = {
        .err = ESP_FAIL,
        .http_status = 0,
        .used_cache = false,
        .has_data = false,
        .data = {},
        .detail = {},
    };

    char *json = static_cast<char *>(calloc(CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, 1));
    if (json == NULL) {
        result.err = ESP_ERR_NO_MEM;
        copy_string(result.detail, sizeof(result.detail), "内存不足，无法创建课表接收缓冲区。");
    } else {
        size_t json_len = 0;
        result.err = app->fetchScheduleJson(json, CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, &json_len, &result.http_status);
        if (result.err == ESP_OK) {
            result.err = app->parseScheduleJson(json, json_len, &result.data);
            if (result.err == ESP_OK) {
                result.data.from_cache = false;
                result.has_data = true;
                esp_err_t cache_err = app->saveCacheJson(json);
                if (cache_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to save schedule cache: %s", esp_err_to_name(cache_err));
                } else {
                    char cached_updated[sizeof(result.data.updated_at)];
                    app->getNowText(cached_updated, sizeof(cached_updated));
                    if (!app->saveCacheUpdatedAt(cached_updated)) {
                        ESP_LOGW(TAG, "Failed to save schedule cache metadata");
                    }
                }
            } else {
                copy_string(result.detail, sizeof(result.detail), "服务器返回的课表格式无法解析。");
            }
        }

        if (result.err != ESP_OK && !result.has_data) {
            ScheduleData cached = {};
            esp_err_t cache_err = app->loadCachedSchedule(&cached);
            if (cache_err == ESP_OK) {
                result.data = cached;
                result.used_cache = true;
                result.has_data = true;
            } else if (result.http_status == 401 || result.http_status == 403) {
                copy_string(result.detail, sizeof(result.detail), "服务器拒绝访问，请检查 token 配置。");
            } else if (result.err == ESP_ERR_INVALID_SIZE) {
                copy_string(result.detail, sizeof(result.detail), "课表响应过大，设备无法缓存或解析。");
            } else if (result.err == ESP_ERR_INVALID_RESPONSE) {
                copy_string(result.detail, sizeof(result.detail), "服务器响应异常，请检查接口格式。");
            } else {
                copy_string(result.detail, sizeof(result.detail), "无法获取课表，且本地没有可用缓存。");
            }
        }

        free(json);
    }

    app->updateFromWorker(result);
    if (app->_worker_done != NULL) {
        xSemaphoreGive(app->_worker_done);
    }
    app->_worker_task = NULL;
    vTaskDelete(NULL);
}

void ClassroomScheduleApp::refreshTimerCallback(lv_timer_t *timer)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(timer != NULL ? timer->user_data : NULL);
    if (app == NULL || app->_closing || app->_classroom[0] == '\0') {
        return;
    }

    app->startRefresh();
}

void ClassroomScheduleApp::refreshEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app != NULL) {
        app->startRefresh();
    }
}

void ClassroomScheduleApp::saveClassroomEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app == NULL || app->_classroom_ta == NULL || app->_busy) {
        return;
    }

    char classroom[sizeof(app->_classroom)];
    copy_string(classroom, sizeof(classroom), lv_textarea_get_text(app->_classroom_ta));
    app->trimClassroom(classroom);
    if (classroom[0] == '\0') {
        app->renderError("教室不能为空", "请输入学校系统中的教室标识。");
        return;
    }

    if (!app->saveClassroom(classroom)) {
        app->renderError("保存失败", "无法保存教室标识，请检查 NVS 状态。");
        return;
    }

    app->setKeyboardVisible(false);
    app->updateClassroomLabel();
    app->startRefresh();
}

void ClassroomScheduleApp::classroomInputEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app == NULL || app->_keyboard == NULL || app->_classroom_ta == NULL) {
        return;
    }

    lv_keyboard_set_textarea(app->_keyboard, app->_classroom_ta);
    bool keyboard_hidden = lv_obj_has_flag(app->_keyboard, LV_OBJ_FLAG_HIDDEN);
    app->setKeyboardVisible(keyboard_hidden);
}

void ClassroomScheduleApp::keyboardEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    lv_obj_t *target = lv_event_get_target(e);
    if (app == NULL || target == NULL || app->_classroom_ta == NULL) {
        return;
    }

    lv_keyboard_set_textarea(target, app->_classroom_ta);
    if (lv_keyboard_get_selected_btn(target) == 39) {
        app->setKeyboardVisible(false);
    }
}
