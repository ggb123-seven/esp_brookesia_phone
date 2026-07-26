/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

class EnvironmentMonitorApp: public ESP_Brookesia_PhoneApp {
public:
    EnvironmentMonitorApp();
    ~EnvironmentMonitorApp();

    bool run(void);
    bool back(void);
    bool close(void);
    bool init(void) override;

private:
    void buildUi(void);
    void updateUi(void);
    bool startRefreshTimer(void);
    void stopRefreshTimer(void);
    lv_obj_t *createMetricPanel(lv_obj_t *parent, const char *title);
    lv_obj_t *createMetricLabel(lv_obj_t *parent, const char *text, uint32_t color);
    void applyLabelStyle(lv_obj_t *label, uint32_t color, const lv_font_t *font);
    void startVisualAnimations(void);
    void stopVisualAnimations(void);

    static void refreshTimerCallback(lv_timer_t *timer);
    static void pulseAnimCallback(void *obj, int32_t value);

    volatile bool _closing;
    lv_timer_t *_refresh_timer;

    lv_obj_t *_root;
    lv_obj_t *_temperature_arc;
    lv_obj_t *_dht_temperature_label;
    lv_obj_t *_humidity_bar;
    lv_obj_t *_dht_humidity_label;
    lv_obj_t *_gas_pulse;
    lv_obj_t *_mq2_level_label;
    lv_obj_t *_mq2_status_label;
};
