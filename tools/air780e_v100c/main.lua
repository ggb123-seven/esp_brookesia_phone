-- SPDX-FileCopyrightText: 2026 ZQYuan
-- SPDX-License-Identifier: Apache-2.0

PROJECT = "V100C_PARENT_ALERT"
VERSION = "001.000.000"

local sys = require("sys")
local audio_drv = require("audio_drv")

local UART_ID = 1
local UART_BAUD = 115200
local PROTOCOL_VERSION = 1
local MAX_LINE_BYTES = 256
local MIN_CC_TTS_FIRMWARE = 2048
local CALL_CONNECT_TIMEOUT_MS = 30000
local TTS_TIMEOUT_MS = 30000
local HANGUP_CLEANUP_TIMEOUT_MS = 10000
local ALERT_TTS = "环境监测检测到烟雾或可燃气体异常，请及时确认。"

local rx_buffer = ""
local audio_ready = false
local cc_ready = false
local active_id = nil
local call_phase = "idle"
local call_timer = nil
local connected_reported = false
local tts_done = false

local function firmware_version_number()
    local version = rtos.version() or ""
    return tonumber(version:match("[Vv](%d+)")) or 0
end

local function firmware_supported()
    if type(cc.extern_source) ~= "function" then
        return false
    end

    local version_number = firmware_version_number()
    return version_number >= MIN_CC_TTS_FIRMWARE
end

local function network_registered()
    local status = mobile.status()
    local function matches(name)
        return mobile[name] ~= nil and status == mobile[name]
    end
    return matches("REGISTERED") or
           matches("REGISTERED_ROAMING") or
           matches("CSFB_NOT_PREFERRED_REGISTERED") or
           matches("CSFB_NOT_PREFERRED_REGISTERED_ROAMING")
end

