/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_memory_utils.h"
#include "esp_mac.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "bsp_board_extra.h"
#include "nvs.h"

#include "ui/ui.h"
#include "Setting.hpp"
#include "app_sntp.h"

#include "esp_brookesia_versions.h"

#define HOME_REFRESH_TASK_STACK_SIZE    (1024 * 4)
#define HOME_REFRESH_TASK_PRIORITY      (1)
#define HOME_REFRESH_TASK_PERIOD_MS     (2000)

#define SNTP_TASK_STACK_SIZE            (1024 * 4)
#define SNTP_TASK_PRIORITY              (4)
#define WIFI_CONNECT_UI_WAIT_TIME_MS    (1 * 1000)
#define WIFI_UI_TIMER_PERIOD_MS         (100)

#define SCREEN_BRIGHTNESS_MIN           (20)
#define SCREEN_BRIGHTNESS_MAX           (BSP_LCD_BACKLIGHT_BRIGHTNESS_MAX)

#define SPEAKER_VOLUME_MIN              (0)
#define SPEAKER_VOLUME_MAX              (100)

#define NVS_STORAGE_NAMESPACE           "storage"
#define NVS_KEY_WIFI_ENABLE             "wifi_en"
#define NVS_KEY_BLE_ENABLE              "ble_en"
#define NVS_KEY_AUDIO_VOLUME            "volume"
#define NVS_KEY_DISPLAY_BRIGHTNESS      "brightness"

#define UI_MAIN_ITEM_LEFT_OFFSET        (20)
#define UI_WIFI_LIST_UP_OFFSET          (20)
#define UI_WIFI_LIST_UP_PAD             (20)
#define UI_WIFI_LIST_DOWN_PAD           (20)
#define UI_WIFI_LIST_H_PERCENT          (75)
#define UI_WIFI_LIST_ITEM_H             (60)
#define UI_WIFI_LIST_ITEM_FONT          (&lv_font_montserrat_26)
#define UI_WIFI_KEYBOARD_H_PERCENT      (30)
#define UI_WIFI_ICON_LOCK_RIGHT_OFFSET       (-10)
#define UI_WIFI_ICON_SIGNAL_RIGHT_OFFSET     (-50)
#define UI_WIFI_ICON_CONNECT_RIGHT_OFFSET    (-90)

using namespace std;

static const char TAG[] = "EUI_Setting";

static bool s_sntp_started = false;

static uint8_t base_mac_addr[6] = {0};
static char mac_str[18] = {0};

static int brightness;

LV_IMG_DECLARE(img_wifisignal_absent);
LV_IMG_DECLARE(img_wifisignal_wake);
LV_IMG_DECLARE(img_wifisignal_moderate);
LV_IMG_DECLARE(img_wifisignal_good);
LV_IMG_DECLARE(img_wifi_lock);
LV_IMG_DECLARE(img_wifi_connect_success);
LV_IMG_DECLARE(img_wifi_connect_fail);

LV_IMG_DECLARE(img_app_setting);
extern lv_obj_t *ui_Min;
extern lv_obj_t *ui_Hour;
extern lv_obj_t *ui_Sec;
extern lv_obj_t *ui_Date;
extern lv_obj_t *ui_Clock_Number;

static void sntpInitTask(void *arg)
{
    (void)arg;
    app_sntp_init();
    vTaskDelete(NULL);
}

AppSettings::AppSettings():
    ESP_Brookesia_PhoneApp("Settings", &img_app_setting, false),                  // auto_resize_visual_area
    _is_ui_resumed(false),
    _is_ui_del(true),
    _screen_index(UI_MAIN_SETTING_INDEX),
    _wifi_signal_strength_level(WIFI_SIGNAL_STRENGTH_NONE),
    _wifi_manager(),
    _panel_wifi_connect(nullptr),
    _spinner_wifi_connect(nullptr),
    _img_wifi_connect(nullptr),
    _wifi_refresh_btn(nullptr),
    _wifi_ui_timer(nullptr),
    _screen_list({nullptr}),
    _wifi_panel_buttons({nullptr}),
    _wifi_ssid_labels({nullptr}),
    _wifi_lock_images({nullptr}),
    _wifi_signal_images({nullptr}),
    _wifi_connected_labels({nullptr}),
    _wifi_scan_results{},
    _wifi_scan_count(0),
    _last_wifi_state_version(UINT32_MAX),
    _last_wifi_scan_version(UINT32_MAX),
    _last_wifi_result_version(UINT32_MAX),
    _wifi_result_shown_at(0),
    _shown_wifi_result(WifiConnectionManager::ConnectResult::NONE),
    status_bar(nullptr),
    backstage(nullptr)
{
}

AppSettings::~AppSettings()
{
}

bool AppSettings::run(void)
{
    _is_ui_del = false;
    _last_wifi_state_version = UINT32_MAX;
    _last_wifi_scan_version = UINT32_MAX;
    _last_wifi_result_version = UINT32_MAX;
    _shown_wifi_result = WifiConnectionManager::ConnectResult::NONE;
    _wifi_result_shown_at = 0;
    _wifi_scan_count = 0;

    // Initialize Squareline UI
    ui_setting_init();

    // Get MAC
    esp_read_mac(base_mac_addr, ESP_MAC_EFUSE_FACTORY);
    snprintf(mac_str, sizeof(mac_str), "%02X-%02X-%02X-%02X-%02X-%02X",
             base_mac_addr[0], base_mac_addr[1], base_mac_addr[2],
             base_mac_addr[3], base_mac_addr[4], base_mac_addr[5]);


    // Initialize custom UI
    extraUiInit();

    // Update UI by NVS parameters
    updateUiByNvsParam();

    _wifi_ui_timer = lv_timer_create(wifiUiTimerCallback, WIFI_UI_TIMER_PERIOD_MS, this);
    if (_wifi_ui_timer == nullptr) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi UI timer");
        return false;
    }
    syncWifiUi();

    return true;
}

