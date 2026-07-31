/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "onenet_cloud_service.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "dht11_service.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "mbedtls/md5.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ONENET_CLOUD_TASK_NAME             "onenet_cloud"
#define ONENET_CLOUD_LOOP_PERIOD_MS        (1000U)
#define ONENET_CLOUD_STOP_WAIT_MS          (3000U)
#define ONENET_CLOUD_MQTT_URI_MAX_LEN      (160U)
#define ONENET_CLOUD_MQTT_RETRY_MIN_MS     (15000U)
#define ONENET_CLOUD_MQTT_RETRY_MAX_MS     (120000U)
#define ONENET_CLOUD_MQTT_NETWORK_TIMEOUT_MS (5000)
#define ONENET_CLOUD_MQTT_BEFORE_HTTP_SETTLE_MS (1500U)
#define ONENET_CLOUD_MQTT_AFTER_HTTP_SETTLE_MS (15000U)
#define ONENET_CLOUD_NETWORK_PAUSE_MAX_MS  (120000U)
#define ONENET_CLOUD_TIME_VALID_YEAR       (2024)
#define ONENET_CLOUD_TIME_WAIT_LOG_MS      (30000)
#define ONENET_CLOUD_NO_IP_LOG_MS          (30000)
#define ONENET_CLOUD_NETWORK_PAUSE_LOG_MS  (30000)
#define ONENET_CLOUD_MQTT_HOST_SUFFIX      "heclouds.com"
#define ONENET_CLOUD_MQTT_CERT_COMMON_NAME "OneNET MQTTS"
#define ONENET_CLOUD_TOPIC_MAX_LEN         (192U)
#define ONENET_CLOUD_URL_MAX_LEN           (256U)
#define ONENET_CLOUD_MD5_HEX_LEN           (33U)
#define ONENET_CLOUD_FILE_CHUNK_SIZE       (1024U)
#define ONENET_CLOUD_HTTP_RESPONSE_LEN     (512U)
#define ONENET_CLOUD_HTTP_TIMEOUT_MS       (15000)
#define ONENET_CLOUD_MULTIPART_BOUNDARY    "----esp32p4-onenet-boundary"
#define ONENET_CLOUD_ONEJSON_VERSION       "1.0"
#define ONENET_CLOUD_MESSAGE_ID_BUF_LEN    (11U)
#define ONENET_CLOUD_PROPERTY_REPLY_SUCCESS_CODE (200)
#define ONENET_CLOUD_REPLY_MESSAGE_LOG_MAX_LEN (96)

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
#define ONENET_CLOUD_PHOTO_DIR CONFIG_BSP_SD_MOUNT_POINT "/camera"
#else
#define ONENET_CLOUD_PHOTO_DIR ""
#endif

#define ONENET_CLOUD_PHOTO_PATH_MAX_LEN    (sizeof(ONENET_CLOUD_PHOTO_DIR) + ONENET_CLOUD_PHOTO_NAME_MAX_LEN)

static const char *TAG = "OneNETCloud";

extern const uint8_t onenet_mqtt_root_pem_start[] asm("_binary_onenet_mqtt_root_pem_start");

typedef struct
{
    bool enabled;
    bool photo_upload_enabled;
    uint16_t mqtt_port;
    uint32_t upload_interval_ms;
    uint32_t photo_upload_cooldown_ms;
    size_t photo_max_bytes;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    char product_id[ONENET_CLOUD_PRODUCT_ID_MAX_LEN];
    char device_name[ONENET_CLOUD_DEVICE_NAME_MAX_LEN];
    char auth_token[ONENET_CLOUD_AUTH_TOKEN_MAX_LEN];
    char mqtt_host[ONENET_CLOUD_HOST_MAX_LEN];
    char api_host[ONENET_CLOUD_HOST_MAX_LEN];
} onenet_cloud_runtime_config_t;

typedef struct
{
    char name[ONENET_CLOUD_PHOTO_NAME_MAX_LEN];
    char path[ONENET_CLOUD_PHOTO_PATH_MAX_LEN];
    size_t size;
    time_t mtime;
} onenet_cloud_photo_info_t;

typedef struct
{
    char hostname[ONENET_CLOUD_HOST_MAX_LEN];
    uint16_t port;
    esp_mqtt_transport_t transport;
} onenet_cloud_mqtt_endpoint_t;

static onenet_cloud_runtime_config_t s_config;
static onenet_cloud_snapshot_t s_snapshot;
static SemaphoreHandle_t s_snapshot_mutex;
static SemaphoreHandle_t s_done_sem;
static TaskHandle_t s_task_handle;
static esp_mqtt_client_handle_t s_mqtt_client;
static volatile bool s_stop_requested;
static volatile bool s_photo_upload_requested;
static volatile bool s_mqtt_restart_requested;
static bool s_initialized;
static bool s_mqtt_started;
static uint32_t s_mqtt_retry_delay_ms;
static int64_t s_next_mqtt_start_ms;
static int64_t s_last_time_wait_log_ms;
static int64_t s_last_no_ip_log_ms;
static int64_t s_last_network_pause_log_ms;
static volatile uint32_t s_network_pause_until_ms;
static uint32_t s_property_message_id;

static void destroy_mqtt_client(void);

static void copy_string(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0)
    {
        return;
    }

    if (src == NULL)
    {
        dest[0] = '\0';
        return;
    }

    snprintf(dest, dest_size, "%s", src);
}

static bool string_has_value(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static void snapshot_lock(void)
{
    if (s_snapshot_mutex != NULL)
    {
        (void)xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    }
}

static void snapshot_unlock(void)
{
    if (s_snapshot_mutex != NULL)
    {
        xSemaphoreGive(s_snapshot_mutex);
    }
}

static bool config_is_complete(void)
{
    return string_has_value(s_config.product_id) && string_has_value(s_config.device_name) &&
           string_has_value(s_config.auth_token) && string_has_value(s_config.mqtt_host) &&
           string_has_value(s_config.api_host);
}

static bool endpoint_uses_onenet_mqtt_ca(const onenet_cloud_mqtt_endpoint_t *endpoint)
{
    return endpoint != NULL &&
           strstr(endpoint->hostname, ONENET_CLOUD_MQTT_HOST_SUFFIX) != NULL;
}

static void update_status(onenet_cloud_status_t status, esp_err_t err, const char *reason);

static void reset_runtime_state(void)
{
    s_done_sem = NULL;
    s_task_handle = NULL;
    s_mqtt_client = NULL;
    s_stop_requested = false;
    s_photo_upload_requested = false;
    s_mqtt_restart_requested = false;
    s_initialized = false;
    s_mqtt_started = false;
    s_mqtt_retry_delay_ms = ONENET_CLOUD_MQTT_RETRY_MIN_MS;
    s_next_mqtt_start_ms = 0;
    s_last_time_wait_log_ms = 0;
    s_last_no_ip_log_ms = 0;
    s_last_network_pause_log_ms = 0;
    s_network_pause_until_ms = 0;
    s_property_message_id = 0;
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
}

static uint32_t now_millis32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool network_pause_active(uint32_t now_ms)
{
    return (int32_t)(s_network_pause_until_ms - now_ms) > 0;
}

static uint32_t network_pause_remaining_ms(uint32_t now_ms)
{
    if (!network_pause_active(now_ms))
    {
        return 0;
    }

    return (uint32_t)(s_network_pause_until_ms - now_ms);
}

static bool system_time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo = {};

    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (ONENET_CLOUD_TIME_VALID_YEAR - 1900);
}

