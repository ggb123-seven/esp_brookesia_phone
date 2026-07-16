/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "WifiConnectionManager.hpp"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr uint32_t WIFI_MANAGER_TASK_STACK_SIZE = 8192;
constexpr UBaseType_t WIFI_MANAGER_TASK_PRIORITY = 4;
constexpr BaseType_t WIFI_MANAGER_TASK_CORE = 0;
constexpr size_t WIFI_MANAGER_COMMAND_QUEUE_LENGTH = 16;
constexpr uint32_t WIFI_MANAGER_LOOP_PERIOD_MS = 200;
constexpr uint32_t WIFI_CANDIDATE_TIMEOUT_MS = 10000;
constexpr uint32_t WIFI_AUTH_RETRY_DELAY_MS = 60000;
constexpr uint32_t WIFI_DISCOVERY_SETTLE_MS = 250;

/*
 * Temporary AP loss uses progressively slower retries. The final entry is the
 * steady-state ceiling once every earlier delay has been used.
 */
constexpr uint32_t WIFI_RETRY_DELAYS_MS[] = {1000, 2000, 5000, 10000, 30000};

const char *TAG = "WifiManager";

bool hasSsid(const wifi_config_t &config)
{
    return config.sta.ssid[0] != '\0';
}

} // namespace

WifiConnectionManager::WifiConnectionManager():
    _command_queue(nullptr),
    _state_mutex(nullptr),
    _task(nullptr),
    _sta_netif(nullptr),
    _wifi_event_instance(nullptr),
    _ip_event_instance(nullptr),
    _snapshot{},
    _scan_results{},
    _saved_config{},
    _candidate_config{},
    _state(State::INITIALIZING),
    _pending_action(PendingAction::NONE),
    _started(false),
    _initialized(false),
    _enabled(false),
    _wifi_started(false),
    _got_ip(false),
    _scanning(false),
    _discovery_active(false),
    _has_saved_config(false),
    _candidate_in_flight(false),
    _ignore_next_disconnect(false),
    _last_disconnect_reason(0),
    _last_rssi(-127),
    _connected_authmode(WIFI_AUTH_OPEN),
    _retry_count(0),
    _retry_due_ms(0),
    _candidate_due_ms(0),
    _connected_ssid{0},
    _last_logged_stack_watermark(UINT32_MAX)
{
    _snapshot.state = State::INITIALIZING;
    _snapshot.rssi = -127;
}

