/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "dht11_service.h"

#include <inttypes.h>

#include "dht.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/gpio_num.h"

#define DHT11_TASK_NAME           "dht11"
#define DHT11_STOP_WAIT_MS        (2500U)
#define DHT11_STOP_POLL_MS        (100U)

static const char *TAG = "DHT11Service";

static dht11_service_config_t s_config;
static dht11_service_snapshot_t s_snapshot;
static TaskHandle_t s_task_handle;
static SemaphoreHandle_t s_done_sem;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_stop_requested;
static bool s_initialized;

static const char *dht11_status_to_name(dht11_service_status_t status)
{
    switch (status) {
    case DHT11_SERVICE_STATUS_NEVER_READ:
        return "NEVER_READ";
    case DHT11_SERVICE_STATUS_OK:
        return "OK";
    case DHT11_SERVICE_STATUS_TIMEOUT:
        return "TIMEOUT";
    case DHT11_SERVICE_STATUS_CHECKSUM_ERROR:
        return "CHECKSUM_ERROR";
    case DHT11_SERVICE_STATUS_IO_ERROR:
        return "IO_ERROR";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t dht11_validate_config(const dht11_service_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid DHT11 service config");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(config->data_gpio), ESP_ERR_INVALID_ARG, TAG,
                        "Invalid DHT11 data GPIO");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(config->data_gpio), ESP_ERR_INVALID_ARG, TAG,
                        "DHT11 data GPIO must support output");
    ESP_RETURN_ON_FALSE(config->sample_period_ms >= DHT11_SERVICE_DEFAULT_SAMPLE_PERIOD_MS,
                        ESP_ERR_INVALID_ARG, TAG, "DHT11 sample period must be at least 2000 ms");
    ESP_RETURN_ON_FALSE(config->max_consecutive_failures > 0, ESP_ERR_INVALID_ARG, TAG,
                        "DHT11 max consecutive failures must be greater than zero");
    ESP_RETURN_ON_FALSE(config->task_stack_size >= 2048, ESP_ERR_INVALID_ARG, TAG,
                        "DHT11 task stack size is too small");
    ESP_RETURN_ON_FALSE(config->task_priority > 0, ESP_ERR_INVALID_ARG, TAG,
                        "DHT11 task priority must be greater than zero");
    return ESP_OK;
}

static dht11_service_status_t dht11_err_to_status(esp_err_t err)
{
    if (err == ESP_OK) {
        return DHT11_SERVICE_STATUS_OK;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return DHT11_SERVICE_STATUS_TIMEOUT;
    }
    if (err == ESP_ERR_INVALID_CRC) {
        return DHT11_SERVICE_STATUS_CHECKSUM_ERROR;
    }
    return DHT11_SERVICE_STATUS_IO_ERROR;
}

static esp_err_t dht11_read_once(float *temperature_c, float *humidity_percent, dht11_service_status_t *status)
{
    esp_err_t err = dht_read_float_data(DHT_TYPE_DHT11, s_config.data_gpio,
                                        humidity_percent, temperature_c);
    *status = dht11_err_to_status(err);

    return err;
}

