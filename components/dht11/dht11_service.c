/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "dht11_service.h"

#include <inttypes.h>
#include <string.h>

#include "driver/rmt_rx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/gpio_num.h"

#define DHT11_TASK_NAME           "dht11"
#define DHT11_STOP_WAIT_MS        (2500U)
#define DHT11_STOP_POLL_MS        (100U)
#define DHT11_RMT_RESOLUTION_HZ   (1000000U)
#define DHT11_RMT_SYMBOLS         (64U)
#define DHT11_RMT_QUEUE_LENGTH    (1U)
#define DHT11_START_SIGNAL_US     (20000U)
#define DHT11_READ_TIMEOUT_MS     (100U)
#define DHT11_DATA_BITS           (40U)
#define DHT11_DATA_BYTES          (5U)
#define DHT11_BIT_ONE_THRESHOLD_US (50U)

static const char *TAG = "DHT11Service";

static dht11_service_config_t s_config;
static dht11_service_snapshot_t s_snapshot;
static TaskHandle_t s_task_handle;
static SemaphoreHandle_t s_done_sem;
static QueueHandle_t s_rmt_rx_queue;
static rmt_channel_handle_t s_rmt_rx_channel;
static rmt_symbol_word_t s_rmt_symbols[DHT11_RMT_SYMBOLS];
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_stop_requested;
static bool s_initialized;

typedef struct {
    uint8_t level;
    uint16_t duration_us;
} dht11_rmt_segment_t;

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

static bool dht11_rmt_rx_done_callback(rmt_channel_handle_t channel,
                                       const rmt_rx_done_event_data_t *edata,
                                       void *user_data)
{
    (void)channel;
    BaseType_t high_task_wakeup = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user_data, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static size_t dht11_append_segment(dht11_rmt_segment_t *segments,
                                   size_t max_segments,
                                   size_t count,
                                   uint8_t level,
                                   uint16_t duration_us)
{
    if (duration_us == 0 || count >= max_segments) {
        return count;
    }

    segments[count].level = level;
    segments[count].duration_us = duration_us;
    return count + 1;
}

static bool dht11_is_response_header(uint16_t low_us, uint16_t high_us)
{
    return (low_us >= 60U) && (low_us <= 110U) && (high_us >= 60U) && (high_us <= 110U);
}

static bool dht11_is_data_bit(uint16_t low_us, uint16_t high_us)
{
    return (low_us >= 30U) && (low_us <= 80U) && (high_us >= 15U) && (high_us <= 95U);
}

static esp_err_t dht11_parse_rmt_symbols(const rmt_symbol_word_t *symbols,
                                         size_t symbol_count,
                                         float *temperature_c,
                                         float *humidity_percent)
{
    dht11_rmt_segment_t segments[DHT11_RMT_SYMBOLS * 2] = {0};
    size_t segment_count = 0;
    for (size_t i = 0; i < symbol_count; ++i) {
        segment_count = dht11_append_segment(segments, DHT11_RMT_SYMBOLS * 2, segment_count,
                                             symbols[i].level0, symbols[i].duration0);
        segment_count = dht11_append_segment(segments, DHT11_RMT_SYMBOLS * 2, segment_count,
                                             symbols[i].level1, symbols[i].duration1);
    }

    uint8_t data[DHT11_DATA_BYTES] = {0};
    size_t bit_count = 0;
    bool response_seen = false;

    for (size_t i = 0; (i + 1U) < segment_count; ++i) {
        if (segments[i].level != 0 || segments[i + 1U].level != 1) {
            continue;
        }

        const uint16_t low_us = segments[i].duration_us;
        const uint16_t high_us = segments[i + 1U].duration_us;
        if (!response_seen) {
            if (dht11_is_response_header(low_us, high_us)) {
                response_seen = true;
            }
            continue;
        }

        if (!dht11_is_data_bit(low_us, high_us)) {
            continue;
        }

        const size_t byte_index = bit_count / 8U;
        data[byte_index] <<= 1;
        if (high_us > DHT11_BIT_ONE_THRESHOLD_US) {
            data[byte_index] |= 1U;
        }

        bit_count++;
        if (bit_count == DHT11_DATA_BITS) {
            break;
        }
    }

    ESP_RETURN_ON_FALSE(response_seen, ESP_ERR_TIMEOUT, TAG, "DHT11 response header not found");
    ESP_RETURN_ON_FALSE(bit_count == DHT11_DATA_BITS, ESP_ERR_TIMEOUT, TAG,
                        "DHT11 received only %u/%u bits", (unsigned)bit_count, (unsigned)DHT11_DATA_BITS);

    const uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    ESP_RETURN_ON_FALSE(data[4] == checksum, ESP_ERR_INVALID_CRC, TAG,
                        "DHT11 checksum failed: data=%02x %02x %02x %02x %02x checksum=%02x",
                        data[0], data[1], data[2], data[3], data[4], checksum);

    *humidity_percent = (float)data[0] + ((float)data[1] / 10.0f);
    *temperature_c = (float)data[2] + ((float)data[3] / 10.0f);
    return ESP_OK;
}

static esp_err_t dht11_start_rmt_rx(void)
{
    memset(s_rmt_symbols, 0, sizeof(s_rmt_symbols));
    xQueueReset(s_rmt_rx_queue);

    const rmt_receive_config_t rx_config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 1000000,
    };
    return rmt_receive(s_rmt_rx_channel, s_rmt_symbols, sizeof(s_rmt_symbols), &rx_config);
}

