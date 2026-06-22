/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "EnvironmentMonitorApp.hpp"

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_EXAMPLE_ENABLE_DHT11_SERVICE
#include "dht11_service.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_MQ2_SERVICE
#include "mq2_service.h"
#endif

#define ENV_MONITOR_REFRESH_MS            (1000)
#define ENV_MONITOR_COLOR_BG              0x06141B
#define ENV_MONITOR_COLOR_PANEL           0x102631
#define ENV_MONITOR_COLOR_PANEL_SECONDARY 0x0D202B
#define ENV_MONITOR_COLOR_STROKE          0x244553
#define ENV_MONITOR_COLOR_TEXT            0xF8FAFC
#define ENV_MONITOR_COLOR_MUTED           0xA7B4C2
#define ENV_MONITOR_COLOR_OK              0x36D399
#define ENV_MONITOR_COLOR_WARN            0xFBBF24
#define ENV_MONITOR_COLOR_ERROR           0xF87171
#define ENV_MONITOR_COLOR_TEMP            0xFFB454
#define ENV_MONITOR_COLOR_HUMIDITY        0x38BDF8
#define ENV_MONITOR_COLOR_GAS             0xA78BFA
#define ENV_MONITOR_FONT_CN               (&environment_font_20)
#define ENV_MONITOR_FONT_VALUE            (&lv_font_montserrat_48)
#define ENV_MONITOR_FONT_SUBVALUE         (&lv_font_montserrat_34)

LV_FONT_DECLARE(environment_font_20);
LV_IMG_DECLARE(img_app_environment_monitor);

static const char *TAG = "EnvironmentMonitor";

[[maybe_unused]] static const char *mq2_state_to_text(int state)
{
#if CONFIG_EXAMPLE_ENABLE_MQ2_SERVICE
    switch ((mq2_service_state_t)state) {
    case MQ2_SERVICE_STATE_WARMING_UP:
        return "预热中";
    case MQ2_SERVICE_STATE_NORMAL:
        return "正常";
    case MQ2_SERVICE_STATE_ALARM:
        return "告警";
    case MQ2_SERVICE_STATE_SENSOR_ERROR:
        return "异常";
    default:
        return "--";
    }
#else
    (void)state;
    return "--";
#endif
}

EnvironmentMonitorApp::EnvironmentMonitorApp():
    ESP_Brookesia_PhoneApp("环境监测", &img_app_environment_monitor, true),
    _closing(false),
    _refresh_timer(NULL),
    _root(NULL),
    _temperature_arc(NULL),
    _dht_temperature_label(NULL),
    _humidity_bar(NULL),
    _dht_humidity_label(NULL),
    _gas_pulse(NULL),
    _mq2_level_label(NULL),
    _mq2_status_label(NULL)
{
}

EnvironmentMonitorApp::~EnvironmentMonitorApp()
{
}

bool EnvironmentMonitorApp::init(void)
{
    return true;
}

bool EnvironmentMonitorApp::run(void)
{
    _closing = false;
    buildUi();
    startVisualAnimations();
    updateUi();

    if (!startRefreshTimer()) {
        ESP_LOGE(TAG, "Failed to start environment monitor refresh timer");
    }

    return true;
}

bool EnvironmentMonitorApp::back(void)
{
    notifyCoreClosed();
    return true;
}

bool EnvironmentMonitorApp::close(void)
{
    stopRefreshTimer();
    stopVisualAnimations();

    _root = NULL;
    _temperature_arc = NULL;
    _dht_temperature_label = NULL;
    _humidity_bar = NULL;
    _dht_humidity_label = NULL;
    _gas_pulse = NULL;
    _mq2_level_label = NULL;
    _mq2_status_label = NULL;

    return true;
}

bool EnvironmentMonitorApp::startRefreshTimer(void)
{
    if (_refresh_timer != NULL) {
        return true;
    }

    _refresh_timer = lv_timer_create(refreshTimerCallback, ENV_MONITOR_REFRESH_MS, this);
    if (_refresh_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create refresh timer");
        return false;
    }

    return true;
}

