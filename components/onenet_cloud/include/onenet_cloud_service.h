/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ONENET_CLOUD_PRODUCT_ID_MAX_LEN     (64U)
#define ONENET_CLOUD_DEVICE_NAME_MAX_LEN    (64U)
#define ONENET_CLOUD_HOST_MAX_LEN           (96U)
#define ONENET_CLOUD_AUTH_TOKEN_MAX_LEN     (256U)
#define ONENET_CLOUD_PHOTO_NAME_MAX_LEN     (96U)
#define ONENET_CLOUD_PHOTO_UUID_MAX_LEN     (80U)
#define ONENET_CLOUD_FAILURE_REASON_MAX_LEN (96U)
#define ONENET_CLOUD_DEFAULT_UPLOAD_INTERVAL_MS       (30000U)
#define ONENET_CLOUD_DEFAULT_PHOTO_COOLDOWN_MS        (60000U)
#define ONENET_CLOUD_DEFAULT_PHOTO_MAX_BYTES          (1536U * 1024U)
#define ONENET_CLOUD_DEFAULT_TASK_STACK_SIZE          (8192U)
#define ONENET_CLOUD_DEFAULT_TASK_PRIORITY            (5U)

typedef enum
{
    ONENET_CLOUD_STATUS_DISABLED = 0,
    ONENET_CLOUD_STATUS_NOT_CONFIGURED,
    ONENET_CLOUD_STATUS_NO_IP,
    ONENET_CLOUD_STATUS_CONNECTING,
    ONENET_CLOUD_STATUS_CONNECTED,
    ONENET_CLOUD_STATUS_PUBLISHING,
    ONENET_CLOUD_STATUS_PUBLISHED,
    ONENET_CLOUD_STATUS_UPLOAD_PENDING,
    ONENET_CLOUD_STATUS_UPLOADING_PHOTO,
    ONENET_CLOUD_STATUS_UPLOAD_OK,
    ONENET_CLOUD_STATUS_NO_PHOTO,
    ONENET_CLOUD_STATUS_FILE_TOO_LARGE,
    ONENET_CLOUD_STATUS_MQTT_ERROR,
    ONENET_CLOUD_STATUS_HTTP_ERROR,
    ONENET_CLOUD_STATUS_INTERNAL_ERROR,
} onenet_cloud_status_t;

typedef struct
{
    bool enabled;
    const char *product_id;
    const char *device_name;
    const char *auth_token;
    const char *mqtt_host;
    uint16_t mqtt_port;
    const char *api_host;
    uint32_t upload_interval_ms;
    bool photo_upload_enabled;
    uint32_t photo_upload_cooldown_ms;
    size_t photo_max_bytes;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} onenet_cloud_config_t;

typedef struct
{
    bool initialized;
    bool enabled;
    bool configured;
    bool mqtt_started;
    bool mqtt_connected;
    bool photo_upload_enabled;
    onenet_cloud_status_t status;
    esp_err_t last_error;
    esp_err_t last_publish_err;
    esp_err_t last_photo_upload_err;
    int last_http_status;
    int64_t last_status_ms;
    int64_t last_publish_ms;
    int64_t last_photo_upload_ms;
    uint32_t publish_count;
    uint32_t failed_publish_count;
    uint32_t photo_upload_count;
    uint32_t failed_photo_upload_count;
    uint32_t photo_count;
    size_t last_photo_size;
    char product_id[ONENET_CLOUD_PRODUCT_ID_MAX_LEN];
    char device_name[ONENET_CLOUD_DEVICE_NAME_MAX_LEN];
    char mqtt_host[ONENET_CLOUD_HOST_MAX_LEN];
    char api_host[ONENET_CLOUD_HOST_MAX_LEN];
    char last_photo_name[ONENET_CLOUD_PHOTO_NAME_MAX_LEN];
    char last_photo_uuid[ONENET_CLOUD_PHOTO_UUID_MAX_LEN];
    char last_failure_reason[ONENET_CLOUD_FAILURE_REASON_MAX_LEN];
} onenet_cloud_snapshot_t;

esp_err_t onenet_cloud_service_init(const onenet_cloud_config_t *config);
esp_err_t onenet_cloud_service_deinit(void);
esp_err_t onenet_cloud_service_request_photo_upload(void);
esp_err_t onenet_cloud_service_get_snapshot(onenet_cloud_snapshot_t *snapshot);
const char *onenet_cloud_service_status_name(onenet_cloud_status_t status);
const char *onenet_cloud_service_status_text(onenet_cloud_status_t status);

#ifdef __cplusplus
}
#endif