esp_err_t WifiConnectionManager::begin(bool enabled)
{
    if (_started) {
        return ESP_ERR_INVALID_STATE;
    }

    _state_mutex = xSemaphoreCreateMutex();
    if (_state_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    _command_queue = xQueueCreate(WIFI_MANAGER_COMMAND_QUEUE_LENGTH, sizeof(Command));
    if (_command_queue == nullptr) {
        vSemaphoreDelete(_state_mutex);
        _state_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }

    _enabled = enabled;
    _snapshot.enabled = enabled;
    _started = true;

    BaseType_t created = xTaskCreatePinnedToCore(managerTask, "WifiManager", WIFI_MANAGER_TASK_STACK_SIZE,
                                                this, WIFI_MANAGER_TASK_PRIORITY, &_task,
                                                WIFI_MANAGER_TASK_CORE);
    if (created != pdPASS) {
        _started = false;
        vQueueDelete(_command_queue);
        vSemaphoreDelete(_state_mutex);
        _command_queue = nullptr;
        _state_mutex = nullptr;
        _task = nullptr;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool WifiConnectionManager::setEnabled(bool enabled)
{
    Command command = {};
    command.type = CommandType::SET_ENABLED;
    command.enabled = enabled;
    return enqueue(command);
}

bool WifiConnectionManager::connectCandidate(const char *ssid, const char *password)
{
    if (ssid == nullptr || password == nullptr || _state_mutex == nullptr) {
        return false;
    }

    const size_t ssid_len = strnlen(ssid, sizeof(_candidate_config.sta.ssid) + 1);
    const size_t password_len = strnlen(password, sizeof(_candidate_config.sta.password) + 1);
    if (ssid_len == 0 || ssid_len > sizeof(_candidate_config.sta.ssid) ||
        password_len > sizeof(_candidate_config.sta.password)) {
        return false;
    }

    if (xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    if (_candidate_in_flight) {
        xSemaphoreGive(_state_mutex);
        return false;
    }

    secureZero(&_candidate_config, sizeof(_candidate_config));
    memcpy(_candidate_config.sta.ssid, ssid, ssid_len);
    memcpy(_candidate_config.sta.password, password, password_len);
    _candidate_config.sta.failure_retry_cnt = 0;
    _candidate_in_flight = true;
    xSemaphoreGive(_state_mutex);

    Command command = {};
    command.type = CommandType::CONNECT_CANDIDATE;
    if (!enqueue(command)) {
        clearCandidate();
        return false;
    }
    return true;
}

bool WifiConnectionManager::startDiscovery(void)
{
    Command command = {};
    command.type = CommandType::START_DISCOVERY;
    return enqueue(command);
}

bool WifiConnectionManager::endDiscovery(void)
{
    Command command = {};
    command.type = CommandType::END_DISCOVERY;
    return enqueue(command);
}

bool WifiConnectionManager::getSnapshot(Snapshot *snapshot)
{
    if (snapshot == nullptr || _state_mutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    *snapshot = _snapshot;
    xSemaphoreGive(_state_mutex);
    return true;
}

size_t WifiConnectionManager::copyScanResults(wifi_ap_record_t *records, size_t capacity, uint32_t *version)
{
    if (records == nullptr || capacity == 0 || _state_mutex == nullptr) {
        return 0;
    }
    if (xSemaphoreTake(_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    const size_t count = (_snapshot.scan_count < capacity) ? _snapshot.scan_count : capacity;
    memcpy(records, _scan_results.data(), count * sizeof(wifi_ap_record_t));
    if (version != nullptr) {
        *version = _snapshot.scan_version;
    }
    xSemaphoreGive(_state_mutex);
    return count;
}

void WifiConnectionManager::managerTask(void *arg)
{
    WifiConnectionManager *manager = static_cast<WifiConnectionManager *>(arg);
    if (manager == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    manager->run();
}

void WifiConnectionManager::eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    WifiConnectionManager *manager = static_cast<WifiConnectionManager *>(arg);
    if (manager == nullptr || manager->_command_queue == nullptr) {
        return;
    }

    Command command = {};
    bool should_enqueue = true;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        command.type = CommandType::STA_CONNECTED;
        const wifi_event_sta_connected_t *connected = static_cast<const wifi_event_sta_connected_t *>(event_data);
        if (connected != nullptr) {
            command.authmode = connected->authmode;
            command.ssid_len = connected->ssid_len;
            copySsid(command.ssid, connected->ssid, connected->ssid_len);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        command.type = CommandType::STA_DISCONNECTED;
        const wifi_event_sta_disconnected_t *disconnected =
            static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        if (disconnected != nullptr) {
            command.reason = disconnected->reason;
            command.rssi = disconnected->rssi;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        command.type = CommandType::GOT_IP;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        command.type = CommandType::LOST_IP;
    } else {
        should_enqueue = false;
    }

    if (should_enqueue && xQueueSend(manager->_command_queue, &command, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi event queue is full: base=%s id=%" PRId32, event_base, event_id);
    }
}

void WifiConnectionManager::run(void)
{
    esp_err_t err = initializeWifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(err));
        _initialized = false;
        publishState(State::ERROR);
    } else {
        _initialized = true;
        if (_enabled) {
            err = startWifi();
            if (err == ESP_OK) {
                if (_has_saved_config) {
                    connectSaved();
                } else {
                    publishState(State::IDLE_NO_CONFIG);
                }
            } else {
                ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
                publishState(State::ERROR);
            }
        } else {
            publishState(State::DISABLED);
        }
    }

    logStackWatermark("initialized");

    while (true) {
        Command command = {};
        if (xQueueReceive(_command_queue, &command, pdMS_TO_TICKS(WIFI_MANAGER_LOOP_PERIOD_MS)) == pdTRUE) {
            handleCommand(command);
        }
        handleDeadlines();
    }
}

esp_err_t WifiConnectionManager::initializeWifi(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    _sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (_sta_netif == nullptr) {
        _sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (_sta_netif == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, eventHandler, this,
                                              &_wifi_event_instance);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, eventHandler, this,
                                              &_ip_event_instance);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err != ESP_OK) {
        return err;
    }

    secureZero(&_saved_config, sizeof(_saved_config));
    err = esp_wifi_get_config(WIFI_IF_STA, &_saved_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved Wi-Fi config is available: %s", esp_err_to_name(err));
        secureZero(&_saved_config, sizeof(_saved_config));
    }
    _saved_config.sta.failure_retry_cnt = 0;
    _has_saved_config = hasSsid(_saved_config);

    char saved_ssid[33] = {};
    copySsid(saved_ssid, _saved_config.sta.ssid, sizeof(_saved_config.sta.ssid));
    ESP_LOGI(TAG, "Wi-Fi initialized: enabled=%d saved_network=%d ssid=%s",
             _enabled, _has_saved_config, _has_saved_config ? saved_ssid : "<none>");
    return ESP_OK;
}

esp_err_t WifiConnectionManager::startWifi(void)
{
    if (_wifi_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        _wifi_started = true;
    }
    return err;
}

void WifiConnectionManager::handleCommand(const Command &command)
{
    switch (command.type) {
    case CommandType::SET_ENABLED:
        handleSetEnabled(command.enabled);
        break;
    case CommandType::CONNECT_CANDIDATE:
        handleConnectCandidate();
        break;
    case CommandType::START_DISCOVERY:
        handleStartDiscovery();
        break;
    case CommandType::END_DISCOVERY:
        handleEndDiscovery();
        break;
    case CommandType::STA_CONNECTED:
        handleStaConnected(command);
        break;
    case CommandType::STA_DISCONNECTED:
        handleStaDisconnected(command.reason, command.rssi);
        break;
    case CommandType::GOT_IP:
        handleGotIp();
        break;
    case CommandType::LOST_IP:
        handleLostIp();
        break;
    }
}

void WifiConnectionManager::handleSetEnabled(bool enabled)
{
    if (_enabled == enabled) {
        publishState(_state);
        return;
    }

    _enabled = enabled;
    _retry_due_ms = 0;
    _retry_count = 0;

    if (!enabled) {
        _discovery_active = false;
        _pending_action = PendingAction::NONE;
        _got_ip = false;
        _scanning = false;
        _candidate_due_ms = 0;
        _ignore_next_disconnect = false;
        clearCandidate();
        publishState(State::DISABLED);

        if (_wifi_started) {
            esp_err_t err = esp_wifi_disconnect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "Wi-Fi disconnect while disabling failed: %s", esp_err_to_name(err));
            }
            err = esp_wifi_stop();
            if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) {
                _wifi_started = false;
            } else {
                ESP_LOGW(TAG, "Wi-Fi stop failed: %s", esp_err_to_name(err));
            }
        }
        ESP_LOGI(TAG, "Wi-Fi disabled; saved credentials retained");
        return;
    }

    if (!_initialized) {
        publishState(State::ERROR);
        return;
    }

    esp_err_t err = startWifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start while enabling failed: %s", esp_err_to_name(err));
        publishState(State::ERROR);
        return;
    }

    if (_has_saved_config) {
        connectSaved();
    } else {
        publishState(State::IDLE_NO_CONFIG);
    }
}

void WifiConnectionManager::handleConnectCandidate(void)
{
    if (!_enabled || !_initialized) {
        failCandidate(0, ESP_ERR_INVALID_STATE);
        return;
    }

    _retry_due_ms = 0;
    _retry_count = 0;
    _discovery_active = true;

    if (_got_ip || _state == State::CONNECTING_SAVED || _state == State::CONNECTED) {
        _pending_action = PendingAction::CONNECT_CANDIDATE;
        esp_err_t err = esp_wifi_disconnect();
        if (err == ESP_OK) {
            return;
        }
        if (err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "Disconnect before candidate failed: %s", esp_err_to_name(err));
        }
        _pending_action = PendingAction::NONE;
    }

    beginCandidateConnection();
}

void WifiConnectionManager::beginCandidateConnection(void)
{
    esp_err_t err = startWifi();
    if (err != ESP_OK) {
        failCandidate(0, err);
        return;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &_candidate_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        failCandidate(0, err);
        return;
    }

    char candidate_ssid[33] = {};
    copySsid(candidate_ssid, _candidate_config.sta.ssid, sizeof(_candidate_config.sta.ssid));
    updateConnectedMetadata(candidate_ssid, WIFI_AUTH_OPEN, -127);
    _got_ip = false;
    _candidate_due_ms = nowMs() + WIFI_CANDIDATE_TIMEOUT_MS;
    publishState(State::CONNECTING_CANDIDATE);
    ESP_LOGI(TAG, "Connecting candidate network: ssid=%s", candidate_ssid);
}

void WifiConnectionManager::failCandidate(uint8_t reason, esp_err_t err, bool already_disconnected)
{
    _candidate_due_ms = 0;
    _got_ip = false;
    _ignore_next_disconnect = false;
    if (!already_disconnected) {
        esp_err_t disconnect_err = esp_wifi_disconnect();
        _ignore_next_disconnect = (disconnect_err == ESP_OK);
        if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT &&
            disconnect_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "Candidate disconnect failed: %s", esp_err_to_name(disconnect_err));
        }
    }

    restoreSavedConfig();
    clearCandidate();
    if (_discovery_active) {
        publishState(State::DISCOVERY);
    } else if (_has_saved_config) {
        scheduleReconnect(reason);
    } else {
        publishState(State::IDLE_NO_CONFIG);
    }
    publishConnectResult(ConnectResult::FAILED);
    ESP_LOGW(TAG, "Candidate network failed: reason=%u err=%s", reason, esp_err_to_name(err));
}