void EnvironmentMonitorApp::stopRefreshTimer(void)
{
    _closing = true;
    if (_refresh_timer != NULL) {
        lv_timer_del(_refresh_timer);
        _refresh_timer = NULL;
    }
    _closing = false;
}

void EnvironmentMonitorApp::buildUi(void)
{
    lv_area_t area = getVisualArea();
    const lv_coord_t width = area.x2 - area.x1;
    const lv_coord_t height = area.y2 - area.y1;

    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root, width, height);
    lv_obj_align(_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(ENV_MONITOR_COLOR_BG), 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 18, 0);
    lv_obj_set_style_pad_column(_root, 14, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *temperature_panel = createMetricPanel(_root, "温度");
    lv_obj_set_width(temperature_panel, 0);
    lv_obj_set_flex_grow(temperature_panel, 2);
    lv_obj_set_height(temperature_panel, LV_PCT(100));

    lv_obj_t *side = lv_obj_create(_root);
    lv_obj_set_width(side, 0);
    lv_obj_set_flex_grow(side, 1);
    lv_obj_set_height(side, LV_PCT(100));
    lv_obj_set_style_bg_opa(side, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(side, 0, 0);
    lv_obj_set_style_pad_all(side, 0, 0);
    lv_obj_set_style_pad_row(side, 14, 0);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *humidity_panel = createMetricPanel(side, "湿度");
    lv_obj_set_width(humidity_panel, LV_PCT(100));
    lv_obj_set_height(humidity_panel, 0);
    lv_obj_set_flex_grow(humidity_panel, 1);

    lv_obj_t *gas_panel = createMetricPanel(side, "气体");
    lv_obj_set_width(gas_panel, LV_PCT(100));
    lv_obj_set_height(gas_panel, 0);
    lv_obj_set_flex_grow(gas_panel, 1);

    _temperature_arc = lv_arc_create(temperature_panel);
    lv_obj_set_size(_temperature_arc, 232, 232);
    lv_arc_set_bg_angles(_temperature_arc, 135, 45);
    lv_arc_set_range(_temperature_arc, -10, 50);
    lv_arc_set_value(_temperature_arc, 0);
    lv_obj_remove_style(_temperature_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(_temperature_arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_temperature_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(_temperature_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(_temperature_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_temperature_arc, lv_color_hex(ENV_MONITOR_COLOR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(_temperature_arc, lv_color_hex(ENV_MONITOR_COLOR_TEMP), LV_PART_INDICATOR);
    lv_obj_align(_temperature_arc, LV_ALIGN_CENTER, 0, 8);

    _dht_temperature_label = createMetricLabel(temperature_panel, "--.-°C", ENV_MONITOR_COLOR_TEXT);
    lv_obj_set_style_text_font(_dht_temperature_label, ENV_MONITOR_FONT_VALUE, 0);
    lv_obj_set_style_text_align(_dht_temperature_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_dht_temperature_label, LV_ALIGN_CENTER, 0, 4);

    _humidity_bar = lv_bar_create(humidity_panel);
    lv_obj_set_size(_humidity_bar, LV_PCT(84), 24);
    lv_bar_set_range(_humidity_bar, 0, 100);
    lv_bar_set_value(_humidity_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(_humidity_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(_humidity_bar, 12, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_humidity_bar, lv_color_hex(ENV_MONITOR_COLOR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_humidity_bar, lv_color_hex(ENV_MONITOR_COLOR_HUMIDITY), LV_PART_INDICATOR);
    lv_obj_align(_humidity_bar, LV_ALIGN_BOTTOM_MID, 0, -22);

    _dht_humidity_label = createMetricLabel(humidity_panel, "--.-%", ENV_MONITOR_COLOR_TEXT);
    lv_obj_set_style_text_font(_dht_humidity_label, ENV_MONITOR_FONT_SUBVALUE, 0);
    lv_obj_set_style_text_align(_dht_humidity_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_dht_humidity_label, LV_ALIGN_CENTER, 0, -6);

    _gas_pulse = lv_obj_create(gas_panel);
    lv_obj_set_size(_gas_pulse, 112, 112);
    lv_obj_set_style_radius(_gas_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(_gas_pulse, 4, 0);
    lv_obj_set_style_border_color(_gas_pulse, lv_color_hex(ENV_MONITOR_COLOR_GAS), 0);
    lv_obj_set_style_bg_color(_gas_pulse, lv_color_hex(ENV_MONITOR_COLOR_GAS), 0);
    lv_obj_set_style_bg_opa(_gas_pulse, LV_OPA_20, 0);
    lv_obj_align(_gas_pulse, LV_ALIGN_CENTER, 0, 8);

    _mq2_status_label = createMetricLabel(gas_panel, "--", ENV_MONITOR_COLOR_TEXT);
    lv_obj_set_style_text_font(_mq2_status_label, ENV_MONITOR_FONT_SUBVALUE, 0);
    lv_obj_set_style_text_align(_mq2_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_mq2_status_label, LV_ALIGN_CENTER, 0, -6);

    _mq2_level_label = createMetricLabel(gas_panel, "--", ENV_MONITOR_COLOR_MUTED);
    lv_obj_set_style_text_align(_mq2_level_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_mq2_level_label, LV_ALIGN_CENTER, 0, 36);
}

lv_obj_t *EnvironmentMonitorApp::createMetricPanel(lv_obj_t *parent, const char *title)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_style_bg_color(panel, lv_color_hex(ENV_MONITOR_COLOR_PANEL), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x183847), 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, title);
    applyLabelStyle(label, ENV_MONITOR_COLOR_MUTED, ENV_MONITOR_FONT_CN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    return panel;
}

lv_obj_t *EnvironmentMonitorApp::createMetricLabel(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text);
    applyLabelStyle(label, color, ENV_MONITOR_FONT_CN);
    return label;
}

void EnvironmentMonitorApp::applyLabelStyle(lv_obj_t *label, uint32_t color, const lv_font_t *font)
{
    if (label == NULL) {
        return;
    }

    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

void EnvironmentMonitorApp::startVisualAnimations(void)
{
    if (_gas_pulse != NULL) {
        lv_anim_t pulse_anim;
        lv_anim_init(&pulse_anim);
        lv_anim_set_var(&pulse_anim, _gas_pulse);
        lv_anim_set_exec_cb(&pulse_anim, pulseAnimCallback);
        lv_anim_set_values(&pulse_anim, 98, 126);
        lv_anim_set_time(&pulse_anim, 1200);
        lv_anim_set_playback_time(&pulse_anim, 1200);
        lv_anim_set_repeat_count(&pulse_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&pulse_anim);
    }
}

void EnvironmentMonitorApp::stopVisualAnimations(void)
{
    if (_gas_pulse != NULL) {
        lv_anim_del(_gas_pulse, pulseAnimCallback);
    }
}

void EnvironmentMonitorApp::updateUi(void)
{
    if (_root == NULL || _closing) {
        return;
    }

    char text[32];

#if CONFIG_EXAMPLE_ENABLE_DHT11_SERVICE
    dht11_service_snapshot_t dht_snapshot = {};
    esp_err_t dht_err = dht11_service_get_snapshot(&dht_snapshot);
    if ((dht_err == ESP_OK) && dht_snapshot.valid && (dht_snapshot.status == DHT11_SERVICE_STATUS_OK)) {
        snprintf(text, sizeof(text), "%.1f°C", (double)dht_snapshot.temperature_c);
        lv_label_set_text(_dht_temperature_label, text);
        lv_arc_set_value(_temperature_arc, (int32_t)dht_snapshot.temperature_c);
        lv_obj_set_style_text_color(_dht_temperature_label, lv_color_hex(ENV_MONITOR_COLOR_TEXT), 0);

        snprintf(text, sizeof(text), "%.1f%%", (double)dht_snapshot.humidity_percent);
        lv_label_set_text(_dht_humidity_label, text);
        lv_bar_set_value(_humidity_bar, (int32_t)dht_snapshot.humidity_percent, LV_ANIM_ON);
        lv_obj_set_style_text_color(_dht_humidity_label, lv_color_hex(ENV_MONITOR_COLOR_TEXT), 0);
    } else {
        lv_label_set_text(_dht_temperature_label, "--.-°C");
        lv_arc_set_value(_temperature_arc, 0);
        lv_obj_set_style_text_color(_dht_temperature_label, lv_color_hex(ENV_MONITOR_COLOR_MUTED), 0);

        lv_label_set_text(_dht_humidity_label, "--.-%");
        lv_bar_set_value(_humidity_bar, 0, LV_ANIM_ON);
        lv_obj_set_style_text_color(_dht_humidity_label, lv_color_hex(ENV_MONITOR_COLOR_MUTED), 0);
    }
#else
    lv_label_set_text(_dht_temperature_label, "--.-°C");
    lv_label_set_text(_dht_humidity_label, "--.-%");
    lv_arc_set_value(_temperature_arc, 0);
    lv_bar_set_value(_humidity_bar, 0, LV_ANIM_ON);
#endif

#if CONFIG_EXAMPLE_ENABLE_MQ2_SERVICE
    mq2_service_snapshot_t mq2_snapshot = {};
    esp_err_t mq2_err = mq2_service_get_snapshot(&mq2_snapshot);
    if (mq2_err == ESP_OK) {
        const bool alarm = mq2_snapshot.state == MQ2_SERVICE_STATE_ALARM;
        const bool sensor_error = mq2_snapshot.state == MQ2_SERVICE_STATE_SENSOR_ERROR;
        const uint32_t gas_color = alarm || sensor_error ? ENV_MONITOR_COLOR_ERROR :
                                   mq2_snapshot.state == MQ2_SERVICE_STATE_NORMAL ? ENV_MONITOR_COLOR_OK :
                                   ENV_MONITOR_COLOR_WARN;

        lv_label_set_text(_mq2_status_label, mq2_state_to_text(mq2_snapshot.state));
        lv_obj_set_style_text_color(_mq2_status_label, lv_color_hex(gas_color), 0);
        lv_obj_set_style_border_color(_gas_pulse, lv_color_hex(gas_color), 0);
        lv_obj_set_style_bg_color(_gas_pulse, lv_color_hex(gas_color), 0);

        snprintf(text, sizeof(text), "%d", mq2_snapshot.digital_level);
        lv_label_set_text(_mq2_level_label, text);
    } else {
        lv_label_set_text(_mq2_status_label, "--");
        lv_obj_set_style_text_color(_mq2_status_label, lv_color_hex(ENV_MONITOR_COLOR_MUTED), 0);
        lv_obj_set_style_border_color(_gas_pulse, lv_color_hex(ENV_MONITOR_COLOR_GAS), 0);
        lv_obj_set_style_bg_color(_gas_pulse, lv_color_hex(ENV_MONITOR_COLOR_GAS), 0);
        lv_label_set_text(_mq2_level_label, "--");
    }
#else
    lv_label_set_text(_mq2_status_label, "--");
    lv_label_set_text(_mq2_level_label, "--");
#endif
}

void EnvironmentMonitorApp::refreshTimerCallback(lv_timer_t *timer)
{
    EnvironmentMonitorApp *app = static_cast<EnvironmentMonitorApp *>(timer != NULL ? timer->user_data : NULL);
    if (app == NULL || app->_closing || app->_root == NULL) {
        return;
    }

    app->updateUi();
}

void EnvironmentMonitorApp::pulseAnimCallback(void *obj, int32_t value)
{
    lv_obj_t *pulse = static_cast<lv_obj_t *>(obj);
    lv_obj_set_size(pulse, value, value);
    lv_obj_set_style_bg_opa(pulse, (lv_opa_t)(LV_OPA_30 - ((value - 98) / 3)), 0);
    lv_obj_align(pulse, LV_ALIGN_CENTER, 0, 8);
}
