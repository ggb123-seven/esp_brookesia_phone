/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include <stdio.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_adapter_init.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_EXAMPLE_ENABLE_DHT11_SERVICE
#include "dht11_service.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_MQ2_SERVICE
#include "mq2_service.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_PARENT_CALL_ALERT_SERVICE
#include "parent_call_alert_service.h"
#endif

#include "local_dashboard_service.h"
#include "onenet_cloud_service.h"

#if CONFIG_EXAMPLE_ENABLE_AS608_VALIDATION
#include "as608_validation.h"
#endif

#include "apps.h"
#include "esp_brookesia.hpp"

static const char *TAG = "main";

#if CONFIG_EXAMPLE_ENABLE_MQ2_SERVICE &&                                    \
    CONFIG_EXAMPLE_ENABLE_PARENT_CALL_ALERT_SERVICE
#define ENVIRONMENT_ALERT_MONITOR_TASK_NAME       "env_alert"
#define ENVIRONMENT_ALERT_MONITOR_TASK_STACK_SIZE (3072U)
#define ENVIRONMENT_ALERT_MONITOR_TASK_PRIORITY   (4U)
#define ENVIRONMENT_ALERT_MONITOR_PERIOD_MS       (500U)

static TaskHandle_t s_environment_alert_monitor_task;

static void environment_alert_monitor_task(void *arg)
{
    (void)arg;
    bool last_alarm = false;

    while (true)
    {
        mq2_service_snapshot_t snapshot = {};
        if (mq2_service_get_snapshot(&snapshot) == ESP_OK && snapshot.valid)
        {
            const bool alarm = snapshot.state == MQ2_SERVICE_STATE_ALARM;
            if (alarm && !last_alarm)
            {
                parent_call_alert_event_t event = {};
                snprintf(event.reason, sizeof(event.reason), "%s", "mq2_alarm");
                snprintf(event.detail, sizeof(event.detail), "%s",
                         "MQ-2 detected smoke or combustible gas");
                snprintf(event.message, sizeof(event.message), "%s",
                         "环境监测检测到烟雾或可燃气体异常，请及时确认。");

                esp_err_t err = parent_call_alert_service_trigger(&event);
                if (err == ESP_OK)
                {
                    last_alarm = true;
                    ESP_LOGW(TAG, "Queued parent call alert from MQ-2 background monitor");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to queue MQ-2 parent call alert: %s",
                             esp_err_to_name(err));
                }
            }
            else if (!alarm)
            {
                last_alarm = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ENVIRONMENT_ALERT_MONITOR_PERIOD_MS));
    }
}

