/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PARENT_CALL_ALERT_SERVICE_DEFAULT_QUEUE_LENGTH        (4U)
#define PARENT_CALL_ALERT_SERVICE_DEFAULT_TASK_STACK_SIZE     (6144U)
#define PARENT_CALL_ALERT_SERVICE_DEFAULT_TASK_PRIORITY       (5U)
#define PARENT_CALL_ALERT_SERVICE_DEFAULT_COOLDOWN_MS         (300000U)
#define PARENT_CALL_ALERT_SERVICE_DEFAULT_HTTP_TIMEOUT_MS     (8000U)
#define PARENT_CALL_ALERT_SERVICE_DEFAULT_AIR780E_BAUD_RATE   (115200)
#define PARENT_CALL_ALERT_SERVICE_DEFAULT_AIR780E_TIMEOUT_MS  (8000U)
#define PARENT_CALL_ALERT_SERVICE_DEFAULT_AIR780E_HOLD_MS     (15000U)
#define PARENT_CALL_ALERT_SERVICE_MAX_REASON_LEN              (48U)
#define PARENT_CALL_ALERT_SERVICE_MAX_DETAIL_LEN              (96U)
#define PARENT_CALL_ALERT_SERVICE_MAX_MESSAGE_LEN             (160U)
#define PARENT_CALL_ALERT_SERVICE_MAX_PHONE_NUMBER_LEN        (32U)

typedef enum {
    PARENT_CALL_ALERT_STATUS_IDLE = 0,
    PARENT_CALL_ALERT_STATUS_QUEUED,
    PARENT_CALL_ALERT_STATUS_SENDING,
    PARENT_CALL_ALERT_STATUS_SENT,
    PARENT_CALL_ALERT_STATUS_SIMULATED,
    PARENT_CALL_ALERT_STATUS_SKIPPED_COOLDOWN,
    PARENT_CALL_ALERT_STATUS_SKIPPED_DISABLED,
    PARENT_CALL_ALERT_STATUS_SKIPPED_NO_IP,
    PARENT_CALL_ALERT_STATUS_FAILED,
} parent_call_alert_status_t;

typedef enum {
    PARENT_CALL_ALERT_TRANSPORT_MOCK = 0,
    PARENT_CALL_ALERT_TRANSPORT_HTTP,
    PARENT_CALL_ALERT_TRANSPORT_AIR780E_AT,
} parent_call_alert_transport_t;

typedef struct {
    const char *server_host;
    uint16_t server_port;
    const char *api_path;
    const char *token;
    uint32_t cooldown_ms;
    uint32_t http_timeout_ms;
    const char *air780e_phone_number;
    uart_port_t air780e_uart_num;
    int air780e_tx_gpio;
    int air780e_rx_gpio;
    int air780e_baud_rate;
    uint32_t air780e_command_timeout_ms;
    uint32_t air780e_call_hold_ms;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    uint32_t queue_length;
    bool enabled;
    parent_call_alert_transport_t transport;
} parent_call_alert_service_config_t;

typedef struct {
    char reason[PARENT_CALL_ALERT_SERVICE_MAX_REASON_LEN];
    char detail[PARENT_CALL_ALERT_SERVICE_MAX_DETAIL_LEN];
    char message[PARENT_CALL_ALERT_SERVICE_MAX_MESSAGE_LEN];
    int64_t timestamp_ms;
} parent_call_alert_event_t;

typedef struct {
    bool initialized;
    bool enabled;
    parent_call_alert_status_t status;
    esp_err_t last_error;
    int last_http_status;
    char last_reason[PARENT_CALL_ALERT_SERVICE_MAX_REASON_LEN];
    char last_detail[PARENT_CALL_ALERT_SERVICE_MAX_DETAIL_LEN];
    int64_t last_attempt_ms;
    int64_t last_sent_ms;
    uint32_t queued_count;
    uint32_t sent_count;
    uint32_t skipped_count;
    uint32_t failed_count;
} parent_call_alert_snapshot_t;

esp_err_t parent_call_alert_service_init(const parent_call_alert_service_config_t *config);
esp_err_t parent_call_alert_service_deinit(void);
esp_err_t parent_call_alert_service_trigger(const parent_call_alert_event_t *event);
esp_err_t parent_call_alert_service_air780e_self_test(void);
esp_err_t parent_call_alert_service_get_snapshot(parent_call_alert_snapshot_t *snapshot);
const char *parent_call_alert_service_status_name(parent_call_alert_status_t status);
const char *parent_call_alert_service_status_text(parent_call_alert_status_t status);

#ifdef __cplusplus
}
#endif
