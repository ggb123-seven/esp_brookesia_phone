/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mq2_service.h"

#include <inttypes.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/gpio_num.h"

#define MQ2_TASK_NAME     "mq2"
#define MQ2_STOP_WAIT_MS  (1500U)
#define MQ2_STOP_POLL_MS  (100U)

static const char *TAG = "MQ2Service";

static mq2_service_config_t s_config;
static mq2_service_snapshot_t s_snapshot;
static TaskHandle_t s_task_handle;
static SemaphoreHandle_t s_done_sem;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_stop_requested;
static bool s_initialized;
static int64_t s_started_at_ms;

static const char *mq2_state_to_name(mq2_service_state_t state)
{
    switch (state) {
    case MQ2_SERVICE_STATE_WARMING_UP:
        return "WARMING_UP";
    case MQ2_SERVICE_STATE_NORMAL:
        return "NORMAL";
    case MQ2_SERVICE_STATE_ALARM:
        return "ALARM";
    case MQ2_SERVICE_STATE_SENSOR_ERROR:
        return "SENSOR_ERROR";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t mq2_validate_config(const mq2_service_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid MQ-2 service config");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(config->do_gpio), ESP_ERR_INVALID_ARG, TAG,
                        "Invalid MQ-2 DO GPIO");
    ESP_RETURN_ON_FALSE((config->alarm_level == 0) || (config->alarm_level == 1),
                        ESP_ERR_INVALID_ARG, TAG, "MQ-2 alarm level must be 0 or 1");
    ESP_RETURN_ON_FALSE(config->sample_period_ms >= 100, ESP_ERR_INVALID_ARG, TAG,
                        "MQ-2 sample period must be at least 100 ms");
    ESP_RETURN_ON_FALSE(config->confirm_samples > 0, ESP_ERR_INVALID_ARG, TAG,
                        "MQ-2 confirm samples must be greater than zero");
    ESP_RETURN_ON_FALSE(config->task_stack_size >= 2048, ESP_ERR_INVALID_ARG, TAG,
                        "MQ-2 task stack size is too small");
    ESP_RETURN_ON_FALSE(config->task_priority > 0, ESP_ERR_INVALID_ARG, TAG,
                        "MQ-2 task priority must be greater than zero");
    return ESP_OK;
}

static void mq2_update_snapshot(esp_err_t err,
                                mq2_service_state_t state,
                                int digital_level,
                                bool alarm_asserted,
                                uint32_t stable_count)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.valid = (err == ESP_OK) && (state != MQ2_SERVICE_STATE_WARMING_UP);
    s_snapshot.sample_seq++;
    s_snapshot.state = state;
    s_snapshot.last_error = err;
    s_snapshot.digital_level = digital_level;
    s_snapshot.alarm_asserted = alarm_asserted;
    s_snapshot.stable_count = stable_count;
    s_snapshot.adc_raw = MQ2_SERVICE_ANALOG_VALUE_UNAVAILABLE;
    s_snapshot.timestamp_ms = esp_timer_get_time() / 1000;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

static bool mq2_is_warming_up(int64_t now_ms)
{
    return (s_config.warmup_ms > 0) && ((uint32_t)(now_ms - s_started_at_ms) < s_config.warmup_ms);
}

static void mq2_sample_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "MQ-2 sampling task started: do_gpio=%d alarm_level=%d period=%" PRIu32 "ms warmup=%" PRIu32 "ms",
             s_config.do_gpio, s_config.alarm_level, s_config.sample_period_ms, s_config.warmup_ms);

    mq2_service_state_t current_state = MQ2_SERVICE_STATE_WARMING_UP;
    uint32_t stable_count = 0;
    bool last_alarm_level = false;

    while (!s_stop_requested) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const int digital_level = gpio_get_level(s_config.do_gpio);
        const bool alarm_level = (digital_level == s_config.alarm_level);
        mq2_service_state_t next_state = current_state;

        if (mq2_is_warming_up(now_ms)) {
            next_state = MQ2_SERVICE_STATE_WARMING_UP;
            stable_count = 0;
        } else {
            if (current_state == MQ2_SERVICE_STATE_WARMING_UP) {
                current_state = alarm_level ? MQ2_SERVICE_STATE_ALARM : MQ2_SERVICE_STATE_NORMAL;
                last_alarm_level = alarm_level;
                stable_count = s_config.confirm_samples;
                ESP_LOGI(TAG, "MQ-2 warmup finished: state=%s level=%d",
                         mq2_state_to_name(current_state), digital_level);
            } else if (alarm_level == last_alarm_level) {
                if (stable_count < s_config.confirm_samples) {
                    stable_count++;
                }
            } else {
                last_alarm_level = alarm_level;
                stable_count = 1;
            }

            if (stable_count >= s_config.confirm_samples) {
                next_state = alarm_level ? MQ2_SERVICE_STATE_ALARM : MQ2_SERVICE_STATE_NORMAL;
            } else {
                next_state = current_state;
            }
        }

        if (next_state != current_state) {
            ESP_LOGW(TAG, "MQ-2 state changed: %s -> %s level=%d stable_count=%" PRIu32,
                     mq2_state_to_name(current_state), mq2_state_to_name(next_state),
                     digital_level, stable_count);
            current_state = next_state;
        }

        mq2_update_snapshot(ESP_OK, current_state, digital_level,
                            current_state == MQ2_SERVICE_STATE_ALARM, stable_count);

        uint32_t elapsed_ms = 0;
        while (!s_stop_requested && (elapsed_ms < s_config.sample_period_ms)) {
            const uint32_t delay_ms = (s_config.sample_period_ms - elapsed_ms) > MQ2_STOP_POLL_MS
                                          ? MQ2_STOP_POLL_MS
                                          : (s_config.sample_period_ms - elapsed_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            elapsed_ms += delay_ms;
        }
    }

    ESP_LOGI(TAG, "MQ-2 sampling task stopped");
    if (s_done_sem != NULL) {
        xSemaphoreGive(s_done_sem);
    }
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t mq2_service_init(const mq2_service_config_t *config)
{
    ESP_RETURN_ON_ERROR(mq2_validate_config(config), TAG, "Invalid MQ-2 service configuration");

    if (s_initialized) {
        return ESP_OK;
    }

    s_config = *config;
    s_stop_requested = false;
    s_started_at_ms = esp_timer_get_time() / 1000;
    s_snapshot = (mq2_service_snapshot_t) {
        .valid = false,
        .state = MQ2_SERVICE_STATE_WARMING_UP,
        .last_error = ESP_ERR_INVALID_STATE,
        .digital_level = -1,
        .adc_raw = MQ2_SERVICE_ANALOG_VALUE_UNAVAILABLE,
    };

    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(s_config.do_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to configure MQ-2 DO GPIO");

    s_done_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done_sem != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create MQ-2 done semaphore");

    BaseType_t ret = xTaskCreate(mq2_sample_task,
                                 MQ2_TASK_NAME,
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
    ESP_LOGI(TAG,
             "MQ-2 service initialized: do_gpio=%d alarm_level=%d period=%" PRIu32 "ms warmup=%" PRIu32 "ms confirm=%" PRIu32,
             s_config.do_gpio, s_config.alarm_level, s_config.sample_period_ms,
             s_config.warmup_ms, s_config.confirm_samples);
    return ESP_OK;
}

esp_err_t mq2_service_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_stop_requested = true;
    if ((s_done_sem != NULL) && (s_task_handle != NULL)) {
        if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(MQ2_STOP_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Timed out waiting for MQ-2 sampling task to stop");
            vTaskDelete(s_task_handle);
            s_task_handle = NULL;
        }
    }

    if (s_done_sem != NULL) {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
    }

    (void)gpio_set_direction(s_config.do_gpio, GPIO_MODE_INPUT);
    s_initialized = false;
    return ESP_OK;
}

esp_err_t mq2_service_get_snapshot(mq2_service_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot, ESP_ERR_INVALID_ARG, TAG, "Invalid MQ-2 snapshot output");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "MQ-2 service is not initialized");

    taskENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    return ESP_OK;
}
