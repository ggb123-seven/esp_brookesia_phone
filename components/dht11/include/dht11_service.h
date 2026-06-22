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

#define DHT11_SERVICE_DEFAULT_DATA_GPIO                GPIO_NUM_32
#define DHT11_SERVICE_DEFAULT_SAMPLE_PERIOD_MS         (2000U)
#define DHT11_SERVICE_DEFAULT_MAX_CONSECUTIVE_FAILURES (3U)
#define DHT11_SERVICE_DEFAULT_TASK_STACK_SIZE          (3072U)
#define DHT11_SERVICE_DEFAULT_TASK_PRIORITY            (4U)

typedef enum {
    DHT11_SERVICE_STATUS_NEVER_READ = 0,
    DHT11_SERVICE_STATUS_OK,
    DHT11_SERVICE_STATUS_TIMEOUT,
    DHT11_SERVICE_STATUS_CHECKSUM_ERROR,
    DHT11_SERVICE_STATUS_IO_ERROR,
} dht11_service_status_t;

typedef struct {
    gpio_num_t data_gpio;
    uint32_t sample_period_ms;
    uint32_t max_consecutive_failures;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} dht11_service_config_t;

typedef struct {
    bool valid;
    dht11_service_status_t status;
    esp_err_t last_error;
    float temperature_c;
    float humidity_percent;
    int64_t timestamp_ms;
    uint32_t sample_seq;
    uint32_t consecutive_failures;
} dht11_service_snapshot_t;

esp_err_t dht11_service_init(const dht11_service_config_t *config);
esp_err_t dht11_service_deinit(void);
esp_err_t dht11_service_get_snapshot(dht11_service_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
