/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "esp_err.h"

class ClassroomScheduleApp: public ESP_Brookesia_PhoneApp {
public:
    ClassroomScheduleApp();
    ~ClassroomScheduleApp();

    bool init(void) override;
    bool run(void);
    bool back(void);
    bool close(void);

private:
    enum ViewState {
        VIEW_NO_CLASSROOM,
        VIEW_IDLE,
        VIEW_LOADING,
        VIEW_READY,
        VIEW_CACHED,
        VIEW_EMPTY,
        VIEW_ERROR,
    };

    struct Course {
        char start[8];
        char end[8];
        char name[96];
        char teacher[64];
        char group[64];
    };

    struct ScheduleData {
        char date[16];
        char classroom[64];
        char classroom_name[96];
        char updated_at[40];
        Course courses[16];
        size_t course_count;
        bool from_cache;
    };

    struct QuerySnapshot {
        uint32_t generation;
        char classroom[64];
        char date[16];
        char server_host[96];
    };

    struct RefreshResult {
        esp_err_t err;
        int http_status;
        bool used_cache;
        bool has_data;
        QuerySnapshot query;
        ScheduleData data;
        char detail[160];
    };

    struct CatalogName {
        char value[64];
    };

    enum CatalogRequestType {
        CATALOG_LOAD_ALL,
        CATALOG_LOAD_ROOMS,
    };

    struct CatalogRequest {
        CatalogRequestType type;
        uint32_t generation;
        char server_host[96];
        char building[64];
        char saved_classroom[64];
    };

    struct CatalogResult {
        esp_err_t err;
        int http_status;
        CatalogRequest request;
        CatalogName *buildings;
        size_t building_count;
        CatalogName *rooms;
        size_t room_count;
        size_t selected_building;
        size_t selected_room;
        bool saved_classroom_valid;
        char detail[160];
    };

    enum PendingRequestType {
        PENDING_NONE,
        PENDING_CATALOG,
        PENDING_SCHEDULE,
    };

    bool loadClassroom(void);
    bool loadBuilding(void);
    bool saveSelection(const char *building, const char *classroom);
    bool loadServerHost(void);
    bool saveServerHost(const char *server_host);
    bool loadCacheUpdatedAt(char *updated_at, size_t updated_at_size);
    bool saveCacheUpdatedAt(const char *updated_at);
    bool loadCacheServerHost(char *server_host, size_t server_host_size);
    bool saveCacheServerHost(const char *server_host);
    void invalidateCacheServerHost(void);
    esp_err_t loadCacheJson(char *buffer, size_t buffer_size, size_t *json_len);
    esp_err_t loadCacheJsonFromSpiffs(char *buffer, size_t buffer_size, size_t *json_len);
    esp_err_t saveCacheJson(const char *json);
    esp_err_t loadCacheJsonFromNvs(char *buffer, size_t buffer_size, size_t *json_len);
    esp_err_t saveCacheJsonToNvs(const char *json);
    esp_err_t fetchScheduleJson(const QuerySnapshot &query, char *buffer, size_t buffer_size, size_t *json_len,
                                int *http_status);
    esp_err_t fetchCatalogJson(const CatalogRequest &request, const char *path, const char *building,
                               char *buffer, size_t buffer_size, size_t *json_len, int *http_status);
    esp_err_t parseBuildingsJson(const char *json, size_t json_len, CatalogName **buildings, size_t *count);
    esp_err_t parseRoomsJson(const char *json, size_t json_len, const char *expected_building,
                             CatalogName **rooms, size_t *count);
    esp_err_t parseScheduleJson(const char *json, size_t json_len, ScheduleData *data);
    esp_err_t parseCachedScheduleJson(const char *json, size_t json_len, const QuerySnapshot &query,
                                      ScheduleData *data);
    esp_err_t loadCachedSchedule(const QuerySnapshot &query, ScheduleData *data);
    bool getToday(char *date, size_t date_size) const;
    void getNowText(char *text, size_t text_size) const;
    esp_err_t buildUrl(const QuerySnapshot &query, char *url, size_t url_size) const;
    bool urlEncode(const char *input, char *output, size_t output_size) const;
    void trimClassroom(char *text) const;
    void trimServerHost(char *text) const;
    bool parseDate(const char *date, lv_calendar_date_t *parsed) const;
    bool formatDate(const lv_calendar_date_t &date, char *text, size_t text_size) const;
    bool createQuerySnapshot(QuerySnapshot *query) const;
    bool queryMatchesCurrent(const QuerySnapshot &query) const;