static bool ensure_time_ready_for_tls(int64_t now_ms)
{
    if (system_time_is_valid())
    {
        return true;
    }

    setenv("TZ", "CST-8", 1);
    tzset();
    update_status(ONENET_CLOUD_STATUS_TIME_SYNC, ESP_ERR_INVALID_STATE, "time_not_set");
    if (s_last_time_wait_log_ms == 0 ||
        now_ms - s_last_time_wait_log_ms >= ONENET_CLOUD_TIME_WAIT_LOG_MS)
    {
        ESP_LOGW(TAG, "Waiting for system time before OneNET TLS connection");
        s_last_time_wait_log_ms = now_ms;
    }
    return false;
}

static void update_status(onenet_cloud_status_t status, esp_err_t err, const char *reason)
{
    snapshot_lock();
    s_snapshot.status = status;
    s_snapshot.last_error = err;
    s_snapshot.configured = config_is_complete();
    s_snapshot.mqtt_started = s_mqtt_started;
    s_snapshot.last_status_ms = esp_timer_get_time() / 1000;
    copy_string(s_snapshot.last_failure_reason, sizeof(s_snapshot.last_failure_reason),
                reason != NULL ? reason : onenet_cloud_service_status_name(status));
    snapshot_unlock();
}

static bool get_station_ip(char *ip, size_t ip_len)
{
    if (ip == NULL || ip_len == 0)
    {
        return false;
    }

    ip[0] = '\0';

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL)
    {
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    esp_err_t err = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (err != ESP_OK || ip_info.ip.addr == 0)
    {
        return false;
    }

    snprintf(ip, ip_len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

static bool has_allowed_photo_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
    {
        return false;
    }

    char ext[8] = {};
    size_t ext_len = strlen(dot);
    if (ext_len == 0 || ext_len >= sizeof(ext))
    {
        return false;
    }

    for (size_t i = 0; i < ext_len; ++i)
    {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }

    return strcmp(ext, ".bmp") == 0 || strcmp(ext, ".jpg") == 0 ||
           strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".png") == 0;
}

static bool is_safe_photo_name(const char *name)
{
    if (name == NULL || name[0] == '\0')
    {
        return false;
    }

    for (const char *cursor = name; *cursor != '\0'; ++cursor)
    {
        const unsigned char ch = (unsigned char)*cursor;
        if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.')
        {
            continue;
        }
        return false;
    }

    return has_allowed_photo_extension(name);
}

static esp_err_t scan_photos(onenet_cloud_photo_info_t *latest, uint32_t *photo_count)
{
    bool latest_requested = latest != NULL;
    bool found_photo = false;

    if (photo_count != NULL)
    {
        *photo_count = 0;
    }
    if (latest_requested)
    {
        memset(latest, 0, sizeof(*latest));
    }

#if !CONFIG_EXAMPLE_ENABLE_SD_CARD
    return ESP_ERR_NOT_SUPPORTED;
#else
    DIR *dir = opendir(ONENET_CLOUD_PHOTO_DIR);
    if (dir == NULL)
    {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!is_safe_photo_name(entry->d_name))
        {
            continue;
        }

        char path[ONENET_CLOUD_PHOTO_PATH_MAX_LEN] = {};
        int written = snprintf(path, sizeof(path), "%s/%s", ONENET_CLOUD_PHOTO_DIR, entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(path))
        {
            continue;
        }

        struct stat st = {};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            continue;
        }

        if (photo_count != NULL)
        {
            (*photo_count)++;
        }

        found_photo = true;
        if (latest_requested && (latest->name[0] == '\0' || st.st_mtime > latest->mtime))
        {
            copy_string(latest->name, sizeof(latest->name), entry->d_name);
            copy_string(latest->path, sizeof(latest->path), path);
            latest->size = (size_t)st.st_size;
            latest->mtime = st.st_mtime;
        }
    }

    closedir(dir);
    return latest_requested && !found_photo ? ESP_ERR_NOT_FOUND : ESP_OK;
#endif
}

static bool add_param_number(cJSON *params, const char *name, double value)
{
    cJSON *node = cJSON_CreateObject();
    if (node == NULL)
    {
        return false;
    }

    if (cJSON_AddNumberToObject(node, "value", value) == NULL ||
        !cJSON_AddItemToObject(params, name, node))
    {
        cJSON_Delete(node);
        return false;
    }

    return true;
}

static bool add_param_string(cJSON *params, const char *name, const char *value)
{
    cJSON *node = cJSON_CreateObject();
    if (node == NULL)
    {
        return false;
    }

    if (cJSON_AddStringToObject(node, "value", value != NULL ? value : "") == NULL ||
        !cJSON_AddItemToObject(params, name, node))
    {
        cJSON_Delete(node);
        return false;
    }

    return true;
}

static const char *dht11_platform_value(dht11_service_status_t status)
{
    switch (status)
    {
    case DHT11_SERVICE_STATUS_NEVER_READ:
        return "never_read";
    case DHT11_SERVICE_STATUS_OK:
        return "ok";
    case DHT11_SERVICE_STATUS_TIMEOUT:
        return "timeout";
    case DHT11_SERVICE_STATUS_CHECKSUM_ERROR:
        return "checksum_error";
    case DHT11_SERVICE_STATUS_IO_ERROR:
        return "io_error";
    default:
        return "unknown";
    }
}

static esp_err_t build_property_payload(char **payload)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid payload pointer");
    *payload = NULL;

    char ip[24] = {};
    (void)get_station_ip(ip, sizeof(ip));

    uint32_t photo_count = 0;
    (void)scan_photos(NULL, &photo_count);

    onenet_cloud_snapshot_t snapshot = {};
    (void)onenet_cloud_service_get_snapshot(&snapshot);

    s_property_message_id++;
    if (s_property_message_id == 0)
    {
        s_property_message_id = 1;
    }

    char message_id[ONENET_CLOUD_MESSAGE_ID_BUF_LEN];
    int message_id_len = snprintf(message_id, sizeof(message_id), "%" PRIu32,
                                  s_property_message_id);
    ESP_RETURN_ON_FALSE(message_id_len > 0 && (size_t)message_id_len < sizeof(message_id),
                        ESP_ERR_INVALID_SIZE, TAG, "Failed to format OneJSON message ID");

    cJSON *root = cJSON_CreateObject();
    cJSON *params_obj = cJSON_CreateObject();
    cJSON *params = NULL;
    ESP_GOTO_ON_FALSE(root != NULL && params_obj != NULL, ESP_ERR_NO_MEM, err, TAG,
                      "Failed to create OneJSON root");

    ESP_GOTO_ON_FALSE(cJSON_AddStringToObject(root, "id", message_id) != NULL &&
                          cJSON_AddStringToObject(root, "version", ONENET_CLOUD_ONEJSON_VERSION) != NULL &&
                          cJSON_AddItemToObject(root, "params", params_obj),
                      ESP_ERR_NO_MEM, err, TAG, "Failed to create OneJSON header");
    params = params_obj;
    params_obj = NULL;