void WifiConnectionManager::commitCandidate(void)
{
    _candidate_due_ms = 0;
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &_candidate_config);
    }

    if (err == ESP_OK) {
        _saved_config = _candidate_config;
        _saved_config.sta.failure_retry_cnt = 0;
        _has_saved_config = true;
        _discovery_active = false;
        clearCandidate();
        _got_ip = true;
        _retry_count = 0;
        _retry_due_ms = 0;
        publishState(State::CONNECTED);
        publishConnectResult(ConnectResult::SUCCESS);
        ESP_LOGI(TAG, "Candidate network connected and saved: ssid=%s", _connected_ssid);
        return;
    }

    ESP_LOGE(TAG, "Candidate connected but saving failed: %s", esp_err_to_name(err));
    clearCandidate();
    _got_ip = true;
    _discovery_active = false;
    publishState(State::CONNECTED);
    publishConnectResult(ConnectResult::SAVE_FAILED);
}

void WifiConnectionManager::handleStartDiscovery(void)
{
    if (!_enabled || !_initialized) {
        return;
    }
    if (_state == State::CONNECTING_CANDIDATE || candidateInFlight()) {
        ESP_LOGW(TAG, "Ignore discovery request while candidate connection is active");
        return;
    }

    _retry_due_ms = 0;
    _retry_count = 0;
    _discovery_active = true;

    if (_got_ip || _state == State::CONNECTED || _state == State::CONNECTING_SAVED) {
        _pending_action = PendingAction::START_DISCOVERY;
        esp_err_t err = esp_wifi_disconnect();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Disconnecting before explicit Wi-Fi discovery");
            return;
        }
        if (err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "Disconnect before discovery failed: %s", esp_err_to_name(err));
        }
        _pending_action = PendingAction::NONE;
    }

    performScan();
}