local function send_message(message)
    message.v = PROTOCOL_VERSION
    local encoded = json.encode(message)
    if type(encoded) ~= "string" or (#encoded + 1) > MAX_LINE_BYTES then
        return false
    end
    uart.write(UART_ID, encoded .. "\n")
    return true
end

local function send_event(request_id, event, fields)
    local message = fields or {}
    message.id = request_id or 0
    message.event = event
    send_message(message)
end

local function stop_call_timer()
    if call_timer then
        sys.timerStop(call_timer)
        call_timer = nil
    end
end

local function reset_call_state()
    stop_call_timer()
    active_id = nil
    call_phase = "idle"
    connected_reported = false
    tts_done = false
end

local function finish_failed_hangup()
    call_timer = nil
    reset_call_state()
end

local function fail_call(reason, hangup)
    if active_id == nil then
        return
    end

    local request_id = active_id
    call_phase = "failed"
    stop_call_timer()
    if hangup then
        call_phase = "failed_hangup"
        cc.extern_source(nil)
        cc.hangUp(0)
    end
    send_event(request_id, "failed", {reason = reason})
    if hangup then
        call_timer = sys.timerStart(finish_failed_hangup, HANGUP_CLEANUP_TIMEOUT_MS)
    else
        reset_call_state()
    end
end

local function complete_call()
    if active_id == nil then
        return
    end

    local request_id = active_id
    send_event(request_id, "completed")
    reset_call_state()
end

local function phone_number_is_valid(phone)
    if type(phone) ~= "string" or #phone < 5 or #phone > 31 then
        return false
    end

    local digits = phone
    if phone:sub(1, 1) == "+" then
        digits = phone:sub(2)
    end
    return #digits > 0 and digits:match("^%d+$") ~= nil
end

local function report_status(request_id)
    local registered = network_registered()
    local supported = firmware_supported()
    local firmware = rtos.version() or "unknown"
    if #firmware > 63 then
        firmware = firmware:sub(1, 63)
    end
    send_event(request_id, "status", {
        ready = audio_ready and cc_ready and supported and registered,
        busy = active_id ~= nil,
        audio_ready = audio_ready,
        cc_ready = cc_ready,
        network_registered = registered,
        csq = mobile.csq() or 0,
        firmware = firmware,
        firmware_supported = supported,
    })
end

local function call_timeout()
    call_timer = nil
    fail_call("dial_timeout", true)
end

local function tts_timeout()
    call_timer = nil
    fail_call("tts_timeout", true)
end

local function start_call(request)
    if active_id ~= nil then
        send_event(request.id, "failed", {reason = "busy"})
        return
    end
    if not firmware_supported() then
        send_event(request.id, "failed", {reason = "unsupported_firmware"})
        return
    end
    if not audio_ready then
        send_event(request.id, "failed", {reason = "audio_not_ready"})
        return
    end
    if not cc_ready then
        send_event(request.id, "failed", {reason = "cc_not_ready"})
        return
    end
    if not network_registered() then
        send_event(request.id, "failed", {reason = "network_not_registered"})
        return
    end
    if not phone_number_is_valid(request.phone) then
        send_event(request.id, "failed", {reason = "invalid_phone"})
        return
    end

    active_id = request.id
    call_phase = "dialing"
    connected_reported = false
    tts_done = false

    if not cc.dial(0, request.phone) then
        fail_call("dial_rejected", false)
        return
    end

    send_event(active_id, "dialing")
    call_timer = sys.timerStart(call_timeout, CALL_CONNECT_TIMEOUT_MS)
end

local function request_id_is_valid(request_id)
    return type(request_id) == "number" and request_id > 0 and
           request_id <= 0xFFFFFFFF and request_id == math.floor(request_id)
end

local function handle_command(request)
    if type(request) ~= "table" or request.v ~= PROTOCOL_VERSION or
       not request_id_is_valid(request.id) or type(request.cmd) ~= "string" then
        local response_id = 0
        if type(request) == "table" and request_id_is_valid(request.id) then
            response_id = request.id
        end
        send_event(response_id, "failed", {reason = "invalid_request"})
        return
    end

    if request.cmd == "status" then
        report_status(request.id)
    elseif request.cmd == "call" then
        start_call(request)
    elseif request.cmd == "hangup" then
        if active_id == nil then
            send_event(request.id, "failed", {reason = "not_in_call"})
        elseif request.id ~= active_id then
            send_event(request.id, "failed", {reason = "request_id_mismatch"})
        else
            call_phase = "manual_hangup"
            cc.extern_source(nil)
            cc.hangUp(0)
        end
    else
        send_event(request.id, "failed", {reason = "unknown_command"})
    end
end

local function handle_line(line)
    if #line == 0 then
        return
    end

    local ok, request = pcall(json.decode, line)
    if not ok or type(request) ~= "table" then
        send_event(0, "failed", {reason = "invalid_json"})
        return
    end
    handle_command(request)
end

local function consume_rx_data(data)
    rx_buffer = rx_buffer .. data
    if #rx_buffer > MAX_LINE_BYTES and not rx_buffer:find("\n", 1, true) then
        rx_buffer = ""
        send_event(0, "failed", {reason = "line_too_long"})
        return
    end

    while true do
        local newline = rx_buffer:find("\n", 1, true)
        if newline == nil then
            break
        end

        local line = rx_buffer:sub(1, newline - 1):gsub("\r$", "")
        rx_buffer = rx_buffer:sub(newline + 1)
        if #line >= MAX_LINE_BYTES then
            send_event(0, "failed", {reason = "line_too_long"})
        else
            handle_line(line)
        end
    end
end

local function uart_receive(id)
    while true do
        local data = uart.read(id, 128)
        if data == "" then
            break
        end
        consume_rx_data(data)
    end
end

local function report_connected()
    if active_id ~= nil and not connected_reported then
        connected_reported = true
        send_event(active_id, "connected")
    end
end

local function handle_cc_event(status)
    if status == "READY" then
        cc_ready = true
        return
    end

    if status == "INCOMINGCALL" then
        cc.hangUp(0)
        return
    end

    if active_id == nil then
        return
    end

    if status == "MAKE_CALL_OK" then
        return
    elseif status == "CONNECTED" or status == "SPEECH_START" then
        report_connected()
    elseif status == "AUDIO_START" then
        report_connected()
        stop_call_timer()
        call_phase = "tts_playing"
        cc.extern_source(nil)
        if cc.extern_source(ALERT_TTS, true) then
            send_event(active_id, "tts_started")
            call_timer = sys.timerStart(tts_timeout, TTS_TIMEOUT_MS)
        else
            fail_call("tts_start_failed", true)
        end
    elseif status == "EXT_SRC_DONE" then
        tts_done = true
        call_phase = "hangup_after_tts"
        send_event(active_id, "tts_done")
        cc.hangUp(0)
    elseif status == "MAKE_CALL_FAILED" then
        fail_call("make_call_failed", false)
    elseif status == "DISCONNECTED" then
        if call_phase == "manual_hangup" then
            fail_call("manual_hangup", false)
        elseif call_phase == "failed_hangup" then
            reset_call_state()
        elseif tts_done then
            complete_call()
        else
            fail_call("remote_disconnected", false)
        end
    elseif status == "HANGUP_CALL_DONE" then
        if call_phase == "hangup_after_tts" then
            complete_call()
        elseif call_phase == "manual_hangup" then
            fail_call("manual_hangup", false)
        elseif call_phase == "failed_hangup" then
            reset_call_state()
        end
    end
end

uart.setup(UART_ID, UART_BAUD, 8, 1)
uart.on(UART_ID, "receive", uart_receive)
sys.subscribe("CC_IND", handle_cc_event)

if wdt then
    wdt.init(9000)
    sys.timerLoopStart(wdt.feed, 3000)
end

sys.taskInit(function()
    if not firmware_supported() then
        return
    end
    audio_ready = audio_drv.initAudioDevice()
    if not audio_ready then
        return
    end
    cc.init(audio_drv.getMultimediaId())
end)

sys.run()