#if CONFIG_EXAMPLE_ENABLE_DHT11_SERVICE
    dht11_service_snapshot_t dht = {};
    esp_err_t dht_err = dht11_service_get_snapshot(&dht);
    ESP_GOTO_ON_FALSE(add_param_number(params, "temperature_c",
                                       dht_err == ESP_OK ? dht.temperature_c : 0.0) &&
                          add_param_number(params, "humidity_percent",
                                           dht_err == ESP_OK ? dht.humidity_percent : 0.0) &&
                          add_param_string(params, "DHT11",
                                           dht_err == ESP_OK ? dht11_platform_value(dht.status) : "unavailable"),
                      ESP_ERR_NO_MEM, err, TAG, "Failed to add DHT11 properties");
#else
    ESP_GOTO_ON_FALSE(add_param_number(params, "temperature_c", 0.0) &&
                          add_param_number(params, "humidity_percent", 0.0) &&
                          add_param_string(params, "DHT11", "disabled"),
                      ESP_ERR_NO_MEM, err, TAG, "Failed to add disabled DHT11 properties");
#endif

    ESP_GOTO_ON_FALSE(add_param_string(params, "device_ip", ip) &&
                          add_param_number(params, "photo_count", photo_count) &&
                          add_param_string(params, "latest_photo_name",
                                           snapshot.last_photo_name) &&
                          add_param_string(params, "latest_photo_uuid",
                                           snapshot.last_photo_uuid) &&
                          add_param_string(params, "latest_photo_upload_status",
                                           onenet_cloud_service_status_name(snapshot.status)),
                      ESP_ERR_NO_MEM, err, TAG, "Failed to add common properties");

    *payload = cJSON_PrintUnformatted(root);
    ESP_GOTO_ON_FALSE(*payload != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to print OneJSON");

    cJSON_Delete(root);
    return ESP_OK;

err:
    cJSON_Delete(params_obj);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t publish_properties(void)
{
    if (s_mqtt_client == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[ONENET_CLOUD_TOPIC_MAX_LEN];
    int written = snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/post",
                           s_config.product_id, s_config.device_name);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < sizeof(topic), ESP_ERR_INVALID_SIZE,
                        TAG, "OneNET property topic is too long");

    char *payload = NULL;
    esp_err_t err = build_property_payload(&payload);
    if (err != ESP_OK)
    {
        return err;
    }

    update_status(ONENET_CLOUD_STATUS_PUBLISHING, ESP_OK, NULL);
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    cJSON_free(payload);
    if (msg_id < 0)
    {
        snapshot_lock();
        s_snapshot.failed_publish_count++;
        s_snapshot.last_publish_err = ESP_FAIL;
        snapshot_unlock();
        update_status(ONENET_CLOUD_STATUS_MQTT_ERROR, ESP_FAIL, "mqtt_publish_failed");
        return ESP_FAIL;
    }

    snapshot_lock();
    s_snapshot.publish_count++;
    s_snapshot.last_publish_err = ESP_OK;
    s_snapshot.last_publish_ms = esp_timer_get_time() / 1000;
    snapshot_unlock();
    update_status(ONENET_CLOUD_STATUS_PUBLISHED, ESP_OK, NULL);
    return ESP_OK;
}

static void md5_to_hex(const uint8_t md5[16], char out[ONENET_CLOUD_MD5_HEX_LEN])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i)
    {
        out[i * 2U] = hex[(md5[i] >> 4) & 0x0F];
        out[i * 2U + 1U] = hex[md5[i] & 0x0F];
    }
    out[32] = '\0';
}

static esp_err_t calculate_file_md5(const char *path, char out[ONENET_CLOUD_MD5_HEX_LEN])
{
    FILE *file = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(file != NULL, ESP_FAIL, TAG, "Failed to open photo for MD5");

    mbedtls_md5_context ctx;
    uint8_t digest[16] = {};
    uint8_t buffer[ONENET_CLOUD_FILE_CHUNK_SIZE];
    mbedtls_md5_init(&ctx);

    int ret = mbedtls_md5_starts(&ctx);
    while (ret == 0)
    {
        size_t read_len = fread(buffer, 1, sizeof(buffer), file);
        if (read_len > 0)
        {
            ret = mbedtls_md5_update(&ctx, buffer, read_len);
        }
        if (read_len < sizeof(buffer))
        {
            if (ferror(file))
            {
                ret = -1;
            }
            break;
        }
    }

    if (ret == 0)
    {
        ret = mbedtls_md5_finish(&ctx, digest);
    }

    mbedtls_md5_free(&ctx);
    fclose(file);

    if (ret != 0)
    {
        return ESP_FAIL;
    }

    md5_to_hex(digest, out);
    return ESP_OK;
}

static esp_err_t write_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    size_t offset = 0;
    while (offset < len)
    {
        int written = esp_http_client_write(client, data + offset, (int)(len - offset));
        if (written <= 0)
        {
            return ESP_FAIL;
        }
        offset += (size_t)written;
    }
    return ESP_OK;
}

static bool find_uuid_recursive(const cJSON *node, char *uuid, size_t uuid_len)
{
    if (node == NULL)
    {
        return false;
    }

    const cJSON *uuid_item = cJSON_GetObjectItemCaseSensitive(node, "uuid");
    if (cJSON_IsString(uuid_item) && uuid_item->valuestring != NULL)
    {
        copy_string(uuid, uuid_len, uuid_item->valuestring);
        return true;
    }

    cJSON *child = NULL;
    cJSON_ArrayForEach(child, node)
    {
        if (find_uuid_recursive(child, uuid, uuid_len))
        {
            return true;
        }
    }

    return false;
}

static void parse_upload_uuid(const char *response, char *uuid, size_t uuid_len)
{
    if (response == NULL || uuid == NULL || uuid_len == 0)
    {
        return;
    }

    cJSON *root = cJSON_Parse(response);
    if (root == NULL)
    {
        return;
    }

    (void)find_uuid_recursive(root, uuid, uuid_len);
    cJSON_Delete(root);
}