void WifiConnectionManager::handleEndDiscovery(void)
{
    if (!_discovery_active) {
        return;
    }
    _discovery_active = false;
    _pending_action = PendingAction::NONE;
    if (_state == State::CONNECTING_CANDIDATE) {
        publishState(State::CONNECTING_CANDIDATE);
        return;
    }
    if (_enabled && _has_saved_config && !_got_ip) {
        connectSaved();
    } else if (_got_ip) {
        publishState(State::CONNECTED);
    } else {
        publishState(State::IDLE_NO_CONFIG);
    }
}

void WifiConnectionManager::performScan(void)
{
    if (!_enabled || !_wifi_started || !_discovery_active) {
        return;
    }

    wifi_ap_record_t *records = static_cast<wifi_ap_record_t *>(
        calloc(MAX_SCAN_RESULTS, sizeof(wifi_ap_record_t)));
    if (records == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate Wi-Fi scan records");
        publishScanResults(nullptr, 0);
        publishState(State::DISCOVERY);
        return;
    }

    _scanning = true;
    publishState(State::DISCOVERY);
    ESP_LOGI(TAG, "Starting explicit Wi-Fi discovery scan");

    uint16_t record_count = MAX_SCAN_RESULTS;
    esp_err_t err = esp_wifi_scan_start(nullptr, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&record_count, records);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi discovery scan failed: %s", esp_err_to_name(err));
        record_count = 0;
    }

    _scanning = false;
    publishScanResults(records, record_count);
    publishState(State::DISCOVERY);
    free(records);
    logStackWatermark("scan");
}