static esp_err_t dht11_read_once(float *temperature_c, float *humidity_percent, dht11_service_status_t *status)
{
    gpio_set_direction(s_config.data_gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(s_config.data_gpio, 0);
    esp_rom_delay_us(DHT11_START_SIGNAL_US);

    esp_err_t err = dht11_start_rmt_rx();
    if (err == ESP_OK) {
        gpio_set_level(s_config.data_gpio, 1);
        gpio_set_direction(s_config.data_gpio, GPIO_MODE_INPUT);

        rmt_rx_done_event_data_t rx_data = {0};
        if (xQueueReceive(s_rmt_rx_queue, &rx_data, pdMS_TO_TICKS(DHT11_READ_TIMEOUT_MS)) == pdTRUE) {
            err = dht11_parse_rmt_symbols(rx_data.received_symbols, rx_data.num_symbols,
                                          temperature_c, humidity_percent);
        } else {
            err = ESP_ERR_TIMEOUT;
        }
    } else {
        gpio_set_level(s_config.data_gpio, 1);
        gpio_set_direction(s_config.data_gpio, GPIO_MODE_INPUT);
    }

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

    s_rmt_rx_queue = xQueueCreate(DHT11_RMT_QUEUE_LENGTH, sizeof(rmt_rx_done_event_data_t));
    ESP_RETURN_ON_FALSE(s_rmt_rx_queue != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create DHT11 RMT RX queue");

    const rmt_rx_channel_config_t rmt_rx_config = {
        .gpio_num = s_config.data_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = DHT11_RMT_RESOLUTION_HZ,
        .mem_block_symbols = DHT11_RMT_SYMBOLS,
    };
    err = rmt_new_rx_channel(&rmt_rx_config, &s_rmt_rx_channel);
    if (err != ESP_OK) {
        vQueueDelete(s_rmt_rx_queue);
        s_rmt_rx_queue = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to create DHT11 RMT RX channel");
    }

    const rmt_rx_event_callbacks_t rmt_callbacks = {
        .on_recv_done = dht11_rmt_rx_done_callback,
    };
    err = rmt_rx_register_event_callbacks(s_rmt_rx_channel, &rmt_callbacks, s_rmt_rx_queue);
    if (err != ESP_OK) {
        rmt_del_channel(s_rmt_rx_channel);
        s_rmt_rx_channel = NULL;
        vQueueDelete(s_rmt_rx_queue);
        s_rmt_rx_queue = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to register DHT11 RMT RX callback");
    }

    err = rmt_enable(s_rmt_rx_channel);
    if (err != ESP_OK) {
        rmt_del_channel(s_rmt_rx_channel);
        s_rmt_rx_channel = NULL;
        vQueueDelete(s_rmt_rx_queue);
        s_rmt_rx_queue = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to enable DHT11 RMT RX channel");
    }

    s_done_sem = xSemaphoreCreateBinary();
    if (s_done_sem == NULL) {
        rmt_disable(s_rmt_rx_channel);
        rmt_del_channel(s_rmt_rx_channel);
        s_rmt_rx_channel = NULL;
        vQueueDelete(s_rmt_rx_queue);
        s_rmt_rx_queue = NULL;
        ESP_LOGE(TAG, "Failed to create DHT11 done semaphore");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(dht11_sample_task,
                                 DHT11_TASK_NAME,
                                 s_config.task_stack_size,
                                 NULL,
                                 s_config.task_priority,
                                 &s_task_handle);
    if (ret != pdPASS) {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
        rmt_disable(s_rmt_rx_channel);
        rmt_del_channel(s_rmt_rx_channel);
        s_rmt_rx_channel = NULL;
        vQueueDelete(s_rmt_rx_queue);
        s_rmt_rx_queue = NULL;
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

    if (s_rmt_rx_channel != NULL) {
        rmt_disable(s_rmt_rx_channel);
        rmt_del_channel(s_rmt_rx_channel);
        s_rmt_rx_channel = NULL;
    }
    if (s_rmt_rx_queue != NULL) {
        vQueueDelete(s_rmt_rx_queue);
        s_rmt_rx_queue = NULL;
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