static esp_err_t upload_photo_http(const onenet_cloud_photo_info_t *photo,
                                   const char *md5_hex,
                                   char *uuid,
                                   size_t uuid_len,
                                   int *http_status)
{
    esp_err_t ret = ESP_OK;
    char url[ONENET_CLOUD_URL_MAX_LEN];
    if (strstr(s_config.api_host, "://") != NULL)
    {
        snprintf(url, sizeof(url), "%s/studio/%s/%s/outupload",
                 s_config.api_host, s_config.product_id, s_config.device_name);
    }
    else
    {
        snprintf(url, sizeof(url), "http://%s/studio/%s/%s/outupload",
                 s_config.api_host, s_config.product_id, s_config.device_name);
    }

    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s",
             ONENET_CLOUD_MULTIPART_BOUNDARY);

    char file_header[256];
    char md5_part[128];
    char filename_part[192];
    char size_part[96];
    char closing[64];
    snprintf(file_header, sizeof(file_header),
             "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
             "Content-Type: image/bmp\r\n\r\n",
             ONENET_CLOUD_MULTIPART_BOUNDARY, photo->name);
    snprintf(md5_part, sizeof(md5_part),
             "\r\n--%s\r\nContent-Disposition: form-data; name=\"md5\"\r\n\r\n%s\r\n",
             ONENET_CLOUD_MULTIPART_BOUNDARY, md5_hex);
    snprintf(filename_part, sizeof(filename_part),
             "--%s\r\nContent-Disposition: form-data; name=\"filename\"\r\n\r\n%s\r\n",
             ONENET_CLOUD_MULTIPART_BOUNDARY, photo->name);
    snprintf(size_part, sizeof(size_part),
             "--%s\r\nContent-Disposition: form-data; name=\"size\"\r\n\r\n%u\r\n",
             ONENET_CLOUD_MULTIPART_BOUNDARY, (unsigned)photo->size);
    snprintf(closing, sizeof(closing), "--%s--\r\n", ONENET_CLOUD_MULTIPART_BOUNDARY);

    const size_t content_length = strlen(file_header) + photo->size + strlen(md5_part) +
                                  strlen(filename_part) + strlen(size_part) + strlen(closing);

    esp_http_client_config_t http_config = {};
    http_config.url = url;
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = ONENET_CLOUD_HTTP_TIMEOUT_MS;
    http_config.buffer_size = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create HTTP client");

    FILE *file = NULL;
    esp_http_client_set_header(client, "Authorization", s_config.auth_token);
    esp_http_client_set_header(client, "Content-Type", content_type);

    ret = esp_http_client_open(client, (int)content_length);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to open OneNET upload request");

    ESP_GOTO_ON_ERROR(write_all(client, file_header, strlen(file_header)), cleanup, TAG,
                      "Failed to write upload header");

    file = fopen(photo->path, "rb");
    ESP_GOTO_ON_FALSE(file != NULL, ESP_FAIL, cleanup, TAG, "Failed to open photo for upload");

    char buffer[ONENET_CLOUD_FILE_CHUNK_SIZE];
    while (true)
    {
        size_t read_len = fread(buffer, 1, sizeof(buffer), file);
        if (read_len > 0)
        {
            ESP_GOTO_ON_ERROR(write_all(client, buffer, read_len), cleanup, TAG,
                              "Failed to write photo body");
        }
        if (read_len < sizeof(buffer))
        {
            ESP_GOTO_ON_FALSE(!ferror(file), ESP_FAIL, cleanup, TAG, "Failed to read photo");
            break;
        }
    }

    ESP_GOTO_ON_ERROR(write_all(client, md5_part, strlen(md5_part)), cleanup, TAG,
                      "Failed to write md5 field");
    ESP_GOTO_ON_ERROR(write_all(client, filename_part, strlen(filename_part)), cleanup, TAG,
                      "Failed to write filename field");
    ESP_GOTO_ON_ERROR(write_all(client, size_part, strlen(size_part)), cleanup, TAG,
                      "Failed to write size field");
    ESP_GOTO_ON_ERROR(write_all(client, closing, strlen(closing)), cleanup, TAG,
                      "Failed to write closing field");

    (void)esp_http_client_fetch_headers(client);
    *http_status = esp_http_client_get_status_code(client);

    char response[ONENET_CLOUD_HTTP_RESPONSE_LEN] = {};
    int read_len = esp_http_client_read_response(client, response, sizeof(response) - 1);
    if (read_len > 0)
    {
        response[read_len] = '\0';
        parse_upload_uuid(response, uuid, uuid_len);
    }

    ESP_GOTO_ON_FALSE(*http_status >= 200 && *http_status < 300, ESP_ERR_INVALID_RESPONSE,
                      cleanup, TAG, "OneNET upload HTTP status=%d", *http_status);
    ESP_GOTO_ON_FALSE(uuid == NULL || uuid[0] != '\0', ESP_ERR_INVALID_RESPONSE,
                      cleanup, TAG, "OneNET upload response has no uuid");

cleanup:
    if (file != NULL)
    {
        fclose(file);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static esp_err_t upload_latest_photo(void)
{
    onenet_cloud_photo_info_t photo = {};
    uint32_t photo_count = 0;
    esp_err_t err = scan_photos(&photo, &photo_count);

    snapshot_lock();
    s_snapshot.photo_count = photo_count;
    snapshot_unlock();

    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGI(TAG, "No photo available for OneNET upload");
        update_status(ONENET_CLOUD_STATUS_NO_PHOTO, err, "no_photo");
        return err;
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to scan photos for OneNET upload: %s", esp_err_to_name(err));
        update_status(ONENET_CLOUD_STATUS_INTERNAL_ERROR, err, "photo_scan_failed");
        return err;
    }

    if (photo.size > s_config.photo_max_bytes)
    {
        snapshot_lock();
        s_snapshot.last_photo_upload_err = ESP_ERR_INVALID_SIZE;
        s_snapshot.last_photo_size = photo.size;
        copy_string(s_snapshot.last_photo_name, sizeof(s_snapshot.last_photo_name), photo.name);
        snapshot_unlock();
        ESP_LOGW(TAG, "OneNET photo exceeds upload limit: size=%u limit=%u",
                 (unsigned)photo.size, (unsigned)s_config.photo_max_bytes);
        update_status(ONENET_CLOUD_STATUS_FILE_TOO_LARGE, ESP_ERR_INVALID_SIZE, "file_too_large");
        return ESP_ERR_INVALID_SIZE;
    }

    onenet_cloud_snapshot_t snapshot = {};
    (void)onenet_cloud_service_get_snapshot(&snapshot);
    if (!s_photo_upload_requested && strcmp(snapshot.last_photo_name, photo.name) == 0 &&
        snapshot.last_photo_uuid[0] != '\0')
    {
        return ESP_OK;
    }

    char md5_hex[ONENET_CLOUD_MD5_HEX_LEN] = {};
    err = calculate_file_md5(photo.path, md5_hex);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to calculate photo MD5 for OneNET upload: %s",
                 esp_err_to_name(err));
        update_status(ONENET_CLOUD_STATUS_INTERNAL_ERROR, err, "md5_failed");
        return err;
    }

    ESP_LOGI(TAG, "Pausing OneNET MQTT before photo HTTP upload");
    destroy_mqtt_client();
    vTaskDelay(pdMS_TO_TICKS(ONENET_CLOUD_MQTT_BEFORE_HTTP_SETTLE_MS));

    update_status(ONENET_CLOUD_STATUS_UPLOADING_PHOTO, ESP_OK, NULL);

    char uuid[ONENET_CLOUD_PHOTO_UUID_MAX_LEN] = {};
    int http_status = 0;
    err = upload_photo_http(&photo, md5_hex, uuid, sizeof(uuid), &http_status);
    s_next_mqtt_start_ms = esp_timer_get_time() / 1000 +
                           ONENET_CLOUD_MQTT_AFTER_HTTP_SETTLE_MS;

    snapshot_lock();
    s_snapshot.last_http_status = http_status;
    s_snapshot.last_photo_upload_err = err;
    s_snapshot.last_photo_upload_ms = esp_timer_get_time() / 1000;
    s_snapshot.last_photo_size = photo.size;
    copy_string(s_snapshot.last_photo_name, sizeof(s_snapshot.last_photo_name), photo.name);
    if (err == ESP_OK)
    {
        s_snapshot.photo_upload_count++;
        copy_string(s_snapshot.last_photo_uuid, sizeof(s_snapshot.last_photo_uuid), uuid);
    }
    else
    {
        s_snapshot.failed_photo_upload_count++;
    }
    snapshot_unlock();

    s_photo_upload_requested = false;
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "OneNET photo upload succeeded: size=%u http_status=%d uuid_present=1",
                 (unsigned)photo.size, http_status);
    }
    else
    {
        ESP_LOGW(TAG, "OneNET photo upload failed: size=%u http_status=%d err=%s",
                 (unsigned)photo.size, http_status, esp_err_to_name(err));
    }
    update_status(err == ESP_OK ? ONENET_CLOUD_STATUS_UPLOAD_OK : ONENET_CLOUD_STATUS_HTTP_ERROR,
                  err, err == ESP_OK ? NULL : "photo_upload_failed");
    return err;
}

