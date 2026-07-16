/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class WifiConnectionManager {
public:
    static constexpr size_t MAX_SCAN_RESULTS = 25;

    enum class State : uint8_t {
        INITIALIZING = 0,
        DISABLED,
        IDLE_NO_CONFIG,
        CONNECTING_SAVED,
        CONNECTING_CANDIDATE,
        CONNECTED,
        RETRY_WAIT,
        DISCOVERY,
        ERROR,
    };

    enum class ConnectResult : uint8_t {
        NONE = 0,
        SUCCESS,
        FAILED,
        SAVE_FAILED,
    };

    struct Snapshot {
        State state;
        ConnectResult connect_result;
        bool initialized;
        bool enabled;
        bool got_ip;
        bool scanning;
        bool discovery_active;
        bool has_saved_config;
        uint8_t disconnect_reason;
        int8_t rssi;
        wifi_auth_mode_t authmode;
        uint32_t state_version;
        uint32_t scan_version;
        uint32_t connect_result_version;
        size_t scan_count;
        char ssid[33];
    };

    WifiConnectionManager();

    esp_err_t begin(bool enabled);
    bool setEnabled(bool enabled);
    bool connectCandidate(const char *ssid, const char *password);
    bool startDiscovery(void);
    bool endDiscovery(void);
    bool getSnapshot(Snapshot *snapshot);
    size_t copyScanResults(wifi_ap_record_t *records, size_t capacity, uint32_t *version);

private:
    enum class CommandType : uint8_t {
        SET_ENABLED = 0,
        CONNECT_CANDIDATE,
        START_DISCOVERY,
        END_DISCOVERY,
        STA_CONNECTED,
        STA_DISCONNECTED,
        GOT_IP,
        LOST_IP,
    };

    enum class PendingAction : uint8_t {
        NONE = 0,
        CONNECT_CANDIDATE,
        START_DISCOVERY,
    };

    struct Command {
        CommandType type;
        bool enabled;
        uint8_t reason;
        int8_t rssi;
        wifi_auth_mode_t authmode;
        uint8_t ssid_len;
        char ssid[33];
    };

    static void managerTask(void *arg);
    static void eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

    void run(void);
    esp_err_t initializeWifi(void);
    esp_err_t startWifi(void);
    void handleCommand(const Command &command);
    void handleSetEnabled(bool enabled);
    void handleConnectCandidate(void);
    void beginCandidateConnection(void);
    void failCandidate(uint8_t reason, esp_err_t err, bool already_disconnected = false);
    void commitCandidate(void);
    void handleStartDiscovery(void);
    void handleEndDiscovery(void);
    void performScan(void);
    void handleStaConnected(const Command &command);
    void handleStaDisconnected(uint8_t reason, int8_t rssi);
    void handleGotIp(void);
    void handleLostIp(void);
    void connectSaved(void);
    void scheduleReconnect(uint8_t reason);
    void handleDeadlines(void);
    void restoreSavedConfig(void);
    void clearCandidate(void);
    bool candidateInFlight(void);
    bool enqueue(const Command &command);
    void publishState(State state);
    void publishConnectResult(ConnectResult result);
    void publishScanResults(const wifi_ap_record_t *records, size_t count);
    void updateConnectedMetadata(const char *ssid, wifi_auth_mode_t authmode, int8_t rssi);
    void logStackWatermark(const char *operation);

    static bool isAuthFailure(uint8_t reason);
    static uint32_t reconnectDelayMs(uint32_t retry_count, uint8_t reason);
    static int64_t nowMs(void);
    static void copySsid(char destination[33], const uint8_t *source, size_t source_len);
    static void secureZero(void *data, size_t size);

    QueueHandle_t _command_queue;
    SemaphoreHandle_t _state_mutex;
    TaskHandle_t _task;
    esp_netif_t *_sta_netif;
    esp_event_handler_instance_t _wifi_event_instance;
    esp_event_handler_instance_t _ip_event_instance;

    Snapshot _snapshot;
    std::array<wifi_ap_record_t, MAX_SCAN_RESULTS> _scan_results;
    wifi_config_t _saved_config;
    wifi_config_t _candidate_config;

    State _state;
    PendingAction _pending_action;
    bool _started;
    bool _initialized;
    bool _enabled;
    bool _wifi_started;
    bool _got_ip;
    bool _scanning;
    bool _discovery_active;
    bool _has_saved_config;
    bool _candidate_in_flight;
    bool _ignore_next_disconnect;
    uint8_t _last_disconnect_reason;
    int8_t _last_rssi;
    wifi_auth_mode_t _connected_authmode;
    uint32_t _retry_count;
    int64_t _retry_due_ms;
    int64_t _candidate_due_ms;
    char _connected_ssid[33];
    UBaseType_t _last_logged_stack_watermark;
};
