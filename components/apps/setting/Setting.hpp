/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <map>
#include <string>
#include "esp_wifi.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "WifiConnectionManager.hpp"

class AppSettings: public ESP_Brookesia_PhoneApp {
public:
    AppSettings();
    ~AppSettings();

    bool run(void);
    bool back(void);
    bool close(void);

    bool init(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    typedef enum {
        UI_MAIN_SETTING_INDEX = 0,
        UI_WIFI_SCAN_INDEX,
        UI_WIFI_CONNECT_INDEX,
        UI_BLUETOOTH_SETTING_INDEX,
        UI_VOLUME_SETTING_INDEX,
        UI_BRIGHTNESS_SETTING_INDEX,
        UI_ABOUT_SETTING_INDEX,
        UI_MAX_INDEX,
    } SettingScreenIndex_t;

    typedef enum {
        WIFI_SIGNAL_STRENGTH_NONE = 0,
        WIFI_SIGNAL_STRENGTH_WEAK = 1,
        WIFI_SIGNAL_STRENGTH_MODERATE = 2,
        WIFI_SIGNAL_STRENGTH_GOOD = 3,
    } WifiSignalStrengthLevel_t;

    typedef enum {
        WIFI_CONNECT_HIDE = 0,
        WIFI_CONNECT_RUNNING,
        WIFI_CONNECT_SUCCESS,
        WIFI_CONNECT_FAIL,
    } WifiConnectState_t;

    /* Operations */
    // UI
    void extraUiInit(void);
    void processWifiConnect(WifiConnectState_t state);
    void initWifiListButton(lv_obj_t* lv_label_ssid, lv_obj_t* lv_img_wifi_lock, lv_obj_t* lv_wifi_img,
                            lv_obj_t *lv_wifi_connect, const char *ssid, bool psk,
                            WifiSignalStrengthLevel_t signal_strength, bool connected);
    void deinitWifiListButton(void);
    void requestWifiDiscovery(void);
    void endWifiDiscovery(void);
    void syncWifiUi(void);
    void renderWifiList(const WifiConnectionManager::Snapshot &snapshot);
    WifiSignalStrengthLevel_t signalLevelFromRssi(int8_t rssi) const;
    // NVS Parameters
    bool loadNvsParam(void);
    bool setNvsParam(std::string key, int value);
    void updateUiByNvsParam(void);
    // Smart Gadget
    // void updateGadgetTime(struct tm timeinfo);

    /* Task */
    static void euiRefresTask(void *arg);

    /* UI Event Callback */
    // Main
    static void onScreenLoadEventCallback( lv_event_t * e);
    // WiFi
    static void onSwitchPanelScreenSettingWiFiSwitchValueChangeEventCallback( lv_event_t * e);
    static void onButtonWifiListClickedEventCallback(lv_event_t * e);
    static void onTextAreaScreenSettingVerificationPasswordClickedEventCallback(lv_event_t *e);
    static void onKeyboardScreenSettingVerificationClickedEventCallback(lv_event_t *e);
    static void onWifiRefreshClickedEventCallback(lv_event_t *e);
    static void wifiUiTimerCallback(lv_timer_t *timer);
    // Bluetooth
    static void onSwitchPanelScreenSettingBLESwitchValueChangeEventCallback( lv_event_t * e);
    // Audio
    static void onSliderPanelVolumeSwitchValueChangeEventCallback( lv_event_t * e);
    // Brightness
    static void onSliderPanelLightSwitchValueChangeEventCallback( lv_event_t * e);

    bool _is_ui_resumed;
    bool _is_ui_del;
    SettingScreenIndex_t _screen_index;
    WifiSignalStrengthLevel_t _wifi_signal_strength_level;
    WifiConnectionManager _wifi_manager;
    lv_obj_t *_panel_wifi_connect;
    lv_obj_t *_spinner_wifi_connect;
    lv_obj_t *_img_wifi_connect;
    lv_obj_t *_wifi_refresh_btn;
    lv_timer_t *_wifi_ui_timer;
    std::array<lv_obj_t *, UI_MAX_INDEX> _screen_list;
    std::array<lv_obj_t *, WifiConnectionManager::MAX_SCAN_RESULTS> _wifi_panel_buttons;
    std::array<lv_obj_t *, WifiConnectionManager::MAX_SCAN_RESULTS> _wifi_ssid_labels;
    std::array<lv_obj_t *, WifiConnectionManager::MAX_SCAN_RESULTS> _wifi_lock_images;
    std::array<lv_obj_t *, WifiConnectionManager::MAX_SCAN_RESULTS> _wifi_signal_images;
    std::array<lv_obj_t *, WifiConnectionManager::MAX_SCAN_RESULTS> _wifi_connected_labels;
    std::array<wifi_ap_record_t, WifiConnectionManager::MAX_SCAN_RESULTS> _wifi_scan_results;
    std::map<std::string, int32_t> _nvs_param_map;
    size_t _wifi_scan_count;
    uint32_t _last_wifi_state_version;
    uint32_t _last_wifi_scan_version;
    uint32_t _last_wifi_result_version;
    uint32_t _wifi_result_shown_at;
    WifiConnectionManager::ConnectResult _shown_wifi_result;

    void setVerificationKeyboardVisible(bool visible);
    const ESP_Brookesia_StatusBar *status_bar; 
    const ESP_Brookesia_RecentsScreen *backstage;
};