static esp_err_t parse_mqtt_endpoint(onenet_cloud_mqtt_endpoint_t *endpoint)
{
    ESP_RETURN_ON_FALSE(endpoint != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid MQTT endpoint");
    ESP_RETURN_ON_FALSE(string_has_value(s_config.mqtt_host), ESP_ERR_INVALID_ARG, TAG, "Empty MQTT host");

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->port = s_config.mqtt_port != 0 ? s_config.mqtt_port : 1883;
    endpoint->transport = endpoint->port == 8883 ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;

    const char *host_start = s_config.mqtt_host;
    const char *scheme_end = strstr(s_config.mqtt_host, "://");
    if (scheme_end != NULL)
    {
        const size_t scheme_len = (size_t)(scheme_end - s_config.mqtt_host);
        if (scheme_len == strlen("mqtt") && strncmp(s_config.mqtt_host, "mqtt", scheme_len) == 0)
        {
            endpoint->transport = MQTT_TRANSPORT_OVER_TCP;
            endpoint->port = s_config.mqtt_port != 0 ? s_config.mqtt_port : 1883;
        }
        else if (scheme_len == strlen("mqtts") && strncmp(s_config.mqtt_host, "mqtts", scheme_len) == 0)
        {
            endpoint->transport = MQTT_TRANSPORT_OVER_SSL;
            endpoint->port = s_config.mqtt_port != 0 ? s_config.mqtt_port : 8883;
        }
        else
        {
            ESP_LOGE(TAG, "Unsupported OneNET MQTT scheme in host config");
            return ESP_ERR_INVALID_ARG;
        }
        host_start = scheme_end + strlen("://");
    }

    const char *host_end = strpbrk(host_start, "/?#");
    const size_t host_port_len = host_end != NULL ? (size_t)(host_end - host_start) : strlen(host_start);
    ESP_RETURN_ON_FALSE(host_port_len > 0 && host_port_len < ONENET_CLOUD_HOST_MAX_LEN,
                        ESP_ERR_INVALID_SIZE, TAG, "Invalid OneNET MQTT host length");

    char host_port[ONENET_CLOUD_HOST_MAX_LEN] = {};
    memcpy(host_port, host_start, host_port_len);
    host_port[host_port_len] = '\0';

    char *port_start = strrchr(host_port, ':');
    if (port_start != NULL)
    {
        *port_start = '\0';
        port_start++;
        ESP_RETURN_ON_FALSE(port_start[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                            "Empty OneNET MQTT port");
        char *end = NULL;
        long parsed_port = strtol(port_start, &end, 10);
        ESP_RETURN_ON_FALSE(end != port_start && *end == '\0' &&
                                parsed_port > 0 && parsed_port <= 65535,
                            ESP_ERR_INVALID_ARG, TAG, "Invalid OneNET MQTT port");
        endpoint->port = (uint16_t)parsed_port;
    }

    ESP_RETURN_ON_FALSE(host_port[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                        "Empty OneNET MQTT hostname");
    copy_string(endpoint->hostname, sizeof(endpoint->hostname), host_port);
    if (endpoint->port == 8883)
    {
        endpoint->transport = MQTT_TRANSPORT_OVER_SSL;
    }
    return ESP_OK;
}

static void destroy_mqtt_client(void)
{
    if (s_mqtt_client != NULL)
    {
        if (s_mqtt_started)
        {
            (void)esp_mqtt_client_stop(s_mqtt_client);
        }
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }
    s_mqtt_started = false;
    s_mqtt_restart_requested = false;
    snapshot_lock();
    s_snapshot.mqtt_started = false;
    s_snapshot.mqtt_connected = false;
    snapshot_unlock();
}

static void schedule_mqtt_retry(const char *reason)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const uint32_t delay_ms = s_mqtt_retry_delay_ms != 0
                                  ? s_mqtt_retry_delay_ms
                                  : ONENET_CLOUD_MQTT_RETRY_MIN_MS;
    s_next_mqtt_start_ms = now_ms + delay_ms;

    uint32_t next_delay_ms = delay_ms * 2U;
    if (next_delay_ms < delay_ms || next_delay_ms > ONENET_CLOUD_MQTT_RETRY_MAX_MS)
    {
        next_delay_ms = ONENET_CLOUD_MQTT_RETRY_MAX_MS;
    }
    s_mqtt_retry_delay_ms = next_delay_ms;

    ESP_LOGW(TAG, "OneNET MQTT retry scheduled in %" PRIu32 " ms: %s",
             delay_ms, reason != NULL ? reason : "mqtt_retry");
    update_status(ONENET_CLOUD_STATUS_MQTT_ERROR, ESP_FAIL, reason);
}

static bool mqtt_event_topic_matches(esp_mqtt_event_handle_t event, const char *topic_suffix)
{
    if (event == NULL || event->topic == NULL || topic_suffix == NULL)
    {
        return false;
    }

    char expected_topic[ONENET_CLOUD_TOPIC_MAX_LEN];
    int written = snprintf(expected_topic, sizeof(expected_topic), "$sys/%s/%s/%s",
                           s_config.product_id, s_config.device_name, topic_suffix);
    return written > 0 && (size_t)written < sizeof(expected_topic) &&
           event->topic_len == written && memcmp(event->topic, expected_topic, (size_t)written) == 0;
}

static void handle_property_post_reply(esp_mqtt_event_handle_t event)
{
    if (event->data == NULL || event->data_len <= 0 || event->current_data_offset != 0 ||
        event->data_len != event->total_data_len)
    {
        ESP_LOGW(TAG, "Ignored fragmented OneNET property report reply: offset=%d len=%d total=%d",
                 event->current_data_offset, event->data_len, event->total_data_len);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(event->data, (size_t)event->data_len);
    if (root == NULL)
    {
        ESP_LOGW(TAG, "Invalid OneNET property report reply JSON: data_len=%d", event->data_len);
        return;
    }

    bool code_valid = false;
    int reply_code = 0;
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code))
    {
        reply_code = code->valueint;
        code_valid = true;
    }
    else if (cJSON_IsString(code) && code->valuestring != NULL)
    {
        char *end = NULL;
        long parsed = strtol(code->valuestring, &end, 10);
        if (end != code->valuestring && *end == '\0' && parsed >= INT_MIN && parsed <= INT_MAX)
        {
            reply_code = (int)parsed;
            code_valid = true;
        }
    }

    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "msg");
    if (!cJSON_IsString(message))
    {
        message = cJSON_GetObjectItemCaseSensitive(root, "message");
    }
    const char *message_text = cJSON_IsString(message) && message->valuestring != NULL
                                   ? message->valuestring
                                   : "";
    int message_len = (int)strlen(message_text);
    if (message_len > ONENET_CLOUD_REPLY_MESSAGE_LOG_MAX_LEN)
    {
        message_len = ONENET_CLOUD_REPLY_MESSAGE_LOG_MAX_LEN;
    }

    if (!code_valid)
    {
        ESP_LOGW(TAG, "OneNET property report reply has no valid code: msg=%.*s",
                 message_len, message_text);
    }
    else if (reply_code == ONENET_CLOUD_PROPERTY_REPLY_SUCCESS_CODE)
    {
        ESP_LOGI(TAG, "OneNET property report accepted: code=%d msg=%.*s",
                 reply_code, message_len, message_text);
    }
    else
    {
        ESP_LOGW(TAG, "OneNET property report rejected: code=%d msg=%.*s",
                 reply_code, message_len, message_text);
    }

    cJSON_Delete(root);
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
    {
        char topic[ONENET_CLOUD_TOPIC_MAX_LEN];
        int written = snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/set",
                               s_config.product_id, s_config.device_name);
        if (written > 0 && (size_t)written < sizeof(topic))
        {
            (void)esp_mqtt_client_subscribe(event->client, topic, 1);
        }

        written = snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/post/reply",
                           s_config.product_id, s_config.device_name);
        if (written > 0 && (size_t)written < sizeof(topic))
        {
            (void)esp_mqtt_client_subscribe(event->client, topic, 1);
        }

        snapshot_lock();
        s_snapshot.mqtt_connected = true;
        snapshot_unlock();
        s_mqtt_retry_delay_ms = ONENET_CLOUD_MQTT_RETRY_MIN_MS;
        s_next_mqtt_start_ms = 0;
        s_mqtt_restart_requested = false;
        update_status(ONENET_CLOUD_STATUS_CONNECTED, ESP_OK, NULL);
        ESP_LOGI(TAG, "OneNET MQTT connected");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        snapshot_lock();
        s_snapshot.mqtt_connected = false;
        snapshot_unlock();
        s_mqtt_restart_requested = true;
        update_status(ONENET_CLOUD_STATUS_MQTT_ERROR, ESP_FAIL, "mqtt_disconnected");
        ESP_LOGW(TAG, "OneNET MQTT disconnected");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "OneNET MQTT publish acknowledged: msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        if (mqtt_event_topic_matches(event, "thing/property/post/reply"))
        {
            handle_property_post_reply(event);
        }
        else
        {
            ESP_LOGI(TAG, "Ignored OneNET property set payload: topic_len=%d data_len=%d",
                     event->topic_len, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        snapshot_lock();
        s_snapshot.mqtt_connected = false;
        s_snapshot.last_publish_err = ESP_FAIL;
        snapshot_unlock();
        s_mqtt_restart_requested = true;
        update_status(ONENET_CLOUD_STATUS_MQTT_ERROR, ESP_FAIL, "mqtt_error");
        ESP_LOGE(TAG, "OneNET MQTT error event");
        break;
    default:
        break;
    }
}