static esp_err_t start_environment_alert_monitor(void)
{
    if (s_environment_alert_monitor_task != NULL)
    {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(
        environment_alert_monitor_task, ENVIRONMENT_ALERT_MONITOR_TASK_NAME,
        ENVIRONMENT_ALERT_MONITOR_TASK_STACK_SIZE, NULL,
        ENVIRONMENT_ALERT_MONITOR_TASK_PRIORITY,
        &s_environment_alert_monitor_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
#endif

#if !CONFIG_EXAMPLE_ENABLE_SD_CARD
static esp_ldo_channel_handle_t sd_ldo_handle = NULL;
#endif
#if CONFIG_EXAMPLE_ENABLE_SD_CARD
static bool s_sdcard_mounted = false;
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_ENVIRONMENT_MONITOR
LV_FONT_DECLARE(environment_font_20);
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_FINGERPRINT
LV_FONT_DECLARE(fingerprint_font_20);
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_CLASSROOM_SCHEDULE
LV_FONT_DECLARE(classroom_schedule_font_20);
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_ENVIRONMENT_MONITOR ||                           \
    CONFIG_EXAMPLE_ENABLE_APP_FINGERPRINT ||                                   \
    CONFIG_EXAMPLE_ENABLE_APP_CLASSROOM_SCHEDULE
static void
use_chinese_launcher_font(ESP_Brookesia_PhoneStylesheet_t *stylesheet) {
  if (stylesheet == NULL) {
    return;
  }

#if CONFIG_EXAMPLE_ENABLE_APP_FINGERPRINT
  const lv_font_t *launcher_font = &fingerprint_font_20;
#elif CONFIG_EXAMPLE_ENABLE_APP_ENVIRONMENT_MONITOR
  const lv_font_t *launcher_font = &environment_font_20;
#else
  const lv_font_t *launcher_font = &classroom_schedule_font_20;
#endif

  ESP_Brookesia_StyleFont_t *fonts = stylesheet->core.home.text.default_fonts;
  for (int i = 0; i < stylesheet->core.home.text.default_fonts_num; ++i) {
    if (fonts[i].size_px == 22) {
      fonts[i].font_resource = launcher_font;
      return;
    }
  }
}
#endif

#if !CONFIG_EXAMPLE_ENABLE_SD_CARD
static esp_err_t init_sd_ldo_only(void) {
  esp_ldo_channel_config_t ldo_cfg = {
      .chan_id = 4,
      .voltage_mv = 3300,
  };
  return esp_ldo_acquire_channel(&ldo_cfg, &sd_ldo_handle);
}
#endif

extern "C" void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  ESP_ERROR_CHECK(bsp_spiffs_mount());
  ESP_LOGI(TAG, "SPIFFS mount successfully");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD
  err = bsp_sdcard_mount();
  if (err == ESP_OK) {
    s_sdcard_mounted = true;
    ESP_LOGI(TAG, "SD card mount successfully");
  } else {
    ESP_LOGW(TAG, "SD card mount failed: %s; SD roster import is unavailable",
             esp_err_to_name(err));
  }
#else
  ESP_ERROR_CHECK(init_sd_ldo_only());
#endif

  ESP_ERROR_CHECK(bsp_extra_codec_init());

#if CONFIG_EXAMPLE_ENABLE_DHT11_SERVICE
  dht11_service_config_t dht11_config = {
      .data_gpio = (gpio_num_t)CONFIG_EXAMPLE_DHT11_DATA_GPIO,
      .sample_period_ms = CONFIG_EXAMPLE_DHT11_SAMPLE_PERIOD_MS,
      .max_consecutive_failures = CONFIG_EXAMPLE_DHT11_MAX_CONSECUTIVE_FAILURES,
      .task_stack_size = CONFIG_EXAMPLE_DHT11_TASK_STACK_SIZE,
      .task_priority = CONFIG_EXAMPLE_DHT11_TASK_PRIORITY,
  };
  err = dht11_service_init(&dht11_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start DHT11 service: %s", esp_err_to_name(err));
  }
#endif

#if CONFIG_EXAMPLE_ENABLE_MQ2_SERVICE
  mq2_service_config_t mq2_config = {
      .do_gpio = (gpio_num_t)CONFIG_EXAMPLE_MQ2_DO_GPIO,
      .alarm_level = CONFIG_EXAMPLE_MQ2_ALARM_LEVEL,
      .sample_period_ms = CONFIG_EXAMPLE_MQ2_SAMPLE_PERIOD_MS,
      .warmup_ms = CONFIG_EXAMPLE_MQ2_WARMUP_MS,
      .confirm_samples = CONFIG_EXAMPLE_MQ2_CONFIRM_SAMPLES,
      .task_stack_size = CONFIG_EXAMPLE_MQ2_TASK_STACK_SIZE,
      .task_priority = CONFIG_EXAMPLE_MQ2_TASK_PRIORITY,
  };
  err = mq2_service_init(&mq2_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start MQ-2 service: %s", esp_err_to_name(err));
  }
#endif

#if CONFIG_EXAMPLE_ENABLE_PARENT_CALL_ALERT_SERVICE
  parent_call_alert_service_config_t parent_call_config = {
      .server_host = CONFIG_EXAMPLE_PARENT_CALL_ALERT_SERVER_HOST,
      .server_port = CONFIG_EXAMPLE_PARENT_CALL_ALERT_SERVER_PORT,
      .api_path = CONFIG_EXAMPLE_PARENT_CALL_ALERT_API_PATH,
      .token = CONFIG_EXAMPLE_PARENT_CALL_ALERT_TOKEN,
      .cooldown_ms = CONFIG_EXAMPLE_PARENT_CALL_ALERT_COOLDOWN_MS,
      .http_timeout_ms = CONFIG_EXAMPLE_PARENT_CALL_ALERT_REQUEST_TIMEOUT_MS,
      .air780e_phone_number =
          CONFIG_EXAMPLE_PARENT_CALL_ALERT_AIR780E_PHONE_NUMBER,
      .air780e_uart_num =
          (uart_port_t)CONFIG_EXAMPLE_PARENT_CALL_ALERT_AIR780E_UART_NUM,
      .air780e_tx_gpio = CONFIG_EXAMPLE_PARENT_CALL_ALERT_AIR780E_TX_GPIO,
      .air780e_rx_gpio = CONFIG_EXAMPLE_PARENT_CALL_ALERT_AIR780E_RX_GPIO,
      .air780e_baud_rate = CONFIG_EXAMPLE_PARENT_CALL_ALERT_AIR780E_BAUD_RATE,
      .air780e_command_timeout_ms =
          CONFIG_EXAMPLE_PARENT_CALL_ALERT_AIR780E_COMMAND_TIMEOUT_MS,
      .air780e_call_hold_ms =
          CONFIG_EXAMPLE_PARENT_CALL_ALERT_AIR780E_CALL_HOLD_MS,
      .v100c_call_timeout_ms =
          CONFIG_EXAMPLE_PARENT_CALL_ALERT_V100C_CALL_TIMEOUT_MS,
      .task_stack_size = CONFIG_EXAMPLE_PARENT_CALL_ALERT_TASK_STACK_SIZE,
      .task_priority = CONFIG_EXAMPLE_PARENT_CALL_ALERT_TASK_PRIORITY,
      .queue_length = CONFIG_EXAMPLE_PARENT_CALL_ALERT_QUEUE_LENGTH,
      .enabled = true,
#if CONFIG_EXAMPLE_PARENT_CALL_ALERT_TRANSPORT_HTTP
      .transport = PARENT_CALL_ALERT_TRANSPORT_HTTP,
#elif CONFIG_EXAMPLE_PARENT_CALL_ALERT_TRANSPORT_V100C_UART
      .transport = PARENT_CALL_ALERT_TRANSPORT_V100C_UART,
#elif CONFIG_EXAMPLE_PARENT_CALL_ALERT_TRANSPORT_AIR780E_AT
      .transport = PARENT_CALL_ALERT_TRANSPORT_AIR780E_AT,
#else
      .transport = PARENT_CALL_ALERT_TRANSPORT_MOCK,
#endif
  };
  err = parent_call_alert_service_init(&parent_call_config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start parent call alert service: %s",
             esp_err_to_name(err));
  }
  else
  {
#if CONFIG_EXAMPLE_PARENT_CALL_ALERT_AIR780E_SELF_TEST_ON_BOOT
    err = parent_call_alert_service_modem_self_test();
    if (err != ESP_OK)
    {
      ESP_LOGW(TAG, "Phone modem self-test failed: %s", esp_err_to_name(err));
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_MQ2_SERVICE
    err = start_environment_alert_monitor();
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to start environment alert monitor: %s",
               esp_err_to_name(err));
    }
#endif
  }
#endif

#if CONFIG_EXAMPLE_ENABLE_AS608_VALIDATION
  err = as608_validation_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start AS608 validation task: %s",
             esp_err_to_name(err));
  }