void WifiConnectionManager::handleStaConnected(const Command &command)
{
    _ignore_next_disconnect = false;
    updateConnectedMetadata(command.ssid, command.authmode, _last_rssi);
    publishState(_state);
    ESP_LOGI(TAG, "Station associated: ssid=%s", _connected_ssid);
}

void WifiConnectionManager::handleStaDisconnected(uint8_t reason, int8_t rssi)
{
    _last_disconnect_reason = reason;
    _last_rssi = rssi;
    _got_ip = false;
    ESP_LOGW(TAG, "Station disconnected: reason=%u rssi=%d state=%u",
             reason, rssi, static_cast<unsigned>(_state));

    if (!_enabled || _state == State::DISABLED) {
        publishState(State::DISABLED);
        return;
    }

    if (_pending_action != PendingAction::NONE) {
        const PendingAction action = _pending_action;
        _pending_action = PendingAction::NONE;
        if (action == PendingAction::CONNECT_CANDIDATE) {
            beginCandidateConnection();
        } else if (action == PendingAction::START_DISCOVERY) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_DISCOVERY_SETTLE_MS));
            performScan();
        }
        return;
    }

    if (_ignore_next_disconnect) {
        _ignore_next_disconnect = false;
        publishState(_discovery_active ? State::DISCOVERY : _state);
        return;
    }

    if (_state == State::CONNECTING_CANDIDATE) {
        failCandidate(reason, ESP_FAIL, true);
        return;
    }

    if (_discovery_active || _state == State::DISCOVERY) {
        publishState(State::DISCOVERY);
        return;
    }

    scheduleReconnect(reason);
}

void WifiConnectionManager::handleGotIp(void)
{
    if (!_enabled) {
        return;
    }
    if (_state == State::CONNECTING_CANDIDATE) {
        _ignore_next_disconnect = false;
        commitCandidate();
        return;
    }
    if (_state != State::CONNECTING_SAVED && _state != State::CONNECTED) {
        ESP_LOGW(TAG, "Ignore stale GOT_IP event in state=%u", static_cast<unsigned>(_state));
        return;
    }

    _ignore_next_disconnect = false;
    _got_ip = true;
    _retry_count = 0;
    _retry_due_ms = 0;
    _discovery_active = false;
    publishState(State::CONNECTED);
    ESP_LOGI(TAG, "Station acquired IP: ssid=%s", _connected_ssid);
}