static esp_err_t ensure_mqtt_started(void)
{
    if (s_mqtt_started)
    {
        return ESP_OK;
    }

    onenet_cloud_mqtt_endpoint_t endpoint = {};
    esp_err_t err = parse_mqtt_endpoint(&endpoint);
    if (err != ESP_OK)
    {
        update_status(ONENET_CLOUD_STATUS_INTERNAL_ERROR, err, "mqtt_endpoint_invalid");
        return err;
    }

    esp_mqtt_client_config_t mqtt_config = {};
    mqtt_config.broker.address.hostname = endpoint.hostname;
    mqtt_config.broker.address.port = endpoint.port;
    mqtt_config.broker.address.transport = endpoint.transport;
    if (endpoint.transport == MQTT_TRANSPORT_OVER_SSL)
    {
        if (endpoint_uses_onenet_mqtt_ca(&endpoint))
        {
            mqtt_config.broker.verification.certificate = (const char *)onenet_mqtt_root_pem_start;
            mqtt_config.broker.verification.common_name = ONENET_CLOUD_MQTT_CERT_COMMON_NAME;
        }
        else
        {
            mqtt_config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }
    mqtt_config.credentials.client_id = s_config.device_name;
    mqtt_config.credentials.username = s_config.product_id;
    mqtt_config.credentials.authentication.password = s_config.auth_token;
    mqtt_config.network.timeout_ms = ONENET_CLOUD_MQTT_NETWORK_TIMEOUT_MS;
    mqtt_config.network.disable_auto_reconnect = true;
    mqtt_config.task.stack_size = 4096;

    s_mqtt_client = esp_mqtt_client_init(&mqtt_config);
    if (s_mqtt_client == NULL)
    {
        update_status(ONENET_CLOUD_STATUS_INTERNAL_ERROR, ESP_ERR_NO_MEM, "mqtt_init_failed");
        return ESP_ERR_NO_MEM;
    }

    err = esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                         mqtt_event_handler, NULL);
    if (err != ESP_OK)
    {
        destroy_mqtt_client();
        update_status(ONENET_CLOUD_STATUS_INTERNAL_ERROR, err, "mqtt_event_register_failed");
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK)
    {
        destroy_mqtt_client();
        update_status(ONENET_CLOUD_STATUS_MQTT_ERROR, err, "mqtt_start_failed");
        return err;
    }

    s_mqtt_started = true;
    snapshot_lock();
    s_snapshot.mqtt_started = true;
    snapshot_unlock();
    update_status(ONENET_CLOUD_STATUS_CONNECTING, ESP_OK, NULL);
    ESP_LOGI(TAG, "OneNET MQTT client started for product=%s device=%s host=%s port=%u transport=%d",
             s_config.product_id, s_config.device_name, endpoint.hostname,
             (unsigned)endpoint.port, (int)endpoint.transport);
    return ESP_OK;
}

static bool snapshot_mqtt_connected(void)
{
    bool connected = false;
    snapshot_lock();
    connected = s_snapshot.mqtt_connected;
    snapshot_unlock();
    return connected;
}