bool AppSettings::back(void)
{
    _is_ui_resumed = false;

    if (_screen_index == UI_WIFI_CONNECT_INDEX) {
        lv_scr_load(ui_ScreenSettingWiFi);
    } else if (_screen_index != UI_MAIN_SETTING_INDEX) {
        if (_screen_index == UI_WIFI_SCAN_INDEX) {
            endWifiDiscovery();
        }
        lv_scr_load(ui_ScreenSettingMain);
    } else {
        endWifiDiscovery();
        notifyCoreClosed();
    }

    return true;
}

bool AppSettings::close(void)
{
    endWifiDiscovery();
    if (_wifi_ui_timer != nullptr) {
        lv_timer_del(_wifi_ui_timer);
        _wifi_ui_timer = nullptr;
    }

    _is_ui_del = true;
    _wifi_refresh_btn = nullptr;
    _panel_wifi_connect = nullptr;
    _spinner_wifi_connect = nullptr;
    _img_wifi_connect = nullptr;
    _wifi_panel_buttons.fill(nullptr);
    _wifi_ssid_labels.fill(nullptr);
    _wifi_lock_images.fill(nullptr);
    _wifi_signal_images.fill(nullptr);
    _wifi_connected_labels.fill(nullptr);

    return true;
}

bool AppSettings::init(void)
{
    ESP_Brookesia_Phone *phone = getPhone();
    ESP_Brookesia_PhoneHome& home = phone->getHome();
    status_bar = home.getStatusBar();
    backstage = home.getRecentsScreen();

    // Initialize NVS parameters
    _nvs_param_map[NVS_KEY_WIFI_ENABLE] = false;
    _nvs_param_map[NVS_KEY_BLE_ENABLE] = false;
    _nvs_param_map[NVS_KEY_AUDIO_VOLUME] = bsp_extra_codec_volume_get();
    _nvs_param_map[NVS_KEY_AUDIO_VOLUME] = max(min((int)_nvs_param_map[NVS_KEY_AUDIO_VOLUME], SPEAKER_VOLUME_MAX), SPEAKER_VOLUME_MIN);
    // _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = bsp_display_brightness_get();
    _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = brightness;
    _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = max(min((int)_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS], SCREEN_BRIGHTNESS_MAX), SCREEN_BRIGHTNESS_MIN);
    // Load NVS parameters if exist
    loadNvsParam();
    // Update System parameters
    bsp_extra_codec_volume_set(_nvs_param_map[NVS_KEY_AUDIO_VOLUME], (int *)&_nvs_param_map[NVS_KEY_AUDIO_VOLUME]);
    bsp_display_brightness_set(_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS]);

    esp_err_t err = _wifi_manager.begin(_nvs_param_map[NVS_KEY_WIFI_ENABLE] != 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi manager: %s", esp_err_to_name(err));
    }

    BaseType_t created = xTaskCreate(euiRefresTask, "Home Refresh", HOME_REFRESH_TASK_STACK_SIZE,
                                     this, HOME_REFRESH_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create home refresh task");
    }

    return true;
}

bool AppSettings::pause(void)
{
    _is_ui_resumed = true;

    return true;
}

bool AppSettings::resume(void)
{
    _is_ui_resumed = false;

    return true;
}

