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
#include "esp_netif.h"
#include "nvs.h"
#include "sdkconfig.h"

#define CLASSROOM_SCHEDULE_WORKER_STACK_SIZE       (20480)
#define CLASSROOM_SCHEDULE_WORKER_PRIORITY         (5)
#define CLASSROOM_SCHEDULE_WORKER_CORE             (0)
#define CLASSROOM_SCHEDULE_CLOSE_WAIT_MS           (CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_REQUEST_TIMEOUT_MS + 2000)
#define CLASSROOM_SCHEDULE_UI_LOCK_WAIT_MS         (200)
#define CLASSROOM_SCHEDULE_UI_LOCK_RETRY_WAIT_MS   (1000)
#define CLASSROOM_SCHEDULE_JSON_MAX_LEN            (8192)
#define CLASSROOM_SCHEDULE_URL_MAX_LEN             (512)
#define CLASSROOM_SCHEDULE_NVS_NAMESPACE           "class_sched"
#define CLASSROOM_SCHEDULE_NVS_KEY_CLASSROOM       "classroom"
#define CLASSROOM_SCHEDULE_NVS_KEY_SERVER_HOST     "server_host"
#define CLASSROOM_SCHEDULE_NVS_KEY_CACHE_UPDATED   "cache_updated"
#define CLASSROOM_SCHEDULE_NVS_KEY_CACHE_SERVER    "cache_server"
#define CLASSROOM_SCHEDULE_NVS_KEY_CACHE_JSON      "cache_json"
#define CLASSROOM_SCHEDULE_CACHE_PATH              "/spiffs/class_schedule.json"
#define CLASSROOM_SCHEDULE_ROOM_MAX_LEN             (16)

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

struct ClassroomBuildingConfig {
    const char *display_name;
    const char *canonical_prefix;
    const char *legacy_prefix;
};

/*
 * 楼宇列表与 EAMS 教室占用页保持一致，顺序按页面展示顺序排列。
 * canonical_prefix 用于保存和查询，legacy_prefix 仅用于兼容已有 NVS 值。
 * EAMS building id 由 Windows 中间层维护；楼宇变化时需同步更新两端和字体子集。
 */
static const ClassroomBuildingConfig CLASSROOM_SCHEDULE_BUILDINGS[] = {
    {"博文楼", "博文楼", ""},
    {"知行楼", "知行楼", ""},
    {"主楼机房", "主楼机房", ""},
    {"静远楼", "静远楼", ""},
    {"博雅楼", "博雅楼", ""},
    {"耘慧楼", "耘慧楼", ""},
    {"物理实验室", "物理实验室", ""},
    {"葫芦岛物理实验室", "葫芦岛物理实验室", ""},
    {"中和楼", "中和楼", ""},
    {"致远楼", "致远楼", ""},
    {"新华楼", "新华楼", ""},
    {"尔雅楼", "尔雅楼", "尔雅"},
    {"葫芦岛机房", "葫芦岛机房", ""},
};

static bool hasNetworkIp(void)
{
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    esp_err_t err = esp_netif_get_ip_info(sta_netif, &ip_info);
    return err == ESP_OK && ip_info.ip.addr != 0;
}

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
    _server_host{0},
    _selected_date{0},
    _active_query{},
    _schedule{},
    _has_schedule(false),
    _classroom_config_valid(false),
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
    _building_dropdown(NULL),
    _room_ta(NULL),
    _server_host_ta(NULL),
    _keyboard(NULL),
    _refresh_btn(NULL),
    _save_btn(NULL),
    _date_btn(NULL),
    _today_btn(NULL),
    _calendar_overlay(NULL),
    _calendar(NULL)
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
    _schedule = {};
    _active_query = {};
    _classroom_config_valid = false;
    if (!getToday(_selected_date, sizeof(_selected_date))) {
        copy_string(_selected_date, sizeof(_selected_date), "1970-01-01");
    }
    loadClassroom();
    loadServerHost();
    buildUi();

    if (_worker_done == NULL) {
        _worker_done = xSemaphoreCreateBinary();
        if (_worker_done == NULL) {
            ESP_LOGE(TAG, "Failed to create worker semaphore");
            renderError("任务错误", "无法创建后台刷新任务同步对象。");
            return true;
        }
    }

    if (!_classroom_config_valid) {
        setViewState(VIEW_NO_CLASSROOM, "未设置教室", "请选择楼宇并填写房间号，然后保存。", CLASSROOM_SCHEDULE_COLOR_WARN);
    } else {
        updateClassroomLabel();
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
    closeCalendar();

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
    _building_dropdown = NULL;
    _room_ta = NULL;
    _server_host_ta = NULL;
    _keyboard = NULL;
    _refresh_btn = NULL;
    _save_btn = NULL;
    _date_btn = NULL;
    _today_btn = NULL;
    _calendar_overlay = NULL;
    _calendar = NULL;
    _classroom_config_valid = false;
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

bool ClassroomScheduleApp::loadServerHost(void)
{
    copy_string(_server_host, sizeof(_server_host), CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_HOST);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open schedule NVS for server host: %s", esp_err_to_name(err));
        return false;
    }

    size_t len = sizeof(_server_host);
    err = nvs_get_str(handle, CLASSROOM_SCHEDULE_NVS_KEY_SERVER_HOST, _server_host, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to load schedule server host: %s", esp_err_to_name(err));
        }
        copy_string(_server_host, sizeof(_server_host), CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_HOST);
        return false;
    }

    trimServerHost(_server_host);
    if (_server_host[0] == '\0') {
        copy_string(_server_host, sizeof(_server_host), CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_HOST);
        return false;
    }
    return true;
}

