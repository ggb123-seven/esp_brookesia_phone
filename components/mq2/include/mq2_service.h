/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQ2_SERVICE_DEFAULT_DO_GPIO              GPIO_NUM_33
#define MQ2_SERVICE_DEFAULT_ALARM_LEVEL          (0)
#define MQ2_SERVICE_DEFAULT_SAMPLE_PERIOD_MS     (500U)
#define MQ2_SERVICE_DEFAULT_WARMUP_MS            (30000U)
#define MQ2_SERVICE_DEFAULT_CONFIRM_SAMPLES      (3U)
#define MQ2_SERVICE_DEFAULT_TASK_STACK_SIZE      (3072U)
#define MQ2_SERVICE_DEFAULT_TASK_PRIORITY        (4U)
#define MQ2_SERVICE_ANALOG_VALUE_UNAVAILABLE     (-1)

typedef enum {
    MQ2_SERVICE_STATE_WARMING_UP = 0,
    MQ2_SERVICE_STATE_NORMAL,
    MQ2_SERVICE_STATE_ALARM,
    MQ2_SERVICE_STATE_SENSOR_ERROR,
} mq2_service_state_t;

typedef struct {
    gpio_num_t do_gpio;
    int alarm_level;
    uint32_t sample_period_ms;
    uint32_t warmup_ms;
    uint32_t confirm_samples;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} mq2_service_config_t;

typedef struct {
    bool valid;
    mq2_service_state_t state;
    esp_err_t last_error;
    int digital_level;
    bool alarm_asserted;
    uint32_t stable_count;
    int adc_raw;
    int64_t timestamp_ms;
    uint32_t sample_seq;
} mq2_service_snapshot_t;

esp_err_t mq2_service_init(const mq2_service_config_t *config);
esp_err_t mq2_service_deinit(void);
esp_err_t mq2_service_get_snapshot(mq2_service_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