void WifiConnectionManager::handleLostIp(void)
{
    _got_ip = false;
    publishState(_state);
    ESP_LOGW(TAG, "Station lost IP: ssid=%s", _connected_ssid);
}

void WifiConnectionManager::connectSaved(void)
{
    if (!_enabled) {
        publishState(State::DISABLED);
        return;
    }
    if (!_has_saved_config) {
        publishState(State::IDLE_NO_CONFIG);
        return;
    }
    if (_discovery_active) {
        publishState(State::DISCOVERY);
        return;
    }
    if (_got_ip) {
        publishState(State::CONNECTED);
        return;
    }

    esp_err_t err = startWifi();
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &_saved_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saved network connect call failed: %s", esp_err_to_name(err));
        scheduleReconnect(_last_disconnect_reason);
        return;
    }

    char saved_ssid[33] = {};
    copySsid(saved_ssid, _saved_config.sta.ssid, sizeof(_saved_config.sta.ssid));
    updateConnectedMetadata(saved_ssid, WIFI_AUTH_OPEN, _last_rssi);
    _got_ip = false;
    publishState(State::CONNECTING_SAVED);
    ESP_LOGI(TAG, "Connecting saved network: ssid=%s attempt=%" PRIu32, saved_ssid, _retry_count + 1);
}

void WifiConnectionManager::scheduleReconnect(uint8_t reason)
{
    if (!_enabled) {
        publishState(State::DISABLED);
        return;
    }
    if (!_has_saved_config) {
        publishState(State::IDLE_NO_CONFIG);
        return;
    }
    if (_discovery_active) {
        publishState(State::DISCOVERY);
        return;
    }

    const uint32_t delay_ms = reconnectDelayMs(_retry_count, reason);
    ++_retry_count;
    _retry_due_ms = nowMs() + delay_ms;
    publishState(State::RETRY_WAIT);
    ESP_LOGW(TAG, "Saved network retry scheduled: reason=%u delay_ms=%" PRIu32, reason, delay_ms);
}

void WifiConnectionManager::handleDeadlines(void)
{
    const int64_t now = nowMs();
    if (_state == State::CONNECTING_CANDIDATE && _candidate_due_ms > 0 && now >= _candidate_due_ms) {
        failCandidate(0, ESP_ERR_TIMEOUT);
        return;
    }
    if (_state == State::RETRY_WAIT && _retry_due_ms > 0 && now >= _retry_due_ms) {
        _retry_due_ms = 0;
        connectSaved();
    }
}

void WifiConnectionManager::restoreSavedConfig(void)
{
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK && _has_saved_config) {
        err = esp_wifi_set_config(WIFI_IF_STA, &_saved_config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore saved Wi-Fi config: %s", esp_err_to_name(err));
    }
}