static void onenet_cloud_task(void *arg)
{
    (void)arg;
    int64_t last_publish_ms = 0;
    int64_t last_photo_attempt_ms = 0;

    while (!s_stop_requested)
    {
        if (!s_config.enabled)
        {
            update_status(ONENET_CLOUD_STATUS_DISABLED, ESP_OK, NULL);
            vTaskDelay(pdMS_TO_TICKS(ONENET_CLOUD_LOOP_PERIOD_MS));
            continue;
        }

        if (!config_is_complete())
        {
            update_status(ONENET_CLOUD_STATUS_NOT_CONFIGURED, ESP_ERR_INVALID_STATE,
                          "missing_product_device_or_token");
            vTaskDelay(pdMS_TO_TICKS(ONENET_CLOUD_LOOP_PERIOD_MS));
            continue;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        const uint32_t now32_ms = (uint32_t)now_ms;
        const uint32_t pause_remaining_ms = network_pause_remaining_ms(now32_ms);
        if (pause_remaining_ms > 0)
        {
            if (s_last_network_pause_log_ms == 0 ||
                now_ms - s_last_network_pause_log_ms >= ONENET_CLOUD_NETWORK_PAUSE_LOG_MS)
            {
                ESP_LOGW(TAG, "OneNET network paused for another %" PRIu32 " ms",
                         pause_remaining_ms);
                s_last_network_pause_log_ms = now_ms;
            }
            if (s_mqtt_client != NULL || s_mqtt_started || s_mqtt_restart_requested)
            {
                destroy_mqtt_client();
            }
            s_next_mqtt_start_ms = now_ms + pause_remaining_ms;
            update_status(ONENET_CLOUD_STATUS_NETWORK_PAUSED, ESP_OK, "network_pause");
            vTaskDelay(pdMS_TO_TICKS(ONENET_CLOUD_LOOP_PERIOD_MS));
            continue;
        }
        s_last_network_pause_log_ms = 0;

        char ip[24] = {};
        if (!get_station_ip(ip, sizeof(ip)))
        {
            update_status(ONENET_CLOUD_STATUS_NO_IP, ESP_ERR_INVALID_STATE, "wifi_no_ip");
            if (s_last_no_ip_log_ms == 0 ||
                now_ms - s_last_no_ip_log_ms >= ONENET_CLOUD_NO_IP_LOG_MS)
            {
                ESP_LOGW(TAG, "Waiting for Wi-Fi IP before OneNET connection");
                s_last_no_ip_log_ms = now_ms;
            }
            vTaskDelay(pdMS_TO_TICKS(ONENET_CLOUD_LOOP_PERIOD_MS));
            continue;
        }
        s_last_no_ip_log_ms = 0;

        if (!ensure_time_ready_for_tls(now_ms))
        {
            vTaskDelay(pdMS_TO_TICKS(ONENET_CLOUD_LOOP_PERIOD_MS));
            continue;
        }

        if (s_mqtt_restart_requested)
        {
            destroy_mqtt_client();
            schedule_mqtt_retry("mqtt_restart");
        }

        if (!s_mqtt_started && now_ms >= s_next_mqtt_start_ms)
        {
            esp_err_t mqtt_err = ensure_mqtt_started();
            if (mqtt_err != ESP_OK)
            {
                destroy_mqtt_client();
                schedule_mqtt_retry("mqtt_start_failed");
            }
        }

        if (s_config.photo_upload_enabled && snapshot_mqtt_connected() &&
            (s_photo_upload_requested ||
             now_ms - last_photo_attempt_ms >= s_config.photo_upload_cooldown_ms))
        {
            last_photo_attempt_ms = now_ms;
            (void)upload_latest_photo();
            vTaskDelay(pdMS_TO_TICKS(ONENET_CLOUD_LOOP_PERIOD_MS));
            continue;
        }

        if (snapshot_mqtt_connected() && now_ms - last_publish_ms >= s_config.upload_interval_ms)
        {
            if (publish_properties() == ESP_OK)
            {
                last_publish_ms = now_ms;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ONENET_CLOUD_LOOP_PERIOD_MS));
    }

    destroy_mqtt_client();

    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

esp_err_t onenet_cloud_service_init(const onenet_cloud_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid OneNET config");
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_snapshot_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_snapshot_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to create snapshot mutex");

    memset(&s_config, 0, sizeof(s_config));
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_config.enabled = config->enabled;
    s_config.photo_upload_enabled = config->photo_upload_enabled;
    s_config.mqtt_port = config->mqtt_port != 0 ? config->mqtt_port : 1883;
    s_config.upload_interval_ms = config->upload_interval_ms != 0
                                      ? config->upload_interval_ms
                                      : ONENET_CLOUD_DEFAULT_UPLOAD_INTERVAL_MS;
    s_config.photo_upload_cooldown_ms = config->photo_upload_cooldown_ms != 0
                                            ? config->photo_upload_cooldown_ms
                                            : ONENET_CLOUD_DEFAULT_PHOTO_COOLDOWN_MS;
    s_config.photo_max_bytes = config->photo_max_bytes != 0
                                   ? config->photo_max_bytes
                                   : ONENET_CLOUD_DEFAULT_PHOTO_MAX_BYTES;
    s_config.task_stack_size = config->task_stack_size != 0
                                   ? config->task_stack_size
                                   : ONENET_CLOUD_DEFAULT_TASK_STACK_SIZE;
    s_config.task_priority = config->task_priority != 0
                                 ? config->task_priority
                                 : ONENET_CLOUD_DEFAULT_TASK_PRIORITY;
    copy_string(s_config.product_id, sizeof(s_config.product_id), config->product_id);
    copy_string(s_config.device_name, sizeof(s_config.device_name), config->device_name);
    copy_string(s_config.auth_token, sizeof(s_config.auth_token), config->auth_token);
    copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), config->mqtt_host);
    copy_string(s_config.api_host, sizeof(s_config.api_host), config->api_host);

    snapshot_lock();
    s_snapshot.initialized = true;
    s_snapshot.enabled = s_config.enabled;
    s_snapshot.configured = config_is_complete();
    s_snapshot.photo_upload_enabled = s_config.photo_upload_enabled;
    s_snapshot.status = s_config.enabled ? ONENET_CLOUD_STATUS_CONNECTING
                                         : ONENET_CLOUD_STATUS_DISABLED;
    copy_string(s_snapshot.product_id, sizeof(s_snapshot.product_id), s_config.product_id);
    copy_string(s_snapshot.device_name, sizeof(s_snapshot.device_name), s_config.device_name);
    copy_string(s_snapshot.mqtt_host, sizeof(s_snapshot.mqtt_host), s_config.mqtt_host);
    copy_string(s_snapshot.api_host, sizeof(s_snapshot.api_host), s_config.api_host);
    copy_string(s_snapshot.last_failure_reason, sizeof(s_snapshot.last_failure_reason),
                onenet_cloud_service_status_name(s_snapshot.status));
    snapshot_unlock();

    ESP_LOGI(TAG,
             "OneNET cloud service initialized: enabled=%d configured=%d product=%s device=%s mqtt=%s:%u api=%s photo_upload=%d interval_ms=%" PRIu32,
             s_config.enabled, config_is_complete(), s_config.product_id, s_config.device_name,
             s_config.mqtt_host, (unsigned)s_config.mqtt_port, s_config.api_host,
             s_config.photo_upload_enabled, s_config.upload_interval_ms);

    s_initialized = true;
    s_stop_requested = false;
    s_photo_upload_requested = false;
    s_mqtt_restart_requested = false;
    s_mqtt_retry_delay_ms = ONENET_CLOUD_MQTT_RETRY_MIN_MS;
    s_next_mqtt_start_ms = 0;

    if (!s_config.enabled)
    {
        ESP_LOGI(TAG, "OneNET cloud service is disabled");
        return ESP_OK;
    }

    s_done_sem = xSemaphoreCreateBinary();
    if (s_done_sem == NULL)
    {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        reset_runtime_state();
        ESP_LOGE(TAG, "Failed to create OneNET stop semaphore");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(onenet_cloud_task, ONENET_CLOUD_TASK_NAME,
                                     s_config.task_stack_size, NULL,
                                     s_config.task_priority, &s_task_handle);
    if (created != pdPASS)
    {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        reset_runtime_state();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t onenet_cloud_service_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }

    s_stop_requested = true;
    if (s_task_handle != NULL && s_done_sem != NULL)
    {
        if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(ONENET_CLOUD_STOP_WAIT_MS)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Timed out waiting for OneNET cloud task to stop");
            return ESP_ERR_TIMEOUT;
        }
    }

    if (s_done_sem != NULL)
    {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
    }
    if (s_snapshot_mutex != NULL)
    {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
    }

    s_task_handle = NULL;
    s_initialized = false;
    return ESP_OK;
}