#endif

#if CONFIG_EXAMPLE_ENABLE_ONENET_CLOUD
  onenet_cloud_config_t onenet_config = {
      .enabled = true,
      .product_id = CONFIG_EXAMPLE_ONENET_PRODUCT_ID,
      .device_name = CONFIG_EXAMPLE_ONENET_DEVICE_NAME,
      .auth_token = CONFIG_EXAMPLE_ONENET_AUTH_TOKEN,
      .mqtt_host = CONFIG_EXAMPLE_ONENET_MQTT_HOST,
      .mqtt_port = CONFIG_EXAMPLE_ONENET_MQTT_PORT,
      .api_host = CONFIG_EXAMPLE_ONENET_API_HOST,
      .upload_interval_ms = CONFIG_EXAMPLE_ONENET_UPLOAD_INTERVAL_MS,
#if CONFIG_EXAMPLE_ONENET_ENABLE_PHOTO_UPLOAD
      .photo_upload_enabled = true,
#else
      .photo_upload_enabled = false,
#endif
      .photo_upload_cooldown_ms = CONFIG_EXAMPLE_ONENET_PHOTO_UPLOAD_COOLDOWN_MS,
      .photo_max_bytes = CONFIG_EXAMPLE_ONENET_PHOTO_MAX_BYTES,
      .task_stack_size = CONFIG_EXAMPLE_ONENET_TASK_STACK_SIZE,
      .task_priority = CONFIG_EXAMPLE_ONENET_TASK_PRIORITY,
  };
  err = onenet_cloud_service_init(&onenet_config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start OneNET cloud service: %s",
             esp_err_to_name(err));
  }
