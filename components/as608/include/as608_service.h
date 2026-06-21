/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "as608_port.h"
#include "driver_as608.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AS608_SERVICE_DEFAULT_ADDRESS (0xFFFFFFFFU)

typedef enum {
    AS608_SERVICE_ENROLL_ERROR = -1,
    AS608_SERVICE_ENROLL_PUT_FINGER = 0,
    AS608_SERVICE_ENROLL_PUT_FINGER_AGAIN = 1,
    AS608_SERVICE_ENROLL_FEATURE_OK = 2,
} as608_service_enroll_event_t;

typedef void (*as608_service_enroll_cb_t)(as608_service_enroll_event_t event, void *user_ctx);

typedef struct {
    as608_port_config_t port;
    uint32_t address;
} as608_service_config_t;

esp_err_t as608_service_init(const as608_service_config_t *config);
esp_err_t as608_service_deinit(void);

esp_err_t as608_service_enroll(as608_service_enroll_cb_t callback,
                               void *user_ctx,
                               uint16_t *page_id,
                               uint16_t *score,
                               as608_status_t *status);

esp_err_t as608_service_enroll_to_page(as608_service_enroll_cb_t callback,
                                       void *user_ctx,
                                       uint16_t target_page_id,
                                       uint16_t *page_id,
                                       uint16_t *score,
                                       as608_status_t *status);

esp_err_t as608_service_wait_finger(as608_status_t *status);
esp_err_t as608_service_identify(uint16_t *page_id, uint16_t *score, as608_status_t *status);
esp_err_t as608_service_match(uint16_t *page_id, uint16_t *score, as608_status_t *status);
esp_err_t as608_service_delete_template(uint16_t page_id, as608_status_t *status);

#ifdef __cplusplus
}
#endif