esp_err_t onenet_cloud_service_pause_network(uint32_t pause_ms)
{
    if (!s_initialized || !s_config.enabled)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (pause_ms == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (pause_ms > ONENET_CLOUD_NETWORK_PAUSE_MAX_MS)
    {
        pause_ms = ONENET_CLOUD_NETWORK_PAUSE_MAX_MS;
    }

    const uint32_t now_ms = now_millis32();
    const uint32_t pause_until_ms = now_ms + pause_ms;
    const uint32_t current_until_ms = s_network_pause_until_ms;
    if (!network_pause_active(now_ms) ||
        (int32_t)(pause_until_ms - current_until_ms) > 0)
    {
        s_network_pause_until_ms = pause_until_ms;
    }

    s_mqtt_restart_requested = true;
    update_status(ONENET_CLOUD_STATUS_NETWORK_PAUSED, ESP_OK, "network_pause_requested");
    return ESP_OK;
}

esp_err_t onenet_cloud_service_request_photo_upload(void)
{
    if (!s_initialized || !s_config.enabled)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_photo_upload_requested = true;
    update_status(ONENET_CLOUD_STATUS_UPLOAD_PENDING, ESP_OK, NULL);
    return ESP_OK;
}

esp_err_t onenet_cloud_service_get_snapshot(onenet_cloud_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid snapshot pointer");

    if (!s_initialized || s_snapshot_mutex == NULL)
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->status = ONENET_CLOUD_STATUS_DISABLED;
        copy_string(snapshot->last_failure_reason, sizeof(snapshot->last_failure_reason),
                    "service_not_started");
        return ESP_OK;
    }

    snapshot_lock();
    *snapshot = s_snapshot;
    snapshot_unlock();
    return ESP_OK;
}

const char *onenet_cloud_service_status_name(onenet_cloud_status_t status)
{
    switch (status)
    {
    case ONENET_CLOUD_STATUS_DISABLED:
        return "disabled";
    case ONENET_CLOUD_STATUS_NOT_CONFIGURED:
        return "not_configured";
    case ONENET_CLOUD_STATUS_NO_IP:
        return "no_ip";
    case ONENET_CLOUD_STATUS_TIME_SYNC:
        return "time_sync";
    case ONENET_CLOUD_STATUS_NETWORK_PAUSED:
        return "network_paused";
    case ONENET_CLOUD_STATUS_CONNECTING:
        return "connecting";
    case ONENET_CLOUD_STATUS_CONNECTED:
        return "connected";
    case ONENET_CLOUD_STATUS_PUBLISHING:
        return "publishing";
    case ONENET_CLOUD_STATUS_PUBLISHED:
        return "published";
    case ONENET_CLOUD_STATUS_UPLOAD_PENDING:
        return "upload_pending";
    case ONENET_CLOUD_STATUS_UPLOADING_PHOTO:
        return "uploading_photo";
    case ONENET_CLOUD_STATUS_UPLOAD_OK:
        return "upload_ok";
    case ONENET_CLOUD_STATUS_NO_PHOTO:
        return "no_photo";
    case ONENET_CLOUD_STATUS_FILE_TOO_LARGE:
        return "file_too_large";
    case ONENET_CLOUD_STATUS_MQTT_ERROR:
        return "mqtt_error";
    case ONENET_CLOUD_STATUS_HTTP_ERROR:
        return "http_error";
    case ONENET_CLOUD_STATUS_INTERNAL_ERROR:
        return "internal_error";
    default:
        return "unknown";
    }
}

const char *onenet_cloud_service_status_text(onenet_cloud_status_t status)
{
    switch (status)
    {
    case ONENET_CLOUD_STATUS_DISABLED:
        return "未启用 OneNET";
    case ONENET_CLOUD_STATUS_NOT_CONFIGURED:
        return "OneNET 未配置";
    case ONENET_CLOUD_STATUS_NO_IP:
        return "等待 Wi-Fi IP";
    case ONENET_CLOUD_STATUS_TIME_SYNC:
        return "等待系统时间";
    case ONENET_CLOUD_STATUS_NETWORK_PAUSED:
        return "OneNET network paused";
    case ONENET_CLOUD_STATUS_CONNECTING:
        return "OneNET 连接中";
    case ONENET_CLOUD_STATUS_CONNECTED:
        return "OneNET 已连接";
    case ONENET_CLOUD_STATUS_PUBLISHING:
        return "正在上报数据";
    case ONENET_CLOUD_STATUS_PUBLISHED:
        return "数据已上报";
    case ONENET_CLOUD_STATUS_UPLOAD_PENDING:
        return "照片待上传";
    case ONENET_CLOUD_STATUS_UPLOADING_PHOTO:
        return "正在上传照片";
    case ONENET_CLOUD_STATUS_UPLOAD_OK:
        return "照片已上传";
    case ONENET_CLOUD_STATUS_NO_PHOTO:
        return "暂无可上传照片";
    case ONENET_CLOUD_STATUS_FILE_TOO_LARGE:
        return "照片超过大小限制";
    case ONENET_CLOUD_STATUS_MQTT_ERROR:
        return "OneNET MQTT 异常";
    case ONENET_CLOUD_STATUS_HTTP_ERROR:
        return "OneNET 文件上传异常";
    case ONENET_CLOUD_STATUS_INTERNAL_ERROR:
        return "OneNET 内部异常";
    default:
        return "OneNET 状态未知";
    }
}
