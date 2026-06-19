/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "as608_port.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "driver_as608_interface.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AS608Port";

static as608_port_config_t s_config = {
    .uart_num = UART_NUM_1,
    .tx_io = UART_PIN_NO_CHANGE,
    .rx_io = UART_PIN_NO_CHANGE,
    .baud_rate = AS608_PORT_DEFAULT_BAUD_RATE,
    .rx_buffer_size = AS608_PORT_DEFAULT_RX_BUFFER_SIZE,
    .read_timeout_ms = AS608_PORT_DEFAULT_READ_TIMEOUT_MS,
};
static bool s_uart_installed;

esp_err_t as608_port_configure(const as608_port_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid AS608 UART config");
    ESP_RETURN_ON_FALSE(config->tx_io >= 0, ESP_ERR_INVALID_ARG, TAG, "Invalid AS608 TX GPIO");
    ESP_RETURN_ON_FALSE(config->rx_io >= 0, ESP_ERR_INVALID_ARG, TAG, "Invalid AS608 RX GPIO");
    ESP_RETURN_ON_FALSE(config->baud_rate > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid AS608 baud rate");

    s_config = *config;
    if (s_config.rx_buffer_size <= 0) {
        s_config.rx_buffer_size = AS608_PORT_DEFAULT_RX_BUFFER_SIZE;
    }
    if (s_config.read_timeout_ms == 0) {
        s_config.read_timeout_ms = AS608_PORT_DEFAULT_READ_TIMEOUT_MS;
    }

    return ESP_OK;
}

uint8_t as608_interface_uart_init(void)
{
    if (s_uart_installed) {
        return 0;
    }

    const uart_config_t uart_config = {
        .baud_rate = s_config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(s_config.uart_num, s_config.rx_buffer_size, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return 1;
    }

    err = uart_param_config(s_config.uart_num, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(err));
        (void)uart_driver_delete(s_config.uart_num);
        return 1;
    }

    err = uart_set_pin(s_config.uart_num, s_config.tx_io, s_config.rx_io,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        (void)uart_driver_delete(s_config.uart_num);
        return 1;
    }

    s_uart_installed = true;
    ESP_LOGI(TAG, "AS608 UART ready: uart=%d tx=%d rx=%d baud=%d",
             s_config.uart_num, s_config.tx_io, s_config.rx_io, s_config.baud_rate);

    return 0;
}

uint8_t as608_interface_uart_deinit(void)
{
    if (!s_uart_installed) {
        return 0;
    }

    esp_err_t err = uart_driver_delete(s_config.uart_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete UART driver: %s", esp_err_to_name(err));
        return 1;
    }

    s_uart_installed = false;
    return 0;
}

uint16_t as608_interface_uart_read(uint8_t *buf, uint16_t len)
{
    if (!s_uart_installed || buf == NULL || len == 0) {
        return 0;
    }

    int read_len = uart_read_bytes(s_config.uart_num, buf, len,
                                   pdMS_TO_TICKS(s_config.read_timeout_ms));
    if (read_len < 0) {
        ESP_LOGE(TAG, "Failed to read UART bytes");
        return 0;
    }

    return (uint16_t)read_len;
}

uint8_t as608_interface_uart_write(uint8_t *buf, uint16_t len)
{
    if (!s_uart_installed || buf == NULL || len == 0) {
        return 1;
    }

    int written = uart_write_bytes(s_config.uart_num, (const char *)buf, len);
    if (written != len) {
        ESP_LOGE(TAG, "Failed to write UART bytes: expected=%u actual=%d", len, written);
        return 1;
    }

    esp_err_t err = uart_wait_tx_done(s_config.uart_num, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART TX wait failed: %s", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

uint8_t as608_interface_uart_flush(void)
{
    if (!s_uart_installed) {
        return 1;
    }

    esp_err_t err = uart_flush_input(s_config.uart_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flush UART input: %s", esp_err_to_name(err));
        return 1;
    }

    return 0;
}

void as608_interface_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void as608_interface_debug_print(const char *const fmt, ...)
{
    if (fmt == NULL) {
        return;
    }

    char buffer[160];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", buffer);
}