#endif

  err = local_dashboard_service_start();
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start local dashboard service: %s",
             esp_err_to_name(err));
  }

  bsp_display_cfg_t cfg = {
      .hw_cfg =
          {
              .hdmi_resolution = BSP_HDMI_RES_NONE,
              .dsi_bus =
                  {
                      .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
                  },
          },
  };
  lv_display_t *disp = lvgl_adapter_init(&cfg);
  assert(disp != nullptr && "Failed to init LVGL adapter");
  bsp_display_backlight_on();

  ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));

  ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone();
  assert(phone != nullptr && "Failed to create phone");

  ESP_Brookesia_PhoneStylesheet_t *phone_stylesheet =
      new ESP_Brookesia_PhoneStylesheet_t
      ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET();
  ESP_BROOKESIA_CHECK_NULL_EXIT(phone_stylesheet,
                                "Create phone stylesheet failed");
#if CONFIG_EXAMPLE_ENABLE_APP_ENVIRONMENT_MONITOR ||                           \
    CONFIG_EXAMPLE_ENABLE_APP_FINGERPRINT ||                                   \
    CONFIG_EXAMPLE_ENABLE_APP_CLASSROOM_SCHEDULE
  use_chinese_launcher_font(phone_stylesheet);
#endif
  ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->addStylesheet(*phone_stylesheet),
                                 "Add phone stylesheet failed");
  ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->activateStylesheet(*phone_stylesheet),
                                 "Activate phone stylesheet failed");

  assert(phone->begin() && "Failed to begin phone");

#if CONFIG_EXAMPLE_ENABLE_APP_CALCULATOR
  Calculator *calculator = new Calculator();
  assert(calculator != nullptr && "Failed to create calculator");
  assert((phone->installApp(calculator) >= 0) && "Failed to begin calculator");
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_FINGERPRINT
  FingerprintApp *fingerprint = new FingerprintApp();
  assert(fingerprint != nullptr && "Failed to create fingerprint app");
  assert((phone->installApp(fingerprint) >= 0) &&
         "Failed to begin fingerprint app");
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_ENVIRONMENT_MONITOR
  EnvironmentMonitorApp *environment_monitor = new EnvironmentMonitorApp();
  assert(environment_monitor != nullptr &&
         "Failed to create environment monitor app");
  assert((phone->installApp(environment_monitor) >= 0) &&
         "Failed to begin environment monitor app");
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_CLASSROOM_SCHEDULE
  ClassroomScheduleApp *classroom_schedule = new ClassroomScheduleApp();
  assert(classroom_schedule != nullptr &&
         "Failed to create classroom schedule app");
  assert((phone->installApp(classroom_schedule) >= 0) &&
         "Failed to begin classroom schedule app");
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_MUSIC_PLAYER
  MusicPlayer *music_player = new MusicPlayer();
  assert(music_player != nullptr && "Failed to create music_player");
  assert((phone->installApp(music_player) >= 0) &&
         "Failed to begin music_player");
#endif

  AppSettings *app_settings = new AppSettings();
  assert(app_settings != nullptr && "Failed to create app_settings");
  assert((phone->installApp(app_settings) >= 0) &&
         "Failed to begin app_settings");

#if CONFIG_EXAMPLE_ENABLE_APP_GAME_2048
  Game2048 *game_2048 = new Game2048();
  assert(game_2048 != nullptr && "Failed to create game_2048");
  assert((phone->installApp(game_2048) >= 0) && "Failed to begin game_2048");
#endif

  Camera *camera = new Camera(1280, 720);
  assert(camera != nullptr && "Failed to create camera");
  assert((phone->installApp(camera) >= 0) && "Failed to begin camera");

#if CONFIG_EXAMPLE_ENABLE_SD_CARD && CONFIG_EXAMPLE_ENABLE_APP_VIDEO_PLAYER
  if (s_sdcard_mounted) {
    ESP_LOGW(TAG, "Using Video Player example requires inserting the SD card "
                  "in advance and saving an MJPEG format video on the SD card");
    AppVideoPlayer *app_video_player = new AppVideoPlayer();
    assert(app_video_player != nullptr && "Failed to create app_video_player");
    assert((phone->installApp(app_video_player) >= 0) &&
           "Failed to begin app_video_player");
  }
#endif

  esp_lv_adapter_unlock();
}
