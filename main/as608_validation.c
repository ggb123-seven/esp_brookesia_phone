/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "as608_validation.h"

#include <stdbool.h>
#include <stdint.h>

#include "as608_service.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define AS608_VALIDATION_TASK_NAME       "as608_valid"
#define AS608_VALIDATION_TASK_STACK_SIZE (4096)
#define AS608_VALIDATION_TASK_PRIORITY   (5)

static const char *TAG = "AS608Validation";
static TaskHandle_t s_validation_task;

static const char *as608_validation_status_name(as608_status_t status)
{
    switch (status) {
    case AS608_STATUS_OK:
        return "OK";
    case AS608_STATUS_NO_FINGERPRINT:
        return "NO_FINGERPRINT";
    case AS608_STATUS_NOT_MATCH:
        return "NOT_MATCH";
    case AS608_STATUS_NOT_FOUND:
        return "NOT_FOUND";
    case AS608_STATUS_LIB_FULL:
        return "LIB_FULL";
    case AS608_STATUS_UNKNOWN:
        return "UNKNOWN";
    default:
        return "OTHER";
    }
}

static void as608_validation_log_result(const char *step,
                                        esp_err_t err,
                                        uint16_t page_id,
                                        uint16_t score,
                                        as608_status_t status)
{
    ESP_LOGI(TAG, "%s: err=%s status=%s(0x%02x) page_id=%u score=%u",
             step,
             esp_err_to_name(err),
             as608_validation_status_name(status),
             (unsigned)status,
             (unsigned)page_id,
             (unsigned)score);
}

static void as608_validation_enroll_cb(as608_service_enroll_event_t event, void *user_ctx)
{
    (void)user_ctx;

    switch (event) {
    case AS608_SERVICE_ENROLL_PUT_FINGER:
        ESP_LOGI(TAG, "Enroll: place the finger on the AS608 sensor");
        break;
    case AS608_SERVICE_ENROLL_PUT_FINGER_AGAIN:
        ESP_LOGI(TAG, "Enroll: lift and place the same finger again");
        break;
    case AS608_SERVICE_ENROLL_FEATURE_OK:
        ESP_LOGI(TAG, "Enroll: feature captured");
        break;
    case AS608_SERVICE_ENROLL_ERROR:
    default:
        ESP_LOGW(TAG, "Enroll: sensor returned an enrollment error");
        break;
    }
}

static void as608_validation_delay_for_operator(const char *prompt)
{
    ESP_LOGI(TAG, "%s", prompt);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_EXAMPLE_AS608_VALIDATION_STEP_DELAY_MS));
}

static bool as608_validation_expect_same_page(const char *step, esp_err_t err, uint16_t expected_page, uint16_t actual_page)
{
    if (err == ESP_OK && actual_page == expected_page) {
        ESP_LOGI(TAG, "%s PASS: page_id matched enrolled template %u", step, (unsigned)expected_page);
        return true;
    }

    ESP_LOGW(TAG, "%s CHECK: expected page_id=%u actual page_id=%u err=%s",
             step, (unsigned)expected_page, (unsigned)actual_page, esp_err_to_name(err));
    return false;
}

static bool as608_validation_expect_not_found(const char *step, esp_err_t err, as608_status_t status)
{
    if (err == ESP_ERR_NOT_FOUND && (status == AS608_STATUS_NOT_FOUND || status == AS608_STATUS_NOT_MATCH)) {
        ESP_LOGI(TAG, "%s PASS: unregistered finger was not matched", step);
        return true;
    }

    ESP_LOGW(TAG, "%s CHECK: expected NOT_FOUND/NOT_MATCH, got err=%s status=%s(0x%02x)",
             step, esp_err_to_name(err), as608_validation_status_name(status), (unsigned)status);
    return false;
}

static bool as608_validation_expect_no_finger(const char *step, esp_err_t err, as608_status_t status)
{
    if (err == ESP_ERR_NOT_FOUND && status == AS608_STATUS_NO_FINGERPRINT) {
        ESP_LOGI(TAG, "%s PASS: no-finger path returned NO_FINGERPRINT", step);
        return true;
    }
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGI(TAG, "%s PASS: no-finger path returned timeout", step);
        return true;
    }

    ESP_LOGW(TAG, "%s CHECK: expected NO_FINGERPRINT or timeout, got err=%s status=%s(0x%02x)",
             step, esp_err_to_name(err), as608_validation_status_name(status), (unsigned)status);
    return false;
}

