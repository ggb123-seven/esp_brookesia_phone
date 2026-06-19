/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "as608_service.h"

#include <stdbool.h>

#include "driver_as608_basic.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "AS608Service";

static bool s_initialized;
static as608_service_enroll_cb_t s_enroll_callback;
static void *s_enroll_user_ctx;

static esp_err_t as608_service_status_to_err(as608_status_t status)
{
    switch (status) {
    case AS608_STATUS_OK:
        return ESP_OK;
    case AS608_STATUS_NO_FINGERPRINT:
    case AS608_STATUS_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case AS608_STATUS_LIB_FULL:
        return ESP_ERR_NO_MEM;
    default:
        return ESP_FAIL;
    }
}

static void as608_service_enroll_bridge(int8_t status, const char *const fmt, ...)
{
    (void)fmt;

    if (s_enroll_callback == NULL) {
        return;
    }

    as608_service_enroll_event_t event = AS608_SERVICE_ENROLL_ERROR;
    if (status == 0) {
        event = AS608_SERVICE_ENROLL_PUT_FINGER;
    } else if (status == 1) {
        event = AS608_SERVICE_ENROLL_PUT_FINGER_AGAIN;
    } else if (status == 2) {
        event = AS608_SERVICE_ENROLL_FEATURE_OK;
    }

    s_enroll_callback(event, s_enroll_user_ctx);
}

esp_err_t as608_service_init(const as608_service_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid AS608 service config");

    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = as608_port_configure(&config->port);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to configure AS608 UART");

    uint32_t address = config->address;
    if (address == 0) {
        address = AS608_SERVICE_DEFAULT_ADDRESS;
    }

    if (as608_basic_init(address) != 0) {
        ESP_LOGE(TAG, "Failed to initialize AS608 module");
        return ESP_FAIL;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t as608_service_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (as608_basic_deinit() != 0) {
        ESP_LOGE(TAG, "Failed to deinitialize AS608 module");
        return ESP_FAIL;
    }

    s_initialized = false;
    return ESP_OK;
}

esp_err_t as608_service_enroll(as608_service_enroll_cb_t callback,
                               void *user_ctx,
                               uint16_t *page_id,
                               uint16_t *score,
                               as608_status_t *status)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "AS608 service is not initialized");

    uint16_t local_page_id = 0;
    uint16_t local_score = 0;
    as608_status_t local_status = AS608_STATUS_UNKNOWN;

    s_enroll_callback = callback;
    s_enroll_user_ctx = user_ctx;

    uint8_t res = as608_basic_input_fingerprint(as608_service_enroll_bridge,
                                                &local_score,
                                                &local_page_id,
                                                &local_status);

    s_enroll_callback = NULL;
    s_enroll_user_ctx = NULL;

    if (page_id != NULL) {
        *page_id = local_page_id;
    }
    if (score != NULL) {
        *score = local_score;
    }
    if (status != NULL) {
        *status = local_status;
    }

    if (res == 2) {
        return ESP_ERR_TIMEOUT;
    }
    if (res != 0) {
        return as608_service_status_to_err(local_status);
    }

    return as608_service_status_to_err(local_status);
}

esp_err_t as608_service_identify(uint16_t *page_id, uint16_t *score, as608_status_t *status)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "AS608 service is not initialized");

    uint16_t local_page_id = 0;
    uint16_t local_score = 0;
    as608_status_t local_status = AS608_STATUS_UNKNOWN;

    uint8_t res = as608_basic_high_speed_verify(&local_page_id, &local_score, &local_status);
    if (page_id != NULL) {
        *page_id = local_page_id;
    }
    if (score != NULL) {
        *score = local_score;
    }
    if (status != NULL) {
        *status = local_status;
    }

    if (res != 0) {
        return as608_service_status_to_err(local_status);
    }

    return as608_service_status_to_err(local_status);
}

esp_err_t as608_service_match(uint16_t *page_id, uint16_t *score, as608_status_t *status)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "AS608 service is not initialized");

    uint16_t local_page_id = 0;
    uint16_t local_score = 0;
    as608_status_t local_status = AS608_STATUS_UNKNOWN;

    uint8_t res = as608_basic_verify(&local_page_id, &local_score, &local_status);
    if (page_id != NULL) {
        *page_id = local_page_id;
    }
    if (score != NULL) {
        *score = local_score;
    }
    if (status != NULL) {
        *status = local_status;
    }

    if (res != 0) {
        return as608_service_status_to_err(local_status);
    }

    return as608_service_status_to_err(local_status);
}
