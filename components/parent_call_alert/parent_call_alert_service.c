/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "parent_call_alert_service.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PARENT_CALL_ALERT_TASK_NAME           "parent_call"
#define PARENT_CALL_ALERT_STOP_WAIT_MS        (2000U)
#define PARENT_CALL_ALERT_URL_MAX_LEN         (256U)
#define PARENT_CALL_ALERT_RESPONSE_MAX_LEN    (256U)
#define PARENT_CALL_ALERT_AT_RX_BUFFER_SIZE   (512U)
#define PARENT_CALL_ALERT_AT_TX_WAIT_MS       (1000U)
#define PARENT_CALL_ALERT_AT_COMMAND_MAX_LEN  (64U)
#define PARENT_CALL_ALERT_AT_RESPONSE_MAX_LEN (256U)

static const char *TAG = "ParentCallAlert";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} alert_http_response_t;

static parent_call_alert_service_config_t s_config;
static parent_call_alert_snapshot_t s_snapshot;
static TaskHandle_t s_task_handle;
static SemaphoreHandle_t s_done_sem;
static QueueHandle_t s_queue;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_stop_requested;
static bool s_initialized;
static bool s_air780e_uart_installed;

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

static bool has_network_ip(void)
{
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info = {};
    esp_err_t err = esp_netif_get_ip_info(sta_netif, &ip_info);
    return err == ESP_OK && ip_info.ip.addr != 0;
}

static const char *transport_name(parent_call_alert_transport_t transport)
{
    switch (transport) {
    case PARENT_CALL_ALERT_TRANSPORT_MOCK:
        return "mock";
    case PARENT_CALL_ALERT_TRANSPORT_HTTP:
        return "http";
    case PARENT_CALL_ALERT_TRANSPORT_AIR780E_AT:
        return "air780e_at";
    default:
        return "unknown";
    }
}