bool ClassroomScheduleApp::saveServerHost(const char *server_host)
{
    if (server_host == NULL || server_host[0] == '\0') {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open schedule NVS for server host write: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, CLASSROOM_SCHEDULE_NVS_KEY_SERVER_HOST, server_host);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save schedule server host: %s", esp_err_to_name(err));
        return false;
    }

    copy_string(_server_host, sizeof(_server_host), server_host);
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

bool ClassroomScheduleApp::loadCacheServerHost(char *server_host, size_t server_host_size)
{
    if (server_host == NULL || server_host_size == 0) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        server_host[0] = '\0';
        return false;
    }

    size_t len = server_host_size;
    err = nvs_get_str(handle, CLASSROOM_SCHEDULE_NVS_KEY_CACHE_SERVER, server_host, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        server_host[0] = '\0';
        return false;
    }

    return server_host[0] != '\0';
}

bool ClassroomScheduleApp::saveCacheServerHost(const char *server_host)
{
    if (server_host == NULL || server_host[0] == '\0') {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_set_str(handle, CLASSROOM_SCHEDULE_NVS_KEY_CACHE_SERVER, server_host);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

void ClassroomScheduleApp::invalidateCacheServerHost(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return;
    }

    err = nvs_erase_key(handle, CLASSROOM_SCHEDULE_NVS_KEY_CACHE_SERVER);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to invalidate schedule cache source: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

esp_err_t ClassroomScheduleApp::loadCacheJson(char *buffer, size_t buffer_size, size_t *json_len)
{
    if (buffer == NULL || buffer_size == 0 || json_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = loadCacheJsonFromNvs(buffer, buffer_size, json_len);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    return loadCacheJsonFromSpiffs(buffer, buffer_size, json_len);
}

esp_err_t ClassroomScheduleApp::loadCacheJsonFromSpiffs(char *buffer, size_t buffer_size, size_t *json_len)
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

    esp_err_t nvs_err = saveCacheJsonToNvs(json);
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save schedule cache to NVS: %s", esp_err_to_name(nvs_err));
    }

    FILE *file = fopen(CLASSROOM_SCHEDULE_CACHE_PATH, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open schedule cache for write: errno=%d", errno);
        return nvs_err == ESP_OK ? ESP_OK : ESP_FAIL;
    }

    const size_t json_len = strlen(json);
    size_t written = fwrite(json, 1, json_len, file);
    fclose(file);
    if (written != json_len) {
        ESP_LOGE(TAG, "Failed to write schedule cache");
        return nvs_err == ESP_OK ? ESP_OK : ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ClassroomScheduleApp::loadCacheJsonFromNvs(char *buffer, size_t buffer_size, size_t *json_len)
{
    if (buffer == NULL || buffer_size == 0 || json_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err = nvs_get_blob(handle, CLASSROOM_SCHEDULE_NVS_KEY_CACHE_JSON, NULL, &required_size);
    if (err == ESP_OK && (required_size == 0 || required_size >= buffer_size)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, CLASSROOM_SCHEDULE_NVS_KEY_CACHE_JSON, buffer, &required_size);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        return err;
    }

    buffer[required_size] = '\0';
    *json_len = required_size;
    return ESP_OK;
}

esp_err_t ClassroomScheduleApp::saveCacheJsonToNvs(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLASSROOM_SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, CLASSROOM_SCHEDULE_NVS_KEY_CACHE_JSON, json, strlen(json));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

esp_err_t ClassroomScheduleApp::fetchScheduleJson(const QuerySnapshot &query, char *buffer, size_t buffer_size,
                                                   size_t *json_len, int *http_status)
{
    if (buffer == NULL || buffer_size == 0 || json_len == NULL || http_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!hasNetworkIp()) {
        ESP_LOGW(TAG, "Skip schedule request because Wi-Fi has no IP address yet");
        return ESP_ERR_INVALID_STATE;
    }

    char url[CLASSROOM_SCHEDULE_URL_MAX_LEN];
    esp_err_t url_err = buildUrl(query, url, sizeof(url));
    if (url_err != ESP_OK) {
        return url_err;
    }

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

    ESP_LOGI(TAG, "Schedule HTTP result: host=%s port=%d classroom=%s date=%s err=%s status=%d bytes=%u overflow=%d",
             query.server_host, CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_PORT, query.classroom, query.date,
             esp_err_to_name(err), *http_status, (unsigned)response.len, response.overflow ? 1 : 0);

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

    ScheduleData *parsed = static_cast<ScheduleData *>(calloc(1, sizeof(ScheduleData)));
    if (parsed == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    bool ok = copy_json_string(root, "date", parsed->date, sizeof(parsed->date), true) &&
              copy_json_string(root, "classroom", parsed->classroom, sizeof(parsed->classroom), true) &&
              copy_json_string(root, "classroom_name", parsed->classroom_name, sizeof(parsed->classroom_name), false) &&
              copy_json_string(root, "updated_at", parsed->updated_at, sizeof(parsed->updated_at), false);

    cJSON *courses = cJSON_GetObjectItemCaseSensitive(root, "courses");
    if (!ok || !cJSON_IsArray(courses)) {
        free(parsed);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int course_count = cJSON_GetArraySize(courses);
    if (course_count < 0) {
        free(parsed);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    parsed->course_count = 0;
    const int max_count = (course_count > (int)(sizeof(parsed->courses) / sizeof(parsed->courses[0]))) ?
                          (int)(sizeof(parsed->courses) / sizeof(parsed->courses[0])) : course_count;
    for (int i = 0; i < max_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(courses, i);
        if (!cJSON_IsObject(item)) {
            free(parsed);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        Course course = {};
        if (!copy_json_string(item, "start", course.start, sizeof(course.start), true) ||
            !copy_json_string(item, "end", course.end, sizeof(course.end), true) ||
            !copy_json_string(item, "name", course.name, sizeof(course.name), true) ||
            !copy_json_string(item, "teacher", course.teacher, sizeof(course.teacher), false) ||
            !copy_json_string(item, "group", course.group, sizeof(course.group), false)) {
            free(parsed);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        parsed->courses[parsed->course_count++] = course;
    }

    if (parsed->updated_at[0] == '\0') {
        copy_string(parsed->updated_at, sizeof(parsed->updated_at), parsed->date);
    }
    if (parsed->classroom_name[0] == '\0') {
        copy_string(parsed->classroom_name, sizeof(parsed->classroom_name), parsed->classroom);
    }

    *data = *parsed;
    free(parsed);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ClassroomScheduleApp::loadCachedSchedule(const QuerySnapshot &query, ScheduleData *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json = static_cast<char *>(calloc(CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, 1));
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = 0;
    esp_err_t err = loadCacheJsonFromNvs(json, CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, &json_len);
    if (err == ESP_OK) {
        err = parseCachedScheduleJson(json, json_len, query, data);
        if (err == ESP_OK) {
            free(json);
            return ESP_OK;
        }
    }

    esp_err_t fallback_err = loadCacheJsonFromSpiffs(json, CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, &json_len);
    if (fallback_err == ESP_OK) {
        fallback_err = parseCachedScheduleJson(json, json_len, query, data);
        if (fallback_err == ESP_OK) {
            free(json);
            return ESP_OK;
        }
    }

    free(json);
    return err != ESP_OK ? err : fallback_err;
}

esp_err_t ClassroomScheduleApp::parseCachedScheduleJson(const char *json, size_t json_len, const QuerySnapshot &query,
                                                        ScheduleData *data)
{
    esp_err_t err = parseScheduleJson(json, json_len, data);
    if (err != ESP_OK) {
        return err;
    }

    if (strcmp(data->classroom, query.classroom) != 0 || strcmp(data->date, query.date) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char cached_server[sizeof(query.server_host)];
    if (!loadCacheServerHost(cached_server, sizeof(cached_server)) ||
        strcmp(cached_server, query.server_host) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    data->from_cache = true;
    char cached_updated[sizeof(data->updated_at)];
    if (loadCacheUpdatedAt(cached_updated, sizeof(cached_updated))) {
        copy_string(data->updated_at, sizeof(data->updated_at), cached_updated);
    }

    return ESP_OK;
}

bool ClassroomScheduleApp::getToday(char *date, size_t date_size) const
{
    if (date == NULL || date_size == 0) {
        return false;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    const int written = snprintf(date, date_size, "%04d-%02d-%02d",
                                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    return written > 0 && (size_t)written < date_size;
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

esp_err_t ClassroomScheduleApp::buildUrl(const QuerySnapshot &query, char *url, size_t url_size) const
{
    if (url == NULL || url_size == 0 || query.classroom[0] == '\0' || query.date[0] == '\0' ||
        query.server_host[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char encoded_classroom[160];
    char encoded_token[160];
    char encoded_date[32];
    if (!urlEncode(query.classroom, encoded_classroom, sizeof(encoded_classroom)) ||
        !urlEncode(CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_TOKEN, encoded_token, sizeof(encoded_token)) ||
        !urlEncode(query.date, encoded_date, sizeof(encoded_date))) {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *path = CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_API_PATH;
    int written = 0;
    if (path[0] == '/') {
        written = snprintf(url, url_size, "http://%s:%d%s?classroom=%s&date=%s&token=%s",
                           query.server_host, CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_PORT,
                           path, encoded_classroom, encoded_date, encoded_token);
    } else {
        written = snprintf(url, url_size, "http://%s:%d/%s?classroom=%s&date=%s&token=%s",
                           query.server_host, CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_PORT,
                           path, encoded_classroom, encoded_date, encoded_token);
    }
    return written > 0 && (size_t)written < url_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool ClassroomScheduleApp::urlEncode(const char *input, char *output, size_t output_size) const
{
    if (output == NULL || output_size == 0) {
        return false;
    }
    output[0] = '\0';
    if (input == NULL) {
        return false;
    }

    size_t out = 0;
    for (size_t i = 0; input[i] != '\0' && out + 1 < output_size; ++i) {
        const unsigned char ch = (unsigned char)input[i];
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            output[out++] = (char)ch;
        } else {
            if (out + 3 >= output_size) {
                output[0] = '\0';
                return false;
            }
            snprintf(output + out, output_size - out, "%%%02X", ch);
            out += 3;
        }
    }
    output[out] = '\0';
    return input[0] == '\0' || output[0] != '\0';
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

void ClassroomScheduleApp::trimServerHost(char *text) const
{
    trimClassroom(text);
    if (text == NULL || text[0] == '\0') {
        return;
    }

    const char http_prefix[] = "http://";
    const char https_prefix[] = "https://";
    if (strncmp(text, http_prefix, strlen(http_prefix)) == 0) {
        memmove(text, text + strlen(http_prefix), strlen(text + strlen(http_prefix)) + 1);
    } else if (strncmp(text, https_prefix, strlen(https_prefix)) == 0) {
        memmove(text, text + strlen(https_prefix), strlen(text + strlen(https_prefix)) + 1);
    }

    char *path = strchr(text, '/');
    if (path != NULL) {
        *path = '\0';
    }

    char *port = strchr(text, ':');
    if (port != NULL) {
        *port = '\0';
    }
    trimClassroom(text);
}

bool ClassroomScheduleApp::validateRoomNumber(const char *room) const
{
    if (room == NULL || room[0] == '\0') {
        return false;
    }

    const size_t len = strlen(room);
    if (len > CLASSROOM_SCHEDULE_ROOM_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)room[i])) {
            return false;
        }
    }
    return true;
}

bool ClassroomScheduleApp::splitClassroom(const char *classroom, size_t *building_index, char *room,
                                          size_t room_size) const
{
    if (classroom == NULL || building_index == NULL || room == NULL || room_size == 0) {
        return false;
    }

    for (size_t i = 0; i < sizeof(CLASSROOM_SCHEDULE_BUILDINGS) / sizeof(CLASSROOM_SCHEDULE_BUILDINGS[0]); ++i) {
        const char *prefix = CLASSROOM_SCHEDULE_BUILDINGS[i].canonical_prefix;
        const size_t prefix_len = strlen(prefix);
        if (strncmp(classroom, prefix, prefix_len) == 0 && validateRoomNumber(classroom + prefix_len)) {
            const int written = snprintf(room, room_size, "%s", classroom + prefix_len);
            if (written <= 0 || (size_t)written >= room_size) {
                return false;
            }
            *building_index = i;
            return true;
        }
    }

    for (size_t i = 0; i < sizeof(CLASSROOM_SCHEDULE_BUILDINGS) / sizeof(CLASSROOM_SCHEDULE_BUILDINGS[0]); ++i) {
        const char *prefix = CLASSROOM_SCHEDULE_BUILDINGS[i].legacy_prefix;
        const size_t prefix_len = strlen(prefix);
        if (prefix[0] != '\0' && strncmp(classroom, prefix, prefix_len) == 0 &&
            validateRoomNumber(classroom + prefix_len)) {
            const int written = snprintf(room, room_size, "%s", classroom + prefix_len);
            if (written <= 0 || (size_t)written >= room_size) {
                return false;
            }
            *building_index = i;
            return true;
        }
    }

    room[0] = '\0';
    return false;
}

bool ClassroomScheduleApp::composeClassroom(size_t building_index, const char *room, char *classroom,
                                            size_t classroom_size) const
{
    const size_t building_count = sizeof(CLASSROOM_SCHEDULE_BUILDINGS) / sizeof(CLASSROOM_SCHEDULE_BUILDINGS[0]);
    if (building_index >= building_count || !validateRoomNumber(room) || classroom == NULL || classroom_size == 0) {
        return false;
    }

    const int written = snprintf(classroom, classroom_size, "%s%s",
                                 CLASSROOM_SCHEDULE_BUILDINGS[building_index].canonical_prefix, room);
    return written > 0 && (size_t)written < classroom_size;
}

bool ClassroomScheduleApp::parseDate(const char *date, lv_calendar_date_t *parsed) const
{
    if (date == NULL || parsed == NULL || strlen(date) != 10 || date[4] != '-' || date[7] != '-') {
        return false;
    }
    for (size_t i = 0; i < 10; ++i) {
        if (i != 4 && i != 7 && !isdigit((unsigned char)date[i])) {
            return false;
        }
    }

    const int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
    const int month = (date[5] - '0') * 10 + (date[6] - '0');
    const int day = (date[8] - '0') * 10 + (date[9] - '0');
    if (year < 1970 || year > 2099 || month < 1 || month > 12) {
        return false;
    }

    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = days_in_month[month - 1];
    const bool leap_year = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap_year) {
        max_day = 29;
    }
    if (day < 1 || day > max_day) {
        return false;
    }

    parsed->year = (uint16_t)year;
    parsed->month = (uint8_t)month;
    parsed->day = (uint8_t)day;
    return true;
}

bool ClassroomScheduleApp::formatDate(const lv_calendar_date_t &date, char *text, size_t text_size) const
{
    if (text == NULL || text_size == 0) {
        return false;
    }
    const int written = snprintf(text, text_size, "%04u-%02u-%02u",
                                 (unsigned)date.year, (unsigned)date.month, (unsigned)date.day);
    if (written <= 0 || (size_t)written >= text_size) {
        return false;
    }

    lv_calendar_date_t checked = {};
    return parseDate(text, &checked);
}

bool ClassroomScheduleApp::createQuerySnapshot(QuerySnapshot *query) const
{
    lv_calendar_date_t parsed_date = {};
    if (query == NULL || !_classroom_config_valid || _classroom[0] == '\0' || _server_host[0] == '\0' ||
        !parseDate(_selected_date, &parsed_date)) {
        return false;
    }

    const int classroom_len = snprintf(query->classroom, sizeof(query->classroom), "%s", _classroom);
    const int date_len = snprintf(query->date, sizeof(query->date), "%s", _selected_date);
    const int server_len = snprintf(query->server_host, sizeof(query->server_host), "%s", _server_host);
    return classroom_len > 0 && (size_t)classroom_len < sizeof(query->classroom) &&
           date_len > 0 && (size_t)date_len < sizeof(query->date) &&
           server_len > 0 && (size_t)server_len < sizeof(query->server_host);
}

bool ClassroomScheduleApp::queryMatchesCurrent(const QuerySnapshot &query) const
{
    return strcmp(query.classroom, _classroom) == 0 && strcmp(query.date, _selected_date) == 0 &&
           strcmp(query.server_host, _server_host) == 0;
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

    size_t building_index = 0;
    char room[CLASSROOM_SCHEDULE_ROOM_MAX_LEN + 1] = {};
    _classroom_config_valid = splitClassroom(_classroom, &building_index, room, sizeof(room));
    if (_classroom_config_valid) {
        char canonical_classroom[sizeof(_classroom)];
        if (composeClassroom(building_index, room, canonical_classroom, sizeof(canonical_classroom))) {
            copy_string(_classroom, sizeof(_classroom), canonical_classroom);
        } else {
            _classroom_config_valid = false;
        }
    }

    lv_obj_t *building_label = lv_label_create(input_row);
    lv_label_set_text(building_label, "楼宇");
    lv_obj_set_width(building_label, 58);
    applyLabelStyle(building_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);

    _building_dropdown = lv_dropdown_create(input_row);
    lv_obj_set_width(_building_dropdown, 210);
    lv_obj_set_height(_building_dropdown, 42);
    char building_options[512] = {};
    size_t option_len = 0;
    for (size_t i = 0; i < sizeof(CLASSROOM_SCHEDULE_BUILDINGS) / sizeof(CLASSROOM_SCHEDULE_BUILDINGS[0]); ++i) {
        const int written = snprintf(building_options + option_len, sizeof(building_options) - option_len,
                                     "%s%s", i == 0 ? "" : "\n", CLASSROOM_SCHEDULE_BUILDINGS[i].display_name);
        if (written <= 0 || (size_t)written >= sizeof(building_options) - option_len) {
            building_options[0] = '\0';
            break;
        }
        option_len += (size_t)written;
    }
    lv_dropdown_set_options(_building_dropdown, building_options);
    lv_dropdown_set_selected(_building_dropdown, (uint16_t)building_index);
    lv_obj_add_event_cb(_building_dropdown, buildingDropdownEventCb, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_building_dropdown, buildingDropdownEventCb, LV_EVENT_VALUE_CHANGED, this);
    applyBuildingDropdownFont();

    lv_obj_t *room_label = lv_label_create(input_row);
    lv_label_set_text(room_label, "房间号");
    lv_obj_set_width(room_label, 76);
    applyLabelStyle(room_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);

    _room_ta = lv_textarea_create(input_row);
    lv_obj_set_width(_room_ta, 120);
    lv_obj_set_height(_room_ta, 42);
    lv_textarea_set_one_line(_room_ta, true);
    lv_textarea_set_max_length(_room_ta, CLASSROOM_SCHEDULE_ROOM_MAX_LEN);
    lv_textarea_set_accepted_chars(_room_ta, "0123456789");
    lv_textarea_set_placeholder_text(_room_ta, "103");
    lv_textarea_set_text(_room_ta, _classroom_config_valid ? room : "");
    lv_obj_set_style_text_font(_room_ta, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(_room_ta, roomInputEventCb, LV_EVENT_CLICKED, this);

    lv_obj_t *server_label = lv_label_create(input_row);
    lv_label_set_text(server_label, "服务器");
    lv_obj_set_width(server_label, 76);
    applyLabelStyle(server_label, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);

    _server_host_ta = lv_textarea_create(input_row);
    lv_obj_set_width(_server_host_ta, 190);
    lv_obj_set_height(_server_host_ta, 42);
    lv_textarea_set_one_line(_server_host_ta, true);
    lv_textarea_set_max_length(_server_host_ta, sizeof(_server_host) - 1);
    lv_textarea_set_placeholder_text(_server_host_ta, CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_HOST);
    lv_textarea_set_text(_server_host_ta, _server_host);
    lv_obj_set_style_text_font(_server_host_ta, CLASSROOM_SCHEDULE_FONT_CN, 0);
    lv_obj_add_event_cb(_server_host_ta, serverHostInputEventCb, LV_EVENT_CLICKED, this);

    _save_btn = createButton(input_row, "保存", saveClassroomEventCb, 108, 42);

    lv_obj_t *date_row = lv_obj_create(_root);
    lv_obj_set_width(date_row, LV_PCT(100));
    lv_obj_set_height(date_row, 50);
    lv_obj_set_style_bg_color(date_row, lv_color_hex(CLASSROOM_SCHEDULE_COLOR_PANEL_SOFT), 0);
    lv_obj_set_style_border_width(date_row, 0, 0);
    lv_obj_set_style_radius(date_row, 8, 0);
    lv_obj_set_style_pad_all(date_row, 6, 0);
    lv_obj_set_style_pad_column(date_row, 10, 0);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *date_title = lv_label_create(date_row);
    lv_label_set_text(date_title, "查询日期");
    lv_obj_set_width(date_title, 110);
    applyLabelStyle(date_title, CLASSROOM_SCHEDULE_COLOR_MUTED, CLASSROOM_SCHEDULE_FONT_CN);

    _date_btn = createButton(date_row, _selected_date, dateButtonEventCb, 210, 38);
    _today_btn = createButton(date_row, "今天", todayButtonEventCb, 108, 38);

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
    lv_keyboard_set_mode(_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(_keyboard, _room_ta);
    lv_obj_add_event_cb(_keyboard, keyboardEventCb, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_keyboard, keyboardEventCb, LV_EVENT_CANCEL, this);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);

    updateClassroomLabel();
    updateSelectedDateUi();
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
    char today[16] = {};
    const bool is_today = getToday(today, sizeof(today)) && strcmp(data.date, today) == 0;
    if (is_today) {
        findCurrentAndNext(data, &current_index, &next_index);
    }
    if (!is_today) {
        char detail[96];
        snprintf(detail, sizeof(detail), "%s 共 %u 节课程", data.date, (unsigned)data.course_count);
        setViewState(data.from_cache ? VIEW_CACHED : VIEW_READY, "课程安排", detail,
                     data.from_cache ? CLASSROOM_SCHEDULE_COLOR_WARN : CLASSROOM_SCHEDULE_COLOR_PRIMARY);
    } else if (current_index >= 0) {
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
        setViewState(data.from_cache ? VIEW_CACHED : VIEW_READY, "课程已结束", "当前教室所选日期已无后续课程。",
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
    setViewState(data.from_cache ? VIEW_CACHED : VIEW_EMPTY, "所选日期无课", "当前教室在所选日期没有课程安排。",
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
        lv_label_set_text(_date_label, _selected_date);
    }
}

void ClassroomScheduleApp::updateControls(void)
{
    if (_refresh_btn != NULL) {
        if (_busy || !_classroom_config_valid) {
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

    lv_obj_t *query_controls[] = {
        _building_dropdown,
        _room_ta,
        _server_host_ta,
        _date_btn,
        _today_btn,
    };
    for (size_t i = 0; i < sizeof(query_controls) / sizeof(query_controls[0]); ++i) {
        if (query_controls[i] == NULL) {
            continue;
        }
        if (_busy) {
            lv_obj_add_state(query_controls[i], LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(query_controls[i], LV_STATE_DISABLED);
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
    if (!visible && _room_ta != NULL) {
        lv_obj_clear_state(_room_ta, LV_STATE_FOCUSED);
    }
    if (!visible && _server_host_ta != NULL) {
        lv_obj_clear_state(_server_host_ta, LV_STATE_FOCUSED);
    }
}

void ClassroomScheduleApp::updateSelectedDateUi(void)
{
    if (_date_label != NULL) {
        lv_label_set_text(_date_label, _selected_date);
    }
    if (_date_btn != NULL) {
        lv_obj_t *label = lv_obj_get_child(_date_btn, 0);
        if (label != NULL) {
            lv_label_set_text(label, _selected_date);
        }
    }
}

void ClassroomScheduleApp::openCalendar(void)
{
    if (_busy || _root == NULL) {
        return;
    }
    if (_calendar_overlay != NULL) {
        lv_obj_move_foreground(_calendar_overlay);
        return;
    }

    setKeyboardVisible(false);

    lv_calendar_date_t selected = {};
    char today_text[16] = {};
    lv_calendar_date_t today = {};
    if (!parseDate(_selected_date, &selected) || !getToday(today_text, sizeof(today_text)) ||
        !parseDate(today_text, &today)) {
        renderError("日期无效", "设备日期无法用于查询，请检查系统时间。");
        return;
    }

    _calendar_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_calendar_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(_calendar_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_calendar_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_calendar_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_calendar_overlay, 0, 0);
    lv_obj_set_style_radius(_calendar_overlay, 0, 0);
    lv_obj_clear_flag(_calendar_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(_calendar_overlay);
    lv_obj_set_size(panel, 500, 430);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(CLASSROOM_SCHEDULE_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "选择日期");
    applyLabelStyle(title, CLASSROOM_SCHEDULE_COLOR_TEXT, CLASSROOM_SCHEDULE_FONT_CN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 4);

    lv_obj_t *close_btn = createButton(panel, "关闭", calendarCloseEventCb, 92, 38);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -4, -4);

    _calendar = lv_calendar_create(panel);
    lv_obj_set_size(_calendar, 450, 340);
    lv_obj_align(_calendar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(_calendar, &lv_font_montserrat_20, 0);
    lv_calendar_set_today_date(_calendar, today.year, today.month, today.day);
    lv_calendar_set_showed_date(_calendar, selected.year, selected.month);
    lv_obj_t *header = lv_calendar_header_arrow_create(_calendar);
    if (header != NULL) {
        lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    }
    lv_obj_add_event_cb(_calendar, calendarEventCb, LV_EVENT_VALUE_CHANGED, this);
}

void ClassroomScheduleApp::closeCalendar(void)
{
    if (_calendar_overlay == NULL) {
        _calendar = NULL;
        return;
    }

    lv_obj_t *overlay = _calendar_overlay;
    _calendar_overlay = NULL;
    _calendar = NULL;
    lv_obj_del(overlay);
}

void ClassroomScheduleApp::applySelectedDate(const char *date)
{
    lv_calendar_date_t parsed = {};
    if (!parseDate(date, &parsed)) {
        renderError("日期无效", "请选择实际存在的日历日期。");
        return;
    }

    closeCalendar();
    if (strcmp(date, _selected_date) == 0) {
        return;
    }

    const int written = snprintf(_selected_date, sizeof(_selected_date), "%s", date);
    if (written <= 0 || (size_t)written >= sizeof(_selected_date)) {
        renderError("日期无效", "日期长度超出设备支持范围。");
        return;
    }

    _schedule = {};
    _has_schedule = false;
    clearCourseList();
    updateClassroomLabel();
    updateSelectedDateUi();
    if (_classroom_config_valid) {
        setViewState(VIEW_IDLE, "日期已更新", "正在刷新所选日期的课表。", CLASSROOM_SCHEDULE_COLOR_PRIMARY);
        startRefresh();
    } else {
        setViewState(VIEW_NO_CLASSROOM, "未设置教室", "请选择楼宇并填写房间号，然后保存。",
                     CLASSROOM_SCHEDULE_COLOR_WARN);
    }
}

void ClassroomScheduleApp::startRefresh(void)
{
    QuerySnapshot query = {};
    if (_busy || _closing || _worker_done == NULL || !createQuerySnapshot(&query)) {
        updateControls();
        return;
    }

    _active_query = query;
    _busy = true;
    ESP_LOGI(TAG, "Schedule query start: host=%s port=%d path=%s classroom=%s date=%s",
             query.server_host, CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_SERVER_PORT,
             CONFIG_EXAMPLE_CLASSROOM_SCHEDULE_API_PATH, query.classroom, query.date);
    if (_worker_done != NULL) {
        xSemaphoreTake(_worker_done, 0);
    }
    _schedule = {};
    _has_schedule = false;
    clearCourseList();
    setViewState(VIEW_LOADING, "正在获取课表", "正在从服务器读取所选日期的课程安排。", CLASSROOM_SCHEDULE_COLOR_ACCENT);
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

void ClassroomScheduleApp::applyBuildingDropdownFont(void)
{
    if (_building_dropdown == NULL) {
        return;
    }

    lv_obj_set_style_text_font(_building_dropdown, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(_building_dropdown, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(_building_dropdown, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(_building_dropdown, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(_building_dropdown, &lv_font_montserrat_20,
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(_building_dropdown, &lv_font_montserrat_20,
                               LV_PART_INDICATOR | LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(_building_dropdown, &lv_font_montserrat_20,
                               LV_PART_INDICATOR | LV_STATE_PRESSED);
    lv_obj_set_style_text_font(_building_dropdown, &lv_font_montserrat_20,
                               LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(_building_dropdown, &lv_font_montserrat_20,
                               LV_PART_INDICATOR | LV_STATE_FOCUSED | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(_building_dropdown, &lv_font_montserrat_20,
                               LV_PART_INDICATOR | LV_STATE_PRESSED | LV_STATE_CHECKED);
    lv_obj_t *building_list = lv_dropdown_get_list(_building_dropdown);
    if (building_list == NULL) {
        return;
    }

    lv_obj_set_style_text_font(building_list, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(building_list, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_font(building_list, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(building_list, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(building_list, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_set_style_text_font(building_list, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(building_list, CLASSROOM_SCHEDULE_FONT_CN,
                               LV_PART_SELECTED | LV_STATE_CHECKED | LV_STATE_PRESSED);

    const uint32_t child_count = lv_obj_get_child_cnt(building_list);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(building_list, i);
        if (child != NULL) {
            lv_obj_set_style_text_font(child, CLASSROOM_SCHEDULE_FONT_CN,
                                       LV_PART_MAIN | LV_STATE_DEFAULT);
        }
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
    if (_closing || _root == NULL || !queryMatchesCurrent(result.query)) {
        return;
    }
    _busy = false;

    esp_err_t lock_err = esp_lv_adapter_lock(pdMS_TO_TICKS(CLASSROOM_SCHEDULE_UI_LOCK_WAIT_MS));
    if (lock_err != ESP_OK && !_closing) {
        lock_err = esp_lv_adapter_lock(pdMS_TO_TICKS(CLASSROOM_SCHEDULE_UI_LOCK_RETRY_WAIT_MS));
    }
    if (lock_err != ESP_OK) {
        ESP_LOGW(TAG, "Skip schedule UI update because LVGL lock timed out");
        return;
    }

    if (_closing || _root == NULL || !queryMatchesCurrent(result.query)) {
        esp_lv_adapter_unlock();
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

    const QuerySnapshot query = app->_active_query;
    RefreshResult *result = static_cast<RefreshResult *>(calloc(1, sizeof(RefreshResult)));
    if (result == NULL) {
        ESP_LOGE(TAG, "Failed to allocate schedule refresh result");
        app->_busy = false;
        if (app->_worker_done != NULL) {
            xSemaphoreGive(app->_worker_done);
        }
        app->_worker_task = NULL;
        ESP_LOGI(TAG, "Schedule task stack minimum: %u bytes",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(NULL)));
        vTaskDelete(NULL);
        return;
    }
    result->err = ESP_FAIL;
    result->query = query;

    char *json = static_cast<char *>(calloc(CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, 1));
    if (json == NULL) {
        result->err = ESP_ERR_NO_MEM;
        copy_string(result->detail, sizeof(result->detail), "内存不足，无法创建课表接收缓冲区。");
    } else {
        size_t json_len = 0;
        result->err = app->fetchScheduleJson(query, json, CLASSROOM_SCHEDULE_JSON_MAX_LEN + 1, &json_len,
                                             &result->http_status);
        if (result->err == ESP_OK) {
            result->err = app->parseScheduleJson(json, json_len, &result->data);
            if (result->err == ESP_OK && (strcmp(result->data.classroom, query.classroom) != 0 ||
                                          strcmp(result->data.date, query.date) != 0)) {
                ESP_LOGE(TAG, "Schedule response mismatch: requested_classroom=%s response_classroom=%s "
                         "requested_date=%s response_date=%s",
                         query.classroom, result->data.classroom, query.date, result->data.date);
                copy_string(result->detail, sizeof(result->detail),
                            "服务器返回的教室或日期与当前查询不同。");
                result->err = ESP_ERR_INVALID_RESPONSE;
            }
            if (result->err == ESP_OK) {
                ESP_LOGI(TAG, "Schedule response parsed: classroom=%s requested_date=%s response_date=%s courses=%u",
                         result->data.classroom, query.date, result->data.date,
                         (unsigned)result->data.course_count);
                result->data.from_cache = false;
                result->has_data = true;
                esp_err_t cache_err = app->saveCacheJson(json);
                if (cache_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to save schedule cache: %s", esp_err_to_name(cache_err));
                } else {
                    char cached_updated[sizeof(result->data.updated_at)];
                    app->getNowText(cached_updated, sizeof(cached_updated));
                    if (!app->saveCacheUpdatedAt(cached_updated) || !app->saveCacheServerHost(query.server_host)) {
                        ESP_LOGW(TAG, "Failed to save schedule cache metadata");
                    }
                }
            } else if (result->detail[0] == '\0') {
                copy_string(result->detail, sizeof(result->detail), "服务器返回的课表格式无法解析。");
            }
        }

        if (result->err != ESP_OK && !result->has_data) {
            esp_err_t cache_err = app->loadCachedSchedule(query, &result->data);
            if (cache_err == ESP_OK) {
                result->used_cache = true;
                result->has_data = true;
                ESP_LOGW(TAG, "Using schedule cache after request failed: request=%s, http=%d",
                         esp_err_to_name(result->err), result->http_status);
            } else if (result->http_status == 401 || result->http_status == 403) {
                copy_string(result->detail, sizeof(result->detail), "服务器拒绝访问，请检查 token 配置。");
            } else if (result->err == ESP_ERR_INVALID_SIZE) {
                copy_string(result->detail, sizeof(result->detail), "课表响应过大，设备无法缓存或解析。");
            } else if (result->err == ESP_ERR_HTTP_CONNECT) {
                copy_string(result->detail, sizeof(result->detail), "服务器不可用，请检查服务器 IP 和端口后重试。");
            } else if (result->err == ESP_ERR_HTTP_READ_TIMEOUT) {
                copy_string(result->detail, sizeof(result->detail), "服务器响应超时，请检查网络后重试。");
            } else if (result->http_status == 502 || result->http_status == 503) {
                copy_string(result->detail, sizeof(result->detail),
                            "课表上游会话不可用，请在电脑端重新登录 EAMS。");
            } else if (result->http_status >= 500) {
                copy_string(result->detail, sizeof(result->detail), "课表服务器内部错误，请检查电脑端服务。");
            } else if (result->err == ESP_ERR_INVALID_RESPONSE) {
                if (result->detail[0] == '\0') {
                    copy_string(result->detail, sizeof(result->detail), "服务器响应异常，请检查接口格式。");
                }
            } else {
                ESP_LOGW(TAG, "No usable schedule cache: request=%s, cache=%s, http=%d",
                         esp_err_to_name(result->err), esp_err_to_name(cache_err), result->http_status);
                copy_string(result->detail, sizeof(result->detail), "无法获取课表，暂无缓存。请检查网络或服务器后重试。");
            }
        }

        free(json);
    }

    app->updateFromWorker(*result);
    free(result);
    ESP_LOGI(TAG, "Schedule task stack minimum: %u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(NULL)));
    if (app->_worker_done != NULL) {
        xSemaphoreGive(app->_worker_done);
    }
    app->_worker_task = NULL;
    vTaskDelete(NULL);
}

void ClassroomScheduleApp::refreshTimerCallback(lv_timer_t *timer)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(timer != NULL ? timer->user_data : NULL);
    if (app == NULL || app->_closing || !app->_classroom_config_valid) {
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

void ClassroomScheduleApp::buildingDropdownEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app != NULL) {
        app->applyBuildingDropdownFont();
    }
}

void ClassroomScheduleApp::saveClassroomEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app == NULL || app->_building_dropdown == NULL || app->_room_ta == NULL ||
        app->_server_host_ta == NULL || app->_busy) {
        return;
    }

    char classroom[sizeof(app->_classroom)];
    char room[CLASSROOM_SCHEDULE_ROOM_MAX_LEN + 1];
    copy_string(room, sizeof(room), lv_textarea_get_text(app->_room_ta));
    app->trimClassroom(room);
    const size_t building_index = lv_dropdown_get_selected(app->_building_dropdown);
    char server_host[sizeof(app->_server_host)];
    copy_string(server_host, sizeof(server_host), lv_textarea_get_text(app->_server_host_ta));
    app->trimServerHost(server_host);
    if (!app->validateRoomNumber(room)) {
        app->renderError("房间号无效", "房间号只能包含数字且不能为空。");
        return;
    }
    if (!app->composeClassroom(building_index, room, classroom, sizeof(classroom))) {
        app->renderError("教室无效", "楼宇与房间号组合超出设备支持范围。");
        return;
    }
    if (server_host[0] == '\0') {
        app->renderError("服务器不能为空", "请输入服务器 IP。");
        return;
    }

    const bool server_changed = strcmp(server_host, app->_server_host) != 0;
    if (!app->saveClassroom(classroom)) {
        app->renderError("保存失败", "无法保存教室标识，请检查 NVS 状态。");
        return;
    }
    if (!app->saveServerHost(server_host)) {
        app->renderError("保存失败", "无法保存服务器 IP，请检查 NVS 状态。");
        return;
    }
    if (server_changed) {
        app->invalidateCacheServerHost();
    }
    app->_classroom_config_valid = true;
    lv_textarea_set_text(app->_room_ta, room);
    lv_textarea_set_text(app->_server_host_ta, app->_server_host);

    app->setKeyboardVisible(false);
    app->_schedule = {};
    app->_has_schedule = false;
    app->clearCourseList();
    app->updateClassroomLabel();
    app->setViewState(VIEW_IDLE, "配置已保存", "正在刷新所选日期的课表。", CLASSROOM_SCHEDULE_COLOR_PRIMARY);
    app->startRefresh();
}

void ClassroomScheduleApp::roomInputEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app == NULL || app->_keyboard == NULL || app->_room_ta == NULL || app->_busy) {
        return;
    }

    lv_keyboard_set_mode(app->_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(app->_keyboard, app->_room_ta);
    app->setKeyboardVisible(true);
}

void ClassroomScheduleApp::serverHostInputEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app == NULL || app->_keyboard == NULL || app->_server_host_ta == NULL || app->_busy) {
        return;
    }

    lv_keyboard_set_mode(app->_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(app->_keyboard, app->_server_host_ta);
    app->setKeyboardVisible(true);
}

void ClassroomScheduleApp::keyboardEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app == NULL) {
        return;
    }
    app->setKeyboardVisible(false);
}

void ClassroomScheduleApp::dateButtonEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app != NULL) {
        app->openCalendar();
    }
}

void ClassroomScheduleApp::todayButtonEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app == NULL || app->_busy) {
        return;
    }

    char today[sizeof(app->_selected_date)] = {};
    if (!app->getToday(today, sizeof(today))) {
        app->renderError("日期无效", "设备日期无法用于查询，请检查系统时间。");
        return;
    }
    app->applySelectedDate(today);
}

void ClassroomScheduleApp::calendarEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    lv_obj_t *calendar = lv_event_get_current_target(e);
    if (app == NULL || calendar == NULL || app->_busy) {
        return;
    }

    lv_calendar_date_t selected = {};
    if (lv_calendar_get_pressed_date(calendar, &selected) != LV_RES_OK) {
        return;
    }
    char date[sizeof(app->_selected_date)] = {};
    if (!app->formatDate(selected, date, sizeof(date))) {
        app->renderError("日期无效", "请选择实际存在的日历日期。");
        return;
    }
    app->applySelectedDate(date);
}

void ClassroomScheduleApp::calendarCloseEventCb(lv_event_t *e)
{
    ClassroomScheduleApp *app = static_cast<ClassroomScheduleApp *>(lv_event_get_user_data(e));
    if (app != NULL) {
        app->closeCalendar();
    }
}