    void buildUi(void);
    void setViewState(ViewState state, const char *status, const char *detail, uint32_t color);
    void renderSchedule(const ScheduleData &data);
    void renderCourseList(const ScheduleData &data, int current_index, int next_index);
    void renderEmpty(const ScheduleData &data);
    void renderError(const char *status, const char *detail);
    void updateClassroomLabel(void);
    void updateControls(void);
    void clearCourseList(void);
    void setKeyboardVisible(bool visible);
    void updateSelectedDateUi(void);
    void openCalendar(void);
    void closeCalendar(void);
    void applySelectedDate(const char *date);
    void startRefresh(void);
    void startCatalogLoad(CatalogRequestType type, const char *building = NULL);
    void launchRefresh(const QuerySnapshot &query);
    void launchCatalogLoad(const CatalogRequest &request);
    void startPendingRequest(void);
    void advanceRequestGeneration(void);
    void stopRefreshTimer(void);
    void applyBuildingDropdownFont(void);
    void updateCatalogDropdown(lv_obj_t *dropdown, const CatalogName *names, size_t count, size_t selected,
                               const char *empty_text);
    void freeCatalog(void);
    void applyLabelStyle(lv_obj_t *label, uint32_t color, const lv_font_t *font);
    lv_obj_t *createButton(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_coord_t width,
                           lv_coord_t height);
    lv_obj_t *createPanel(lv_obj_t *parent, lv_coord_t height);
    int parseMinuteOfDay(const char *time_text) const;
    void findCurrentAndNext(const ScheduleData &data, int *current_index, int *next_index) const;
    void updateFromWorker(const RefreshResult &result);
    void updateFromCatalogWorker(CatalogResult *result);

    static void refreshTask(void *arg);
    static void catalogTask(void *arg);
    static void refreshTimerCallback(lv_timer_t *timer);
    static void refreshEventCb(lv_event_t *e);
    static void buildingDropdownEventCb(lv_event_t *e);
    static void roomDropdownEventCb(lv_event_t *e);
    static void saveClassroomEventCb(lv_event_t *e);
    static void serverHostInputEventCb(lv_event_t *e);
    static void keyboardEventCb(lv_event_t *e);
    static void dateButtonEventCb(lv_event_t *e);
    static void todayButtonEventCb(lv_event_t *e);
    static void calendarEventCb(lv_event_t *e);
    static void calendarCloseEventCb(lv_event_t *e);

    volatile bool _busy;
    volatile bool _closing;
    bool _updating_dropdowns;
    ViewState _view_state;
    char _building[64];
    char _classroom[64];
    char _draft_building[64];
    char _draft_classroom[64];
    char _server_host[96];
    char _catalog_server_host[96];
    char _selected_date[16];
    uint32_t _request_generation;
    QuerySnapshot _active_query;
    CatalogRequest _active_catalog_request;
    PendingRequestType _pending_request_type;
    QuerySnapshot _pending_query;
    CatalogRequest _pending_catalog_request;
    ScheduleData _schedule;
    bool _has_schedule;
    bool _classroom_config_valid;
    CatalogName *_buildings;
    size_t _building_count;
    CatalogName *_rooms;
    size_t _room_count;

    TaskHandle_t _worker_task;
    SemaphoreHandle_t _worker_done;
    lv_timer_t *_refresh_timer;

    lv_obj_t *_root;
    lv_obj_t *_title_label;
    lv_obj_t *_classroom_label;
    lv_obj_t *_date_label;
    lv_obj_t *_updated_label;
    lv_obj_t *_status_label;
    lv_obj_t *_detail_label;
    lv_obj_t *_course_list;
    lv_obj_t *_building_dropdown;
    lv_obj_t *_room_dropdown;
    lv_obj_t *_server_host_ta;
    lv_obj_t *_keyboard;
    lv_obj_t *_refresh_btn;
    lv_obj_t *_save_btn;
    lv_obj_t *_date_btn;
    lv_obj_t *_today_btn;
    lv_obj_t *_calendar_overlay;
    lv_obj_t *_calendar;
};