static void dht11_update_snapshot(esp_err_t err,
                                  dht11_service_status_t status,
                                  float temperature_c,
                                  float humidity_percent)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.sample_seq++;
    s_snapshot.status = status;
    s_snapshot.last_error = err;
    s_snapshot.timestamp_ms = esp_timer_get_time() / 1000;
    if (err == ESP_OK) {
        s_snapshot.valid = true;
        s_snapshot.temperature_c = temperature_c;
        s_snapshot.humidity_percent = humidity_percent;
        s_snapshot.consecutive_failures = 0;
    } else {
        s_snapshot.consecutive_failures++;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

static void dht11_sample_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "DHT11 sampling task started: gpio=%d period=%" PRIu32 "ms",
             s_config.data_gpio, s_config.sample_period_ms);

    while (!s_stop_requested) {
        float temperature_c = 0.0f;
        float humidity_percent = 0.0f;
        dht11_service_status_t status = DHT11_SERVICE_STATUS_NEVER_READ;
        esp_err_t err = dht11_read_once(&temperature_c, &humidity_percent, &status);

        dht11_update_snapshot(err, status, temperature_c, humidity_percent);

        dht11_service_snapshot_t snapshot = {0};
        taskENTER_CRITICAL(&s_snapshot_lock);
        snapshot = s_snapshot;
        taskEXIT_CRITICAL(&s_snapshot_lock);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "DHT11 sample #%" PRIu32 ": temperature=%.1fC humidity=%.1f%%",
                     snapshot.sample_seq, snapshot.temperature_c, snapshot.humidity_percent);
        } else if (snapshot.consecutive_failures >= s_config.max_consecutive_failures) {
            ESP_LOGE(TAG, "DHT11 sample #%" PRIu32 " failed: status=%s err=%s consecutive_failures=%" PRIu32,
                     snapshot.sample_seq, dht11_status_to_name(status), esp_err_to_name(err),
                     snapshot.consecutive_failures);
        } else {
            ESP_LOGW(TAG, "DHT11 sample #%" PRIu32 " failed: status=%s err=%s",
                     snapshot.sample_seq, dht11_status_to_name(status), esp_err_to_name(err));
        }

        uint32_t elapsed_ms = 0;
        while (!s_stop_requested && (elapsed_ms < s_config.sample_period_ms)) {
            const uint32_t delay_ms = (s_config.sample_period_ms - elapsed_ms) > DHT11_STOP_POLL_MS
                                          ? DHT11_STOP_POLL_MS
                                          : (s_config.sample_period_ms - elapsed_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            elapsed_ms += delay_ms;
        }
    }

    ESP_LOGI(TAG, "DHT11 sampling task stopped");
    if (s_done_sem != NULL) {
        xSemaphoreGive(s_done_sem);
    }
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t dht11_service_init(const dht11_service_config_t *config)
{
    ESP_RETURN_ON_ERROR(dht11_validate_config(config), TAG, "Invalid DHT11 service configuration");

    if (s_initialized) {
        return ESP_OK;
    }

    s_config = *config;
    s_stop_requested = false;
    s_snapshot = (dht11_service_snapshot_t) {
        .valid = false,
        .status = DHT11_SERVICE_STATUS_NEVER_READ,
        .last_error = ESP_ERR_INVALID_STATE,
    };

    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(s_config.data_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to configure DHT11 data GPIO");

    s_done_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done_sem != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create DHT11 done semaphore");

    BaseType_t ret = xTaskCreate(dht11_sample_task,
                                 DHT11_TASK_NAME,
                                 s_config.task_stack_size,
                                 NULL,
                                 s_config.task_priority,
                                 &s_task_handle);
    if (ret != pdPASS) {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "DHT11 service initialized: gpio=%d period=%" PRIu32 "ms failure_threshold=%" PRIu32,
             s_config.data_gpio, s_config.sample_period_ms, s_config.max_consecutive_failures);
    return ESP_OK;
}

esp_err_t dht11_service_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_stop_requested = true;
    if ((s_done_sem != NULL) && (s_task_handle != NULL)) {
        if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(DHT11_STOP_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Timed out waiting for DHT11 sampling task to stop");
            vTaskDelete(s_task_handle);
            s_task_handle = NULL;
        }
    }

    if (s_done_sem != NULL) {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
    }

    (void)gpio_set_direction(s_config.data_gpio, GPIO_MODE_INPUT);
    s_initialized = false;
    return ESP_OK;
}

esp_err_t dht11_service_get_snapshot(dht11_service_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot, ESP_ERR_INVALID_ARG, TAG, "Invalid DHT11 snapshot output");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "DHT11 service is not initialized");

    taskENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    return ESP_OK;
}