static void as608_validation_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(CONFIG_EXAMPLE_AS608_VALIDATION_START_DELAY_MS));

    ESP_LOGI(TAG, "AS608 hardware validation started");
    ESP_LOGI(TAG, "Configured wiring: AS608 TX -> ESP32-P4 GPIO%d, AS608 RX -> ESP32-P4 GPIO%d, UART%d, baud=%d, address=0x%08x",
             CONFIG_EXAMPLE_AS608_RX_GPIO,
             CONFIG_EXAMPLE_AS608_TX_GPIO,
             CONFIG_EXAMPLE_AS608_UART_NUM,
             CONFIG_EXAMPLE_AS608_BAUD_RATE,
             (unsigned)CONFIG_EXAMPLE_AS608_ADDRESS);
    ESP_LOGI(TAG, "Connect AS608 VCC/GND according to the module label and board power header before running this task");

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
        ESP_LOGE(TAG, "AS608 init FAIL: %s", esp_err_to_name(err));
        goto exit_task;
    }
    ESP_LOGI(TAG, "AS608 init PASS");

    uint16_t enrolled_page_id = 0;
    uint16_t score = 0;
    as608_status_t status = AS608_STATUS_UNKNOWN;

    ESP_LOGI(TAG, "Step 1/5: enroll one finger; follow the enrollment prompts within the driver timeout");
    err = as608_service_enroll(as608_validation_enroll_cb, NULL, &enrolled_page_id, &score, &status);
    as608_validation_log_result("Enroll result", err, enrolled_page_id, score, status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Enrollment failed; remaining validation steps are skipped");
        goto deinit_service;
    }
    ESP_LOGI(TAG, "Enroll PASS: template page_id=%u score=%u", (unsigned)enrolled_page_id, (unsigned)score);

    uint16_t matched_page_id = 0;
    uint16_t matched_score = 0;
    as608_status_t matched_status = AS608_STATUS_UNKNOWN;
    as608_validation_delay_for_operator("Step 2/5: place the enrolled finger for as608_service_match()");
    err = as608_service_match(&matched_page_id, &matched_score, &matched_status);
    as608_validation_log_result("Match enrolled finger", err, matched_page_id, matched_score, matched_status);
    bool match_passed = as608_validation_expect_same_page("Match enrolled finger", err, enrolled_page_id, matched_page_id);

    uint16_t identified_page_id = 0;
    uint16_t identified_score = 0;
    as608_status_t identified_status = AS608_STATUS_UNKNOWN;
    as608_validation_delay_for_operator("Step 3/5: place the enrolled finger for high-speed identify()");
    err = as608_service_identify(&identified_page_id, &identified_score, &identified_status);
    as608_validation_log_result("Identify enrolled finger", err, identified_page_id, identified_score, identified_status);
    bool identify_passed = as608_validation_expect_same_page("Identify enrolled finger", err, enrolled_page_id, identified_page_id);

    uint16_t unknown_page_id = 0;
    uint16_t unknown_score = 0;
    as608_status_t unknown_status = AS608_STATUS_UNKNOWN;
    as608_validation_delay_for_operator("Step 4/5: place an unregistered finger for identify()");
    err = as608_service_identify(&unknown_page_id, &unknown_score, &unknown_status);
    as608_validation_log_result("Identify unregistered finger", err, unknown_page_id, unknown_score, unknown_status);
    bool unknown_passed = as608_validation_expect_not_found("Identify unregistered finger", err, unknown_status);

    uint16_t no_finger_page_id = 0;
    uint16_t no_finger_score = 0;
    as608_status_t no_finger_status = AS608_STATUS_UNKNOWN;
    as608_validation_delay_for_operator("Step 5/5: remove all fingers from the sensor for identify()");
    err = as608_service_identify(&no_finger_page_id, &no_finger_score, &no_finger_status);
    as608_validation_log_result("Identify no finger", err, no_finger_page_id, no_finger_score, no_finger_status);
    bool no_finger_passed = as608_validation_expect_no_finger("Identify no finger", err, no_finger_status);

    ESP_LOGI(TAG, "AS608 validation summary: match=%s identify=%s unknown=%s no_finger=%s",
             match_passed ? "PASS" : "CHECK",
             identify_passed ? "PASS" : "CHECK",
             unknown_passed ? "PASS" : "CHECK",
             no_finger_passed ? "PASS" : "CHECK");

#if CONFIG_EXAMPLE_AS608_VALIDATION_DELETE_ENROLLED
    as608_status_t delete_status = AS608_STATUS_UNKNOWN;
    err = as608_service_delete_template(enrolled_page_id, &delete_status);
    ESP_LOGI(TAG, "Delete enrolled template: err=%s status=%s(0x%02x) page_id=%u",
             esp_err_to_name(err),
             as608_validation_status_name(delete_status),
             (unsigned)delete_status,
             (unsigned)enrolled_page_id);
#endif

deinit_service:
    err = as608_service_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AS608 deinit failed: %s", esp_err_to_name(err));
    }

exit_task:
    ESP_LOGI(TAG, "AS608 hardware validation task finished");
    s_validation_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t as608_validation_start(void)
{
    ESP_RETURN_ON_FALSE(s_validation_task == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "AS608 validation task already running");

    BaseType_t created = xTaskCreate(as608_validation_task,
                                     AS608_VALIDATION_TASK_NAME,
                                     AS608_VALIDATION_TASK_STACK_SIZE,
                                     NULL,
                                     AS608_VALIDATION_TASK_PRIORITY,
                                     &s_validation_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "Failed to create AS608 validation task");
    return ESP_OK;
}