void AppSettings::extraUiInit(void)
{
    /* Main */
    lv_label_set_text(ui_LabelPanelSettingMainContainer3Volume, "Audio");
    lv_label_set_text(ui_LabelPanelSettingMainContainer4Light, "Display");
    lv_obj_align_to(ui_LabelPanelSettingMainContainer1WiFi, ui_ImagePanelSettingMainContainer1WiFi, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer2Blue, ui_ImagePanelSettingMainContainer2Blue, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer3Volume, ui_ImagePanelSettingMainContainer3Volume, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer4Light, ui_ImagePanelSettingMainContainer4Light, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    lv_obj_align_to(ui_LabelPanelSettingMainContainer5About, ui_ImagePanelSettingMainContainer5About, LV_ALIGN_OUT_RIGHT_MID,
                    UI_MAIN_ITEM_LEFT_OFFSET, 0);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_MAIN_SETTING_INDEX] = ui_ScreenSettingMain;
    lv_obj_add_event_cb(ui_ScreenSettingMain, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* WiFi */
    // Switch
    lv_obj_add_event_cb(ui_SwitchPanelScreenSettingWiFiSwitch, onSwitchPanelScreenSettingWiFiSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    // List
    // lv_obj_clear_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui_PanelScreenSettingWiFiList, LV_DIR_VER);
    lv_obj_set_height(ui_PanelScreenSettingWiFiList, lv_pct(UI_WIFI_LIST_H_PERCENT));
    lv_obj_align_to(ui_PanelScreenSettingWiFiList, ui_PanelScreenSettingWiFiSwitch, LV_ALIGN_OUT_BOTTOM_MID, 0,
                    UI_WIFI_LIST_UP_OFFSET);
    lv_obj_set_style_pad_all(ui_PanelScreenSettingWiFiList, 0, 0);
    lv_obj_set_style_pad_top(ui_PanelScreenSettingWiFiList, UI_WIFI_LIST_UP_PAD, 0);
    lv_obj_set_style_pad_bottom(ui_PanelScreenSettingWiFiList, UI_WIFI_LIST_DOWN_PAD, 0);
    for (size_t i = 0; i < WifiConnectionManager::MAX_SCAN_RESULTS; ++i) {
        _wifi_panel_buttons[i] = lv_obj_create(ui_PanelScreenSettingWiFiList);
        lv_obj_set_size(_wifi_panel_buttons[i], lv_pct(100), UI_WIFI_LIST_ITEM_H);
        lv_obj_set_style_radius(_wifi_panel_buttons[i], 0, 0);
        lv_obj_set_style_border_width(_wifi_panel_buttons[i], 0, 0);
        lv_obj_set_style_text_font(_wifi_panel_buttons[i], UI_WIFI_LIST_ITEM_FONT, 0);
        lv_obj_add_flag(_wifi_panel_buttons[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(_wifi_panel_buttons[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(_wifi_panel_buttons[i], lv_color_hex(0xCBCBCB),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(_wifi_panel_buttons[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(_wifi_panel_buttons[i], lv_color_white(),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(_wifi_panel_buttons[i], LV_OPA_COVER,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_flag(_wifi_panel_buttons[i], LV_OBJ_FLAG_HIDDEN);

        _wifi_ssid_labels[i] = lv_label_create(_wifi_panel_buttons[i]);
        lv_obj_set_width(_wifi_ssid_labels[i], lv_pct(70));
        lv_label_set_long_mode(_wifi_ssid_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_align(_wifi_ssid_labels[i], LV_ALIGN_LEFT_MID);

        _wifi_lock_images[i] = lv_img_create(_wifi_panel_buttons[i]);
        lv_obj_align(_wifi_lock_images[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_LOCK_RIGHT_OFFSET, 0);
        lv_obj_add_flag(_wifi_lock_images[i], LV_OBJ_FLAG_HIDDEN);

        _wifi_signal_images[i] = lv_img_create(_wifi_panel_buttons[i]);
        lv_obj_align(_wifi_signal_images[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_SIGNAL_RIGHT_OFFSET, 0);

        _wifi_connected_labels[i] = lv_label_create(_wifi_panel_buttons[i]);
        lv_label_set_text(_wifi_connected_labels[i], LV_SYMBOL_OK);
        lv_obj_align(_wifi_connected_labels[i], LV_ALIGN_RIGHT_MID, UI_WIFI_ICON_CONNECT_RIGHT_OFFSET, 0);
        lv_obj_add_flag(_wifi_connected_labels[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_event_cb(_wifi_panel_buttons[i], onButtonWifiListClickedEventCallback,
                            LV_EVENT_CLICKED, this);
    }
    lv_obj_add_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(ui_SwitchPanelScreenSettingWiFiSwitch, LV_ALIGN_RIGHT_MID, -12, 0);
    _wifi_refresh_btn = lv_btn_create(ui_PanelScreenSettingWiFiSwitch);
    lv_obj_set_size(_wifi_refresh_btn, 44, 44);
    lv_obj_align(_wifi_refresh_btn, LV_ALIGN_RIGHT_MID, -75, 0);
    lv_obj_set_style_radius(_wifi_refresh_btn, 6, 0);
    lv_obj_set_style_bg_color(_wifi_refresh_btn, lv_color_hex(0xE5F3FF), 0);
    lv_obj_set_style_shadow_width(_wifi_refresh_btn, 0, 0);
    lv_obj_add_event_cb(_wifi_refresh_btn, onWifiRefreshClickedEventCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *refresh_icon = lv_label_create(_wifi_refresh_btn);
    lv_label_set_text(refresh_icon, LV_SYMBOL_REFRESH);
    lv_obj_center(refresh_icon);
    lv_obj_add_flag(ui_ButtonScreenSettingWiFiReturn, LV_OBJ_FLAG_HIDDEN);
    // Connect
    lv_obj_add_flag(ui_SpinnerScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
    _panel_wifi_connect = lv_obj_create(ui_ScreenSettingVerification);
    lv_obj_set_size(_panel_wifi_connect, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(_panel_wifi_connect, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(_panel_wifi_connect, LV_OPA_50, 0);
    lv_obj_center(_panel_wifi_connect);
    _img_wifi_connect = lv_img_create(_panel_wifi_connect);
    lv_obj_center(_img_wifi_connect);
    _spinner_wifi_connect = lv_spinner_create(_panel_wifi_connect, 1000, 600);
    lv_obj_set_size(_spinner_wifi_connect, lv_pct(20), lv_pct(20));
    lv_obj_center(_spinner_wifi_connect);
    processWifiConnect(WIFI_CONNECT_HIDE);
    // Keyboard
    lv_textarea_set_password_mode(ui_TextAreaScreenSettingVerificationPassword, true);
    // lv_obj_set_size(ui_KeyboardScreenSettingVerification, lv_pct(100), lv_pct(UI_WIFI_KEYBOARD_H_PERCENT));
    // lv_obj_align(ui_KeyboardScreenSettingVerification, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(ui_KeyboardScreenSettingVerification, ui_TextAreaScreenSettingVerificationPassword);
    lv_obj_add_event_cb(ui_TextAreaScreenSettingVerificationPassword,
                        onTextAreaScreenSettingVerificationPasswordClickedEventCallback,
                        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(ui_KeyboardScreenSettingVerification, onKeyboardScreenSettingVerificationClickedEventCallback,
                        LV_EVENT_CLICKED, this);
    setVerificationKeyboardVisible(false);
    // Record the screen index and install the screen loaded event callback
    lv_obj_add_flag(ui_ButtonScreenSettingBLEReturn, LV_OBJ_FLAG_HIDDEN);
    _screen_list[UI_WIFI_SCAN_INDEX] = ui_ScreenSettingWiFi;
    lv_obj_add_event_cb(ui_ScreenSettingWiFi, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);
    _screen_list[UI_WIFI_CONNECT_INDEX] = ui_ScreenSettingVerification;
    lv_obj_add_event_cb(ui_ScreenSettingVerification, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* Bluetooth */
    lv_obj_add_event_cb(ui_SwitchPanelScreenSettingBLESwitch, onSwitchPanelScreenSettingBLESwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_BLUETOOTH_SETTING_INDEX] = ui_ScreenSettingBLE;
    lv_obj_add_event_cb(ui_ScreenSettingBLE, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* Display */
    lv_slider_set_range(ui_SliderPanelScreenSettingLightSwitch1, SCREEN_BRIGHTNESS_MIN, SCREEN_BRIGHTNESS_MAX);
    lv_obj_add_event_cb(ui_SliderPanelScreenSettingLightSwitch1, onSliderPanelLightSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_flag(ui_ButtonScreenSettingLightReturn, LV_OBJ_FLAG_HIDDEN);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_BRIGHTNESS_SETTING_INDEX] = ui_ScreenSettingLight;
    lv_obj_add_event_cb(ui_ScreenSettingLight, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* Audio */
    lv_slider_set_range(ui_SliderPanelScreenSettingVolumeSwitch, SPEAKER_VOLUME_MIN, SPEAKER_VOLUME_MAX);
    lv_obj_add_event_cb(ui_SliderPanelScreenSettingVolumeSwitch, onSliderPanelVolumeSwitchValueChangeEventCallback,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_flag(ui_ButtonScreenSettingVolumeReturn, LV_OBJ_FLAG_HIDDEN);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_VOLUME_SETTING_INDEX] = ui_ScreenSettingVolume;
    lv_obj_add_event_cb(ui_ScreenSettingVolume, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    /* About */
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout4, "ESP_Brookesia");
    lv_obj_add_flag(ui_ButtonScreenSettingAboutReturn, LV_OBJ_FLAG_HIDDEN);
    // Record the screen index and install the screen loaded event callback
    _screen_list[UI_ABOUT_SETTING_INDEX] = ui_ScreenSettingAbout;
    lv_obj_add_event_cb(ui_ScreenSettingAbout, onScreenLoadEventCallback, LV_EVENT_SCREEN_LOADED, this);

    lv_obj_add_flag(ui_PanelSettingMainContainerItem2, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout3, mac_str);
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout5, "v0.2.0");
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout2, "ESP32-P4-Function-EV-Board");
    lv_obj_set_x( ui_LabelPanelPanelScreenSettingAbout2, 167 );

    char char_ui_version[20];
    snprintf(char_ui_version, sizeof(char_ui_version), "v%d.%d.%d", ESP_BROOKESIA_CONF_VER_MAJOR, ESP_BROOKESIA_CONF_VER_MINOR, ESP_BROOKESIA_CONF_VER_PATCH);
    lv_label_set_text(ui_LabelPanelPanelScreenSettingAbout6, char_ui_version);
}

void AppSettings::processWifiConnect(WifiConnectState_t state)
{
    if (_panel_wifi_connect == nullptr || _img_wifi_connect == nullptr || _spinner_wifi_connect == nullptr) {
        return;
    }

    switch (state) {
    case WIFI_CONNECT_HIDE:
        lv_obj_add_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        break;
    case WIFI_CONNECT_RUNNING:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        break;
    case WIFI_CONNECT_SUCCESS:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(_img_wifi_connect, &img_wifi_connect_success);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        break;
    case WIFI_CONNECT_FAIL:
        lv_obj_clear_flag(_panel_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_img_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(_img_wifi_connect, &img_wifi_connect_fail);
        lv_obj_add_flag(_spinner_wifi_connect, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }
}

bool AppSettings::loadNvsParam(void)
{
    esp_err_t err = ESP_OK;
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return false;
    }

    for (auto& key_value : _nvs_param_map) {
        err = nvs_get_i32(nvs_handle, key_value.first.c_str(), &key_value.second);
        switch (err) {
        case ESP_OK:
            ESP_LOGI(TAG, "Load %s: %d", key_value.first.c_str(), key_value.second);
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            err = nvs_set_i32(nvs_handle, key_value.first.c_str(), key_value.second);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) setting %s", esp_err_to_name(err), key_value.first.c_str());
            }
            ESP_LOGW(TAG, "The value of %s is not initialized yet, set it to default value: %d", key_value.first.c_str(),
                     key_value.second);
            break;
        default:
            break;
        }
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing NVS changes", esp_err_to_name(err));
        return false;
    }
    nvs_close(nvs_handle);

    return true;
}

bool AppSettings::setNvsParam(std::string key, int value)
{
    esp_err_t err = ESP_OK;
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_i32(nvs_handle, key.c_str(), value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) setting %s", esp_err_to_name(err), key.c_str());
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing NVS changes", esp_err_to_name(err));
        return false;
    }
    nvs_close(nvs_handle);

    return true;
}

void AppSettings::updateUiByNvsParam(void)
{
    if (_nvs_param_map[NVS_KEY_WIFI_ENABLE]) {
        lv_obj_add_state(ui_SwitchPanelScreenSettingWiFiSwitch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui_SwitchPanelScreenSettingWiFiSwitch, LV_STATE_CHECKED);
    }

    if (_nvs_param_map[NVS_KEY_BLE_ENABLE]) {
        lv_obj_add_state(ui_SwitchPanelScreenSettingBLESwitch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(ui_SwitchPanelScreenSettingBLESwitch, LV_STATE_CHECKED);
    }

    lv_slider_set_value(ui_SliderPanelScreenSettingLightSwitch1, _nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS], LV_ANIM_OFF);
    lv_slider_set_value(ui_SliderPanelScreenSettingVolumeSwitch, _nvs_param_map[NVS_KEY_AUDIO_VOLUME], LV_ANIM_OFF);
}

void AppSettings::requestWifiDiscovery(void)
{
    if (!_wifi_manager.startDiscovery()) {
        ESP_LOGW(TAG, "Failed to queue Wi-Fi discovery request");
    }
}

void AppSettings::endWifiDiscovery(void)
{
    _wifi_manager.endDiscovery();
}

AppSettings::WifiSignalStrengthLevel_t AppSettings::signalLevelFromRssi(int8_t rssi) const
{
    if (rssi > -60) {
        return WIFI_SIGNAL_STRENGTH_GOOD;
    }
    if (rssi > -80) {
        return WIFI_SIGNAL_STRENGTH_MODERATE;
    }
    if (rssi > -100) {
        return WIFI_SIGNAL_STRENGTH_WEAK;
    }
    return WIFI_SIGNAL_STRENGTH_NONE;
}

void AppSettings::initWifiListButton(lv_obj_t *ssid_label, lv_obj_t *lock_image, lv_obj_t *signal_image,
                                     lv_obj_t *connected_label, const char *ssid, bool psk,
                                     WifiSignalStrengthLevel_t signal_strength, bool connected)
{
    lv_label_set_text(ssid_label, ssid != nullptr ? ssid : "");

    if (psk) {
        lv_img_set_src(lock_image, &img_wifi_lock);
        lv_obj_clear_flag(lock_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lock_image, LV_OBJ_FLAG_HIDDEN);
    }

    if (connected) {
        lv_obj_clear_flag(connected_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(connected_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (signal_strength == WIFI_SIGNAL_STRENGTH_GOOD) {
        lv_img_set_src(signal_image, &img_wifisignal_good);
    } else if (signal_strength == WIFI_SIGNAL_STRENGTH_MODERATE) {
        lv_img_set_src(signal_image, &img_wifisignal_moderate);
    } else if (signal_strength == WIFI_SIGNAL_STRENGTH_WEAK) {
        lv_img_set_src(signal_image, &img_wifisignal_wake);
    } else {
        lv_img_set_src(signal_image, &img_wifisignal_absent);
    }
}

void AppSettings::deinitWifiListButton(void)
{
    _wifi_scan_count = 0;
    for (size_t i = 0; i < WifiConnectionManager::MAX_SCAN_RESULTS; ++i) {
        if (_wifi_panel_buttons[i] != nullptr) {
            lv_obj_add_flag(_wifi_panel_buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (_wifi_lock_images[i] != nullptr) {
            lv_obj_add_flag(_wifi_lock_images[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (_wifi_connected_labels[i] != nullptr) {
            lv_obj_add_flag(_wifi_connected_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void AppSettings::renderWifiList(const WifiConnectionManager::Snapshot &snapshot)
{
    uint32_t scan_version = snapshot.scan_version;
    size_t count = _wifi_manager.copyScanResults(_wifi_scan_results.data(), _wifi_scan_results.size(),
                                                  &scan_version);
    _last_wifi_scan_version = scan_version;

    if (snapshot.got_ip && snapshot.ssid[0] != '\0') {
        size_t connected_index = count;
        for (size_t i = 0; i < count; ++i) {
            if (strncmp(reinterpret_cast<const char *>(_wifi_scan_results[i].ssid), snapshot.ssid,
                        sizeof(_wifi_scan_results[i].ssid)) == 0) {
                connected_index = i;
                break;
            }
        }

        wifi_ap_record_t connected_record = {};
        if (connected_index < count) {
            connected_record = _wifi_scan_results[connected_index];
        } else {
            const size_t ssid_len = strnlen(snapshot.ssid, sizeof(connected_record.ssid) - 1);
            memcpy(connected_record.ssid, snapshot.ssid, ssid_len);
        }
        connected_record.authmode = snapshot.authmode;
        connected_record.rssi = snapshot.rssi;

        if (connected_index > 0 && connected_index < count) {
            memmove(&_wifi_scan_results[1], &_wifi_scan_results[0],
                    connected_index * sizeof(_wifi_scan_results[0]));
            _wifi_scan_results[0] = connected_record;
        } else if (connected_index == count) {
            const size_t move_count = (count < _wifi_scan_results.size()) ? count : count - 1;
            if (move_count > 0) {
                memmove(&_wifi_scan_results[1], &_wifi_scan_results[0],
                        move_count * sizeof(_wifi_scan_results[0]));
            }
            _wifi_scan_results[0] = connected_record;
            if (count < _wifi_scan_results.size()) {
                ++count;
            }
        } else {
            _wifi_scan_results[0] = connected_record;
        }
    }

    _wifi_scan_count = count;
    for (size_t i = 0; i < _wifi_panel_buttons.size(); ++i) {
        if (i >= count) {
            lv_obj_add_flag(_wifi_panel_buttons[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const wifi_ap_record_t &record = _wifi_scan_results[i];
        const char *ssid = reinterpret_cast<const char *>(record.ssid);
        const bool connected = snapshot.got_ip && strncmp(ssid, snapshot.ssid, sizeof(record.ssid)) == 0;
        const bool psk = record.authmode != WIFI_AUTH_OPEN && record.authmode != WIFI_AUTH_OWE;
        WifiSignalStrengthLevel_t signal = signalLevelFromRssi(record.rssi);
        if (connected && record.rssi <= -100) {
            signal = WIFI_SIGNAL_STRENGTH_MODERATE;
        }

        initWifiListButton(_wifi_ssid_labels[i], _wifi_lock_images[i], _wifi_signal_images[i],
                           _wifi_connected_labels[i], ssid, psk, signal, connected);
        lv_obj_clear_flag(_wifi_panel_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (count > 0 && snapshot.enabled && !snapshot.scanning) {
        lv_obj_clear_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::syncWifiUi(void)
{
    if (_is_ui_del) {
        return;
    }

    WifiConnectionManager::Snapshot snapshot = {};
    if (!_wifi_manager.getSnapshot(&snapshot)) {
        return;
    }

    const bool state_changed = snapshot.state_version != _last_wifi_state_version;
    const bool scan_changed = snapshot.scan_version != _last_wifi_scan_version;
    if (_wifi_refresh_btn != nullptr) {
        const bool refresh_disabled = !snapshot.initialized || !snapshot.enabled || snapshot.scanning ||
                                      snapshot.state == WifiConnectionManager::State::CONNECTING_CANDIDATE;
        if (refresh_disabled) {
            lv_obj_add_state(_wifi_refresh_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(_wifi_refresh_btn, LV_STATE_DISABLED);
        }
    }

    if (snapshot.scanning && _screen_index == UI_WIFI_SCAN_INDEX) {
        lv_obj_clear_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
    }

    if (!snapshot.enabled) {
        deinitWifiListButton();
        lv_obj_add_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
        _last_wifi_scan_version = snapshot.scan_version;
    } else if (state_changed || scan_changed) {
        renderWifiList(snapshot);
    }

    if (snapshot.state == WifiConnectionManager::State::CONNECTING_CANDIDATE &&
        _screen_index == UI_WIFI_CONNECT_INDEX &&
        _shown_wifi_result == WifiConnectionManager::ConnectResult::NONE) {
        processWifiConnect(WIFI_CONNECT_RUNNING);
    }

    if (_last_wifi_result_version == UINT32_MAX) {
        _last_wifi_result_version = snapshot.connect_result_version;
    } else if (snapshot.connect_result_version != _last_wifi_result_version) {
        _last_wifi_result_version = snapshot.connect_result_version;
        _shown_wifi_result = snapshot.connect_result;
        _wifi_result_shown_at = lv_tick_get();
        if (snapshot.connect_result == WifiConnectionManager::ConnectResult::SUCCESS) {
            processWifiConnect(WIFI_CONNECT_SUCCESS);
        } else if (snapshot.connect_result == WifiConnectionManager::ConnectResult::FAILED ||
                   snapshot.connect_result == WifiConnectionManager::ConnectResult::SAVE_FAILED) {
            processWifiConnect(WIFI_CONNECT_FAIL);
        } else {
            processWifiConnect(WIFI_CONNECT_HIDE);
        }
    }

    if (_shown_wifi_result != WifiConnectionManager::ConnectResult::NONE &&
        static_cast<uint32_t>(lv_tick_get() - _wifi_result_shown_at) >= WIFI_CONNECT_UI_WAIT_TIME_MS) {
        const bool connected = _shown_wifi_result == WifiConnectionManager::ConnectResult::SUCCESS;
        _shown_wifi_result = WifiConnectionManager::ConnectResult::NONE;
        processWifiConnect(WIFI_CONNECT_HIDE);
        if (connected && _screen_index == UI_WIFI_CONNECT_INDEX) {
            lv_scr_load(ui_ScreenSettingWiFi);
        }
    }

    _last_wifi_state_version = snapshot.state_version;
}

void AppSettings::euiRefresTask(void *arg)
{
    AppSettings *app = static_cast<AppSettings *>(arg);
    if (app == nullptr) {
        ESP_LOGE(TAG, "App instance is NULL");
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        WifiConnectionManager::Snapshot snapshot = {};
        app->_wifi_manager.getSnapshot(&snapshot);
        WifiSignalStrengthLevel_t signal = WIFI_SIGNAL_STRENGTH_NONE;
        if (snapshot.got_ip) {
            signal = app->signalLevelFromRssi(snapshot.rssi);
            if (signal == WIFI_SIGNAL_STRENGTH_NONE) {
                signal = WIFI_SIGNAL_STRENGTH_MODERATE;
            }
            if (!s_sntp_started) {
                s_sntp_started = true;
                BaseType_t created = xTaskCreate(sntpInitTask, "SNTP Init", SNTP_TASK_STACK_SIZE, nullptr,
                                                 SNTP_TASK_PRIORITY, nullptr);
                if (created != pdPASS) {
                    ESP_LOGW(TAG, "Failed to create SNTP init task");
                    s_sntp_started = false;
                }
            }
        }
        app->_wifi_signal_strength_level = signal;

        bool backstage_visible = false;
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (app->status_bar != nullptr) {
                if (!app->status_bar->setClock(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_hour >= 12)) {
                    ESP_LOGE(TAG, "Set clock failed");
                }
                app->status_bar->setWifiIconState(static_cast<int>(signal));
            }
            if (app->backstage != nullptr) {
                backstage_visible = app->backstage->checkVisible();
            }
            esp_lv_adapter_unlock();
        }

        if (backstage_visible) {
            const uint16_t free_sram_size_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
            const uint16_t total_sram_size_kb = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024;
            const uint16_t free_psram_size_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
            const uint16_t total_psram_size_kb = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024;
            ESP_LOGI(TAG, "Free sram size: %d KB, total sram size: %d KB, "
                     "free psram size: %d KB, total psram size: %d KB",
                     free_sram_size_kb, total_sram_size_kb, free_psram_size_kb, total_psram_size_kb);

            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                if (app->backstage != nullptr && app->backstage->checkVisible() &&
                    !app->backstage->setMemoryLabel(free_sram_size_kb, total_sram_size_kb,
                                                    free_psram_size_kb, total_psram_size_kb)) {
                    ESP_LOGE(TAG, "Update memory usage failed");
                }
                esp_lv_adapter_unlock();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(HOME_REFRESH_TASK_PERIOD_MS));
    }
}

void AppSettings::wifiUiTimerCallback(lv_timer_t *timer)
{
    AppSettings *app = static_cast<AppSettings *>(timer != nullptr ? timer->user_data : nullptr);
    if (app != nullptr) {
        app->syncWifiUi();
    }
}

void AppSettings::onWifiRefreshClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = static_cast<AppSettings *>(lv_event_get_user_data(e));
    if (app != nullptr && !app->_is_ui_del) {
        app->requestWifiDiscovery();
    }
}

void AppSettings::setVerificationKeyboardVisible(bool visible)
{
    if (ui_KeyboardScreenSettingVerification != NULL) {
        if (visible) {
            lv_obj_clear_flag(ui_KeyboardScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_KeyboardScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!visible && ui_TextAreaScreenSettingVerificationPassword != NULL) {
        lv_obj_clear_state(ui_TextAreaScreenSettingVerificationPassword, LV_STATE_FOCUSED);
    }
}

void AppSettings::onTextAreaScreenSettingVerificationPasswordClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    bool keyboard_hidden = false;

    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (ui_KeyboardScreenSettingVerification == NULL ||
        ui_TextAreaScreenSettingVerificationPassword == NULL) {
        goto end;
    }

    lv_keyboard_set_textarea(ui_KeyboardScreenSettingVerification, ui_TextAreaScreenSettingVerificationPassword);
    keyboard_hidden = lv_obj_has_flag(ui_KeyboardScreenSettingVerification, LV_OBJ_FLAG_HIDDEN);
    app->setVerificationKeyboardVisible(keyboard_hidden);

end:
    return;
}

void AppSettings::onKeyboardScreenSettingVerificationClickedEventCallback(lv_event_t *e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);

    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");
    ESP_BROOKESIA_CHECK_NULL_GOTO(target, end, "Invalid target pointer");

    if (ui_TextAreaScreenSettingVerificationPassword == NULL) {
        goto end;
    }

    lv_keyboard_set_textarea(target, ui_TextAreaScreenSettingVerificationPassword);

    if(lv_keyboard_get_selected_btn(target) == 39) {
        const char *ssid = lv_label_get_text(ui_LabelScreenSettingVerificationSSID);
        const char *password = lv_textarea_get_text(ui_TextAreaScreenSettingVerificationPassword);

        app->setVerificationKeyboardVisible(false);
        app->processWifiConnect(WIFI_CONNECT_RUNNING);
        const bool queued = app->_wifi_manager.connectCandidate(ssid != nullptr ? ssid : "",
                                                                 password != nullptr ? password : "");
        lv_textarea_set_text(ui_TextAreaScreenSettingVerificationPassword, "");
        if (!queued) {
            app->_shown_wifi_result = WifiConnectionManager::ConnectResult::FAILED;
            app->_wifi_result_shown_at = lv_tick_get();
            app->processWifiConnect(WIFI_CONNECT_FAIL);
            ESP_LOGW(TAG, "Failed to queue candidate Wi-Fi connection");
        }
    }

end:
    return;
}

void AppSettings::onScreenLoadEventCallback( lv_event_t * e)
{
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }
    SettingScreenIndex_t last_scr_index = app->_screen_index;

    for (int i = 0; i < UI_MAX_INDEX; i++) {
        if (app->_screen_list[i] == lv_event_get_target(e)) {
            app->_screen_index = (SettingScreenIndex_t)i;
            break;
        }
    }

    const bool was_wifi_screen = last_scr_index == UI_WIFI_SCAN_INDEX || last_scr_index == UI_WIFI_CONNECT_INDEX;
    const bool is_wifi_screen = app->_screen_index == UI_WIFI_SCAN_INDEX ||
                                app->_screen_index == UI_WIFI_CONNECT_INDEX;
    if (was_wifi_screen && !is_wifi_screen) {
        app->endWifiDiscovery();
    }

    if (app->_screen_index == UI_WIFI_SCAN_INDEX && app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] != 0) {
        WifiConnectionManager::Snapshot snapshot = {};
        const bool has_snapshot = app->_wifi_manager.getSnapshot(&snapshot);
        if (!has_snapshot || (!snapshot.got_ip && !snapshot.discovery_active &&
                              snapshot.state != WifiConnectionManager::State::CONNECTING_CANDIDATE)) {
            app->requestWifiDiscovery();
        }
        app->syncWifiUi();
    }
}

void AppSettings::onSwitchPanelScreenSettingWiFiSwitchValueChangeEventCallback( lv_event_t * e) {
    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    if (app == nullptr) {
        ESP_LOGE(TAG, "Invalid app pointer");
        return;
    }
    lv_state_t state = lv_obj_get_state(ui_SwitchPanelScreenSettingWiFiSwitch);
    const bool enabled = (state & LV_STATE_CHECKED) != 0;

    app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = enabled;
    app->setNvsParam(NVS_KEY_WIFI_ENABLE, enabled ? 1 : 0);
    if (!app->_wifi_manager.setEnabled(enabled)) {
        ESP_LOGW(TAG, "Failed to queue Wi-Fi enabled state: enabled=%d", enabled);
    }

    if (enabled) {
        if (app->_screen_index == UI_WIFI_SCAN_INDEX) {
            app->requestWifiDiscovery();
        }
    } else {
        app->deinitWifiListButton();
        lv_obj_add_flag(ui_PanelScreenSettingWiFiList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_SpinnerScreenSettingWiFi, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::onButtonWifiListClickedEventCallback(lv_event_t * e)
{
    AppSettings *app = static_cast<AppSettings *>(lv_event_get_user_data(e));
    lv_obj_t *btn = lv_event_get_target(e);
    if (app == nullptr || btn == nullptr) {
        return;
    }

    size_t index = app->_wifi_panel_buttons.size();
    for (size_t i = 0; i < app->_wifi_panel_buttons.size(); ++i) {
        if (app->_wifi_panel_buttons[i] == btn) {
            index = i;
            break;
        }
    }
    if (index >= app->_wifi_scan_count) {
        return;
    }

    const char *ssid = reinterpret_cast<const char *>(app->_wifi_scan_results[index].ssid);
    if (ssid[0] == '\0') {
        return;
    }
    WifiConnectionManager::Snapshot snapshot = {};
    if (app->_wifi_manager.getSnapshot(&snapshot) && snapshot.got_ip &&
        strncmp(ssid, snapshot.ssid, sizeof(snapshot.ssid)) == 0) {
        return;
    }

    app->processWifiConnect(WIFI_CONNECT_HIDE);
    app->setVerificationKeyboardVisible(false);
    lv_textarea_set_text(ui_TextAreaScreenSettingVerificationPassword, "");
    lv_label_set_text(ui_LabelScreenSettingVerificationSSID, ssid);
    lv_scr_load(ui_ScreenSettingVerification);
}

void AppSettings::onSwitchPanelScreenSettingBLESwitchValueChangeEventCallback( lv_event_t * e) {
    lv_state_t state = lv_obj_get_state(ui_SwitchPanelScreenSettingBLESwitch);

    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (state & LV_STATE_CHECKED) {
        app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = true;
        app->setNvsParam(NVS_KEY_BLE_ENABLE, 1);
    } else {
        app->_nvs_param_map[NVS_KEY_WIFI_ENABLE] = false;
        app->setNvsParam(NVS_KEY_BLE_ENABLE, 0);
    }

end:
    return;
}

void AppSettings::onSliderPanelVolumeSwitchValueChangeEventCallback( lv_event_t * e) {
    int volume = lv_slider_get_value(ui_SliderPanelScreenSettingVolumeSwitch);

    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (volume != app->_nvs_param_map[NVS_KEY_AUDIO_VOLUME]) {
        if ((bsp_extra_codec_volume_set(volume, NULL) != ESP_OK) && (bsp_extra_codec_volume_get() != volume)) {
            ESP_LOGE(TAG, "Set volume failed");
            lv_slider_set_value(ui_SliderPanelScreenSettingVolumeSwitch, app->_nvs_param_map[NVS_KEY_AUDIO_VOLUME], LV_ANIM_OFF);
            return;
        }
        app->_nvs_param_map[NVS_KEY_AUDIO_VOLUME] = volume;
        app->setNvsParam(NVS_KEY_AUDIO_VOLUME, volume);
    }

end:
    return;
}

void AppSettings::onSliderPanelLightSwitchValueChangeEventCallback( lv_event_t * e) {
    brightness = lv_slider_get_value(ui_SliderPanelScreenSettingLightSwitch1);

    AppSettings *app = (AppSettings *)lv_event_get_user_data(e);
    ESP_BROOKESIA_CHECK_NULL_GOTO(app, end, "Invalid app pointer");

    if (brightness != app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS]) {
        // if ((bsp_display_brightness_set(brightness) != ESP_OK) && (bsp_display_brightness_get() != brightness)) {
        if (bsp_display_brightness_set(brightness) != ESP_OK) {
            ESP_LOGE(TAG, "Set brightness failed");
            lv_slider_set_value(ui_SliderPanelScreenSettingLightSwitch1, app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS], LV_ANIM_OFF);
            return;
        }
        app->_nvs_param_map[NVS_KEY_DISPLAY_BRIGHTNESS] = brightness;
        app->setNvsParam(NVS_KEY_DISPLAY_BRIGHTNESS, brightness);
    }

end:
    return;
}