void WifiConnectionManager::clearCandidate(void)
{
    if (_state_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(_state_mutex, portMAX_DELAY);
    secureZero(&_candidate_config, sizeof(_candidate_config));
    _candidate_in_flight = false;
    xSemaphoreGive(_state_mutex);
}

bool WifiConnectionManager::candidateInFlight(void)
{
    if (_state_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(_state_mutex, portMAX_DELAY);
    const bool in_flight = _candidate_in_flight;
    xSemaphoreGive(_state_mutex);
    return in_flight;
}

bool WifiConnectionManager::enqueue(const Command &command)
{
    if (_command_queue == nullptr) {
        return false;
    }
    if (xQueueSend(_command_queue, &command, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi command queue is full: command=%u", static_cast<unsigned>(command.type));
        return false;
    }
    return true;
}

void WifiConnectionManager::publishState(State state)
{
    _state = state;
    if (_state_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(_state_mutex, portMAX_DELAY);
    _snapshot.state = state;
    _snapshot.initialized = _initialized;
    _snapshot.enabled = _enabled;
    _snapshot.got_ip = _got_ip;
    _snapshot.scanning = _scanning;
    _snapshot.discovery_active = _discovery_active;
    _snapshot.has_saved_config = _has_saved_config;
    _snapshot.disconnect_reason = _last_disconnect_reason;
    _snapshot.rssi = _last_rssi;
    _snapshot.authmode = _connected_authmode;
    memcpy(_snapshot.ssid, _connected_ssid, sizeof(_snapshot.ssid));
    ++_snapshot.state_version;
    xSemaphoreGive(_state_mutex);
}

void WifiConnectionManager::publishConnectResult(ConnectResult result)
{
    if (_state_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(_state_mutex, portMAX_DELAY);
    _snapshot.connect_result = result;
    ++_snapshot.connect_result_version;
    xSemaphoreGive(_state_mutex);
}

void WifiConnectionManager::publishScanResults(const wifi_ap_record_t *records, size_t count)
{
    if (_state_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(_state_mutex, portMAX_DELAY);

    const size_t bounded_count = (count < MAX_SCAN_RESULTS) ? count : MAX_SCAN_RESULTS;
    _scan_results.fill(wifi_ap_record_t{});
    if (records != nullptr && bounded_count > 0) {
        memcpy(_scan_results.data(), records, bounded_count * sizeof(wifi_ap_record_t));
    }
    _snapshot.scan_count = bounded_count;
    ++_snapshot.scan_version;
    xSemaphoreGive(_state_mutex);
}

void WifiConnectionManager::updateConnectedMetadata(const char *ssid, wifi_auth_mode_t authmode, int8_t rssi)
{
    if (ssid != nullptr) {
        const size_t length = strnlen(ssid, sizeof(_connected_ssid) - 1);
        memset(_connected_ssid, 0, sizeof(_connected_ssid));
        memcpy(_connected_ssid, ssid, length);
    }
    _connected_authmode = authmode;
    _last_rssi = rssi;
}

void WifiConnectionManager::logStackWatermark(const char *operation)
{
    const UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
    if (watermark < _last_logged_stack_watermark) {
        _last_logged_stack_watermark = watermark;
        ESP_LOGI(TAG, "Task stack minimum after %s: %" PRIu32 " bytes",
                 operation, static_cast<uint32_t>(watermark));
    }
}

bool WifiConnectionManager::isAuthFailure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return true;
    default:
        return false;
    }
}

uint32_t WifiConnectionManager::reconnectDelayMs(uint32_t retry_count, uint8_t reason)
{
    if (isAuthFailure(reason)) {
        return WIFI_AUTH_RETRY_DELAY_MS;
    }
    const size_t count = sizeof(WIFI_RETRY_DELAYS_MS) / sizeof(WIFI_RETRY_DELAYS_MS[0]);
    const size_t index = (retry_count < count) ? retry_count : count - 1;
    return WIFI_RETRY_DELAYS_MS[index];
}

int64_t WifiConnectionManager::nowMs(void)
{
    return esp_timer_get_time() / 1000;
}

void WifiConnectionManager::copySsid(char destination[33], const uint8_t *source, size_t source_len)
{
    if (destination == nullptr) {
        return;
    }
    memset(destination, 0, 33);
    if (source == nullptr || source_len == 0) {
        return;
    }
    const size_t bounded_len = (source_len < 32) ? source_len : 32;
    size_t actual_len = 0;
    while (actual_len < bounded_len && source[actual_len] != '\0') {
        ++actual_len;
    }
    memcpy(destination, source, actual_len);
}

void WifiConnectionManager::secureZero(void *data, size_t size)
{
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
    while (size-- > 0) {
        *bytes++ = 0;
    }
}