static void update_snapshot(parent_call_alert_status_t status,
                            esp_err_t err,
                            int http_status,
                            const parent_call_alert_event_t *event)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.status = status;
    s_snapshot.last_error = err;
    s_snapshot.last_http_status = http_status;
    s_snapshot.last_attempt_ms = esp_timer_get_time() / 1000;
    if (event != NULL) {
        copy_string(s_snapshot.last_reason, sizeof(s_snapshot.last_reason), event->reason);
        copy_string(s_snapshot.last_detail, sizeof(s_snapshot.last_detail), event->detail);
    }

    switch (status) {
    case PARENT_CALL_ALERT_STATUS_QUEUED:
        s_snapshot.queued_count++;
        break;
    case PARENT_CALL_ALERT_STATUS_SENT:
    case PARENT_CALL_ALERT_STATUS_SIMULATED:
        s_snapshot.sent_count++;
        s_snapshot.last_sent_ms = s_snapshot.last_attempt_ms;
        break;
    case PARENT_CALL_ALERT_STATUS_SKIPPED_COOLDOWN:
    case PARENT_CALL_ALERT_STATUS_SKIPPED_DISABLED:
    case PARENT_CALL_ALERT_STATUS_SKIPPED_NO_IP:
        s_snapshot.skipped_count++;
        break;
    case PARENT_CALL_ALERT_STATUS_FAILED:
        s_snapshot.failed_count++;
        break;
    default:
        break;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

static esp_err_t alert_http_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL) {
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    alert_http_response_t *buffer = (alert_http_response_t *)event->user_data;
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

static esp_err_t build_alert_json(const parent_call_alert_event_t *event,
                                  char *buffer,
                                  size_t buffer_size)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create alert JSON");

    bool ok = cJSON_AddStringToObject(root, "reason", event->reason) != NULL &&
              cJSON_AddStringToObject(root, "detail", event->detail) != NULL &&
              cJSON_AddStringToObject(root, "message", event->message) != NULL &&
              cJSON_AddNumberToObject(root, "timestamp_ms", (double)event->timestamp_ms) != NULL;

    if (!ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    const bool printed = cJSON_PrintPreallocated(root, buffer, (int)buffer_size, false);
    cJSON_Delete(root);
    return printed ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t send_alert_http(const parent_call_alert_event_t *event, int *http_status)
{
    char url[PARENT_CALL_ALERT_URL_MAX_LEN];
    char payload[256];
    char response_buffer[PARENT_CALL_ALERT_RESPONSE_MAX_LEN] = {0};

    esp_err_t err = build_alert_json(event, payload, sizeof(payload));
    if (err != ESP_OK) {
        return err;
    }

    snprintf(url, sizeof(url), "http://%s:%u%s",
             s_config.server_host, (unsigned)s_config.server_port, s_config.api_path);

    alert_http_response_t response = {
        .data = response_buffer,
        .len = 0,
        .cap = sizeof(response_buffer),
        .overflow = false,
    };

    esp_http_client_config_t http_config = {};
    http_config.url = url;
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = (int)s_config.http_timeout_ms;
    http_config.event_handler = alert_http_event_handler;
    http_config.user_data = &response;
    http_config.buffer_size = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (s_config.token != NULL && s_config.token[0] != '\0') {
        esp_http_client_set_header(client, "X-Alert-Token", s_config.token);
    }
    esp_http_client_set_post_field(client, payload, strlen(payload));

    err = esp_http_client_perform(client);
    *http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (response.overflow) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (*http_status < 200 || *http_status >= 300) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static bool air780e_phone_number_is_safe(const char *phone_number)
{
    if (phone_number == NULL || phone_number[0] == '\0') {
        return false;
    }

    const size_t len = strlen(phone_number);
    if (len >= PARENT_CALL_ALERT_SERVICE_MAX_PHONE_NUMBER_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        const unsigned char ch = (unsigned char)phone_number[i];
        if (isdigit(ch)) {
            continue;
        }
        if (ch == '+' && i == 0) {
            continue;
        }
        return false;
    }
    return true;
}

static void air780e_uart_deinit(void)
{
    if (!s_air780e_uart_installed) {
        return;
    }

    uart_driver_delete(s_config.air780e_uart_num);
    s_air780e_uart_installed = false;
}

static esp_err_t air780e_uart_init(void)
{
    if (s_air780e_uart_installed) {
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate = s_config.air780e_baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(s_config.air780e_uart_num,
                                        PARENT_CALL_ALERT_AT_RX_BUFFER_SIZE,
                                        0,
                                        0,
                                        NULL,
                                        0);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_param_config(s_config.air780e_uart_num, &uart_config);
    if (err != ESP_OK) {
        uart_driver_delete(s_config.air780e_uart_num);
        return err;
    }

    err = uart_set_pin(s_config.air780e_uart_num,
                       s_config.air780e_tx_gpio,
                       s_config.air780e_rx_gpio,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_driver_delete(s_config.air780e_uart_num);
        return err;
    }

    uart_flush_input(s_config.air780e_uart_num);
    s_air780e_uart_installed = true;
    ESP_LOGI(TAG, "Air780E UART initialized: uart=%d tx=%d rx=%d baud=%d",
             (int)s_config.air780e_uart_num,
             s_config.air780e_tx_gpio,
             s_config.air780e_rx_gpio,
             s_config.air780e_baud_rate);
    return ESP_OK;
}

static esp_err_t air780e_at_read_until(const char *expect,
                                       const char *reject,
                                       uint32_t timeout_ms,
                                       char *response,
                                       size_t response_size)
{
    ESP_RETURN_ON_FALSE(expect != NULL && expect[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                        "AT expected response is required");
    ESP_RETURN_ON_FALSE(response != NULL && response_size > 1, ESP_ERR_INVALID_ARG, TAG,
                        "AT response buffer is invalid");

    response[0] = '\0';
    size_t response_len = 0;
    const int64_t deadline_ms = (esp_timer_get_time() / 1000) + timeout_ms;

    while ((esp_timer_get_time() / 1000) < deadline_ms) {
        uint8_t byte = 0;
        const int read_len = uart_read_bytes(s_config.air780e_uart_num, &byte, 1, pdMS_TO_TICKS(100));
        if (read_len <= 0) {
            continue;
        }

        if (response_len + 1 >= response_size) {
            response[response_size - 1] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }

        response[response_len++] = (char)byte;
        response[response_len] = '\0';

        if ((reject != NULL && reject[0] != '\0' && strstr(response, reject) != NULL) ||
            strstr(response, "ERROR") != NULL) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (strstr(response, expect) != NULL) {
            return ESP_OK;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t air780e_at_command(const char *command,
                                    const char *expect,
                                    uint32_t timeout_ms,
                                    char *response,
                                    size_t response_size)
{
    ESP_RETURN_ON_FALSE(command != NULL && command[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                        "AT command is required");

    ESP_RETURN_ON_ERROR(air780e_uart_init(), TAG, "Failed to initialize Air780E UART");
    uart_flush_input(s_config.air780e_uart_num);

    const int command_len = (int)strlen(command);
    const int written = uart_write_bytes(s_config.air780e_uart_num, command, command_len);
    if (written != command_len) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(uart_wait_tx_done(s_config.air780e_uart_num,
                                          pdMS_TO_TICKS(PARENT_CALL_ALERT_AT_TX_WAIT_MS)),
                        TAG, "Timed out waiting for Air780E AT command TX");

    return air780e_at_read_until(expect, NULL, timeout_ms, response, response_size);
}

static esp_err_t send_alert_air780e_at(const parent_call_alert_event_t *event)
{
    char response[PARENT_CALL_ALERT_AT_RESPONSE_MAX_LEN];
    const uint32_t timeout_ms = s_config.air780e_command_timeout_ms;

    ESP_RETURN_ON_ERROR(air780e_at_command("AT\r\n", "OK", timeout_ms, response, sizeof(response)),
                        TAG, "Air780E did not respond to AT");

    esp_err_t echo_err = air780e_at_command("ATE0\r\n", "OK", timeout_ms, response, sizeof(response));
    if (echo_err != ESP_OK) {
        ESP_LOGW(TAG, "Air780E echo-off command failed, continuing: err=%s", esp_err_to_name(echo_err));
    }

    ESP_RETURN_ON_ERROR(air780e_at_command("AT+CPIN?\r\n", "+CPIN: READY", timeout_ms,
                                           response, sizeof(response)),
                        TAG, "Air780E SIM is not ready");

    char dial_command[PARENT_CALL_ALERT_AT_COMMAND_MAX_LEN];
    const int written = snprintf(dial_command, sizeof(dial_command), "ATD%s;\r\n",
                                 s_config.air780e_phone_number);
    ESP_RETURN_ON_FALSE(written > 0 && written < (int)sizeof(dial_command),
                        ESP_ERR_INVALID_SIZE, TAG, "Air780E dial command is too long");

    ESP_RETURN_ON_ERROR(air780e_at_command(dial_command, "OK", timeout_ms, response, sizeof(response)),
                        TAG, "Air780E dial command failed");

    ESP_LOGI(TAG, "Air780E voice call command accepted: reason=%s", event->reason);
    if (s_config.air780e_call_hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_config.air780e_call_hold_ms));
    }

    esp_err_t hangup_err = air780e_at_command("ATH\r\n", "OK", timeout_ms, response, sizeof(response));
    if (hangup_err != ESP_OK) {
        ESP_LOGW(TAG, "Air780E hangup command failed after dial: err=%s", esp_err_to_name(hangup_err));
    }
    return ESP_OK;
}

static void simulate_alert_send(const parent_call_alert_event_t *event)
{
    update_snapshot(PARENT_CALL_ALERT_STATUS_SIMULATED, ESP_OK, 0, event);
    ESP_LOGW(TAG, "Parent call alert simulated only: reason=%s; real phone module is not connected",
             event->reason);
}

static bool should_skip_for_cooldown(int64_t now_ms)
{
    if (s_config.cooldown_ms == 0) {
        return false;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    const int64_t last_sent_ms = s_snapshot.last_sent_ms;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    return last_sent_ms > 0 && (uint32_t)(now_ms - last_sent_ms) < s_config.cooldown_ms;
}

static void alert_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Parent call alert task started: transport=%s cooldown=%" PRIu32 "ms",
             transport_name(s_config.transport), s_config.cooldown_ms);

    while (!s_stop_requested) {
        parent_call_alert_event_t event = {};
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (!s_config.enabled) {
            update_snapshot(PARENT_CALL_ALERT_STATUS_SKIPPED_DISABLED, ESP_ERR_INVALID_STATE, 0, &event);
            ESP_LOGW(TAG, "Skip parent call alert because service is disabled");
            continue;
        }
        if (should_skip_for_cooldown(now_ms)) {
            update_snapshot(PARENT_CALL_ALERT_STATUS_SKIPPED_COOLDOWN, ESP_ERR_INVALID_STATE, 0, &event);
            ESP_LOGW(TAG, "Skip parent call alert because cooldown is active: reason=%s", event.reason);
            continue;
        }
        if (s_config.transport == PARENT_CALL_ALERT_TRANSPORT_MOCK) {
            simulate_alert_send(&event);
            continue;
        }
        if (s_config.transport == PARENT_CALL_ALERT_TRANSPORT_HTTP && !has_network_ip()) {
            update_snapshot(PARENT_CALL_ALERT_STATUS_SKIPPED_NO_IP, ESP_ERR_INVALID_STATE, 0, &event);
            ESP_LOGW(TAG, "Skip parent call alert because Wi-Fi has no IP address yet");
            continue;
        }

        update_snapshot(PARENT_CALL_ALERT_STATUS_SENDING, ESP_OK, 0, &event);
        int http_status = 0;
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (s_config.transport == PARENT_CALL_ALERT_TRANSPORT_HTTP) {
            err = send_alert_http(&event, &http_status);
        } else if (s_config.transport == PARENT_CALL_ALERT_TRANSPORT_AIR780E_AT) {
            err = send_alert_air780e_at(&event);
        }

        if (err == ESP_OK) {
            update_snapshot(PARENT_CALL_ALERT_STATUS_SENT, ESP_OK, http_status, &event);
            ESP_LOGI(TAG, "Parent call alert sent: reason=%s transport=%s http=%d",
                     event.reason, transport_name(s_config.transport), http_status);
        } else {
            update_snapshot(PARENT_CALL_ALERT_STATUS_FAILED, err, http_status, &event);
            ESP_LOGE(TAG, "Parent call alert failed: reason=%s transport=%s err=%s http=%d",
                     event.reason, transport_name(s_config.transport), esp_err_to_name(err), http_status);
        }
    }

    ESP_LOGI(TAG, "Parent call alert task stopped");
    if (s_done_sem != NULL) {
        xSemaphoreGive(s_done_sem);
    }
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t validate_config(const parent_call_alert_service_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid alert config");
    ESP_RETURN_ON_FALSE(config->task_stack_size >= 4096, ESP_ERR_INVALID_ARG, TAG,
                        "Alert task stack size is too small");
    ESP_RETURN_ON_FALSE(config->task_priority > 0, ESP_ERR_INVALID_ARG, TAG,
                        "Alert task priority must be greater than zero");
    ESP_RETURN_ON_FALSE(config->queue_length > 0, ESP_ERR_INVALID_ARG, TAG,
                        "Alert queue length must be greater than zero");
    ESP_RETURN_ON_FALSE((config->transport == PARENT_CALL_ALERT_TRANSPORT_MOCK) ||
                        (config->transport == PARENT_CALL_ALERT_TRANSPORT_HTTP) ||
                        (config->transport == PARENT_CALL_ALERT_TRANSPORT_AIR780E_AT),
                        ESP_ERR_INVALID_ARG, TAG, "Invalid alert transport");
    if (config->transport == PARENT_CALL_ALERT_TRANSPORT_HTTP) {
        ESP_RETURN_ON_FALSE(config->server_host != NULL && config->server_host[0] != '\0',
                            ESP_ERR_INVALID_ARG, TAG, "Alert server host is required");
        ESP_RETURN_ON_FALSE(config->server_port > 0, ESP_ERR_INVALID_ARG, TAG,
                            "Alert server port is required");
        ESP_RETURN_ON_FALSE(config->api_path != NULL && config->api_path[0] == '/',
                            ESP_ERR_INVALID_ARG, TAG, "Alert API path must start with /");
    }
    if (config->transport == PARENT_CALL_ALERT_TRANSPORT_AIR780E_AT) {
        ESP_RETURN_ON_FALSE(air780e_phone_number_is_safe(config->air780e_phone_number),
                            ESP_ERR_INVALID_ARG, TAG, "Air780E phone number is invalid");
        ESP_RETURN_ON_FALSE(config->air780e_uart_num > UART_NUM_0 &&
                            config->air780e_uart_num < UART_NUM_MAX,
                            ESP_ERR_INVALID_ARG, TAG, "Air780E UART port is invalid");
        ESP_RETURN_ON_FALSE(config->air780e_tx_gpio >= 0 && config->air780e_rx_gpio >= 0,
                            ESP_ERR_INVALID_ARG, TAG, "Air780E TX/RX GPIO must be configured");
        ESP_RETURN_ON_FALSE(config->air780e_baud_rate > 0, ESP_ERR_INVALID_ARG, TAG,
                            "Air780E baud rate is invalid");
        ESP_RETURN_ON_FALSE(config->air780e_command_timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG,
                            "Air780E command timeout is invalid");
    }
    return ESP_OK;
}

esp_err_t parent_call_alert_service_init(const parent_call_alert_service_config_t *config)
{
    ESP_RETURN_ON_ERROR(validate_config(config), TAG, "Invalid parent call alert configuration");

    if (s_initialized) {
        return ESP_OK;
    }

    s_config = *config;
    s_stop_requested = false;
    s_snapshot = (parent_call_alert_snapshot_t) {
        .initialized = true,
        .enabled = config->enabled,
        .status = config->enabled ? PARENT_CALL_ALERT_STATUS_IDLE : PARENT_CALL_ALERT_STATUS_SKIPPED_DISABLED,
        .last_error = ESP_OK,
    };

    s_queue = xQueueCreate(s_config.queue_length, sizeof(parent_call_alert_event_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create alert queue");

    s_done_sem = xSemaphoreCreateBinary();
    if (s_done_sem == NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(alert_task,
                                 PARENT_CALL_ALERT_TASK_NAME,
                                 s_config.task_stack_size,
                                 NULL,
                                 s_config.task_priority,
                                 &s_task_handle);
    if (ret != pdPASS) {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Parent call alert service initialized: enabled=%d transport=%s",
             s_config.enabled, transport_name(s_config.transport));
    if (s_config.transport == PARENT_CALL_ALERT_TRANSPORT_HTTP) {
        ESP_LOGI(TAG, "Parent call alert HTTP endpoint configured: host=%s port=%u path=%s",
                 s_config.server_host, (unsigned)s_config.server_port, s_config.api_path);
    }
    return ESP_OK;
}

esp_err_t parent_call_alert_service_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_stop_requested = true;
    if ((s_done_sem != NULL) && (s_task_handle != NULL)) {
        if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(PARENT_CALL_ALERT_STOP_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Timed out waiting for parent call alert task to stop");
            vTaskDelete(s_task_handle);
            s_task_handle = NULL;
        }
    }

    if (s_done_sem != NULL) {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    air780e_uart_deinit();

    s_initialized = false;
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.initialized = false;
    s_snapshot.status = PARENT_CALL_ALERT_STATUS_IDLE;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return ESP_OK;
}

esp_err_t parent_call_alert_service_trigger(const parent_call_alert_event_t *event)
{
    ESP_RETURN_ON_FALSE(event != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid alert event");
    ESP_RETURN_ON_FALSE(s_initialized && s_queue != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Parent call alert service is not initialized");
    ESP_RETURN_ON_FALSE(event->reason[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "Alert reason is required");

    parent_call_alert_event_t queued = *event;
    if (queued.timestamp_ms <= 0) {
        queued.timestamp_ms = esp_timer_get_time() / 1000;
    }

    if (xQueueSend(s_queue, &queued, 0) != pdTRUE) {
        update_snapshot(PARENT_CALL_ALERT_STATUS_FAILED, ESP_ERR_NO_MEM, 0, &queued);
        return ESP_ERR_NO_MEM;
    }

    update_snapshot(PARENT_CALL_ALERT_STATUS_QUEUED, ESP_OK, 0, &queued);
    return ESP_OK;
}

esp_err_t parent_call_alert_service_air780e_self_test(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Parent call alert service is not initialized");
    ESP_RETURN_ON_FALSE(s_config.transport == PARENT_CALL_ALERT_TRANSPORT_AIR780E_AT,
                        ESP_ERR_INVALID_STATE, TAG, "Parent call alert transport is not Air780E AT");

    char response[PARENT_CALL_ALERT_AT_RESPONSE_MAX_LEN];
    esp_err_t err = air780e_at_command("AT\r\n", "OK", s_config.air780e_command_timeout_ms,
                                       response, sizeof(response));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Air780E AT self-test passed");
    } else {
        ESP_LOGE(TAG, "Air780E AT self-test failed: err=%s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t parent_call_alert_service_get_snapshot(parent_call_alert_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid alert snapshot output");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Parent call alert service is not initialized");

    taskENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return ESP_OK;
}

const char *parent_call_alert_service_status_name(parent_call_alert_status_t status)
{
    switch (status) {
    case PARENT_CALL_ALERT_STATUS_IDLE:
        return "IDLE";
    case PARENT_CALL_ALERT_STATUS_QUEUED:
        return "QUEUED";
    case PARENT_CALL_ALERT_STATUS_SENDING:
        return "SENDING";
    case PARENT_CALL_ALERT_STATUS_SENT:
        return "SENT";
    case PARENT_CALL_ALERT_STATUS_SIMULATED:
        return "SIMULATED";
    case PARENT_CALL_ALERT_STATUS_SKIPPED_COOLDOWN:
        return "SKIPPED_COOLDOWN";
    case PARENT_CALL_ALERT_STATUS_SKIPPED_DISABLED:
        return "SKIPPED_DISABLED";
    case PARENT_CALL_ALERT_STATUS_SKIPPED_NO_IP:
        return "SKIPPED_NO_IP";
    case PARENT_CALL_ALERT_STATUS_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *parent_call_alert_service_status_text(parent_call_alert_status_t status)
{
    switch (status) {
    case PARENT_CALL_ALERT_STATUS_IDLE:
        return "待命";
    case PARENT_CALL_ALERT_STATUS_QUEUED:
        return "已排队";
    case PARENT_CALL_ALERT_STATUS_SENDING:
        return "发送中";
    case PARENT_CALL_ALERT_STATUS_SENT:
        return "已发送";
    case PARENT_CALL_ALERT_STATUS_SIMULATED:
        return "模拟";
    case PARENT_CALL_ALERT_STATUS_SKIPPED_COOLDOWN:
        return "冷却中";
    case PARENT_CALL_ALERT_STATUS_SKIPPED_DISABLED:
        return "未启用";
    case PARENT_CALL_ALERT_STATUS_SKIPPED_NO_IP:
        return "无网络";
    case PARENT_CALL_ALERT_STATUS_FAILED:
        return "失败";
    default:
        return "--";
    }
}
