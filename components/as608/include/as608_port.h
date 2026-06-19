/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AS608_PORT_DEFAULT_BAUD_RATE      (57600)
#define AS608_PORT_DEFAULT_RX_BUFFER_SIZE (2048)
#define AS608_PORT_DEFAULT_READ_TIMEOUT_MS (300)

typedef struct {
    uart_port_t uart_num;
    int tx_io;
    int rx_io;
    int baud_rate;
    int rx_buffer_size;
    uint32_t read_timeout_ms;
} as608_port_config_t;

esp_err_t as608_port_configure(const as608_port_config_t *config);

#ifdef __cplusplus
}
#endif
