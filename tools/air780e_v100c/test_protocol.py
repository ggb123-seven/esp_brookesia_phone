#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ZQYuan
# SPDX-License-Identifier: Apache-2.0

"""Safe V100C JSON protocol validator and optional status probe."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path
from typing import Any

PROTOCOL_VERSION = 1
MAX_LINE_BYTES = 256
PHONE_RE = re.compile(r"^\+?[0-9]{5,31}$")


def encode_request(request_id: int, command: str, **fields: Any) -> bytes:
    if isinstance(request_id, bool) or not 1 <= request_id <= 0xFFFFFFFF:
        raise ValueError("request id must be an unsigned non-zero 32-bit integer")
    payload = {"v": PROTOCOL_VERSION, "id": request_id, "cmd": command, **fields}
    line = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
    if len(line) > MAX_LINE_BYTES:
        raise ValueError("protocol line exceeds 256 bytes")
    return line


def decode_response(line: bytes) -> dict[str, Any]:
    if len(line) > MAX_LINE_BYTES:
        raise ValueError("response line exceeds 256 bytes")
    payload = json.loads(line.decode("utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("response must be an object")
    if payload.get("v") != PROTOCOL_VERSION:
        raise ValueError("unsupported protocol version")
    response_id = payload.get("id")
    if (isinstance(response_id, bool) or not isinstance(response_id, int) or
            not 0 <= response_id <= 0xFFFFFFFF or
            not isinstance(payload.get("event"), str)):
        raise ValueError("response is missing id/event")
    return payload


def run_offline_checks() -> None:
    status = encode_request(1, "status")
    assert status == b'{"v":1,"id":1,"cmd":"status"}\n'

    call = encode_request(2, "call", phone="+8613800138000")
    parsed_call = json.loads(call)
    assert PHONE_RE.fullmatch(parsed_call["phone"])
    assert b"+8613800138000" in call

    response = decode_response(b'{"v":1,"id":2,"event":"tts_started"}\n')
    assert response["event"] == "tts_started"

    try:
        encode_request(3, "x" * MAX_LINE_BYTES)
    except ValueError:
        pass
    else:
        raise AssertionError("oversized protocol request was accepted")

    try:
        decode_response(b"{" + (b" " * (MAX_LINE_BYTES - 1)) + b"\n")
    except ValueError:
        pass
    else:
        raise AssertionError("oversized protocol response was accepted")

    for invalid_phone in ("", "1234", "12;ATH", "+", "1" * 32):
        assert PHONE_RE.fullmatch(invalid_phone) is None

    for invalid_id in (-1, 0, 0x100000000, True):
        try:
            encode_request(invalid_id, "status")
        except ValueError:
            pass
        else:
            raise AssertionError(f"invalid request id was accepted: {invalid_id!r}")

    lua_source = Path(__file__).with_name("main.lua").read_text(encoding="utf-8")
    required_markers = (
        'local ALERT_TTS = "环境监测检测到烟雾或可燃气体异常，请及时确认。"',
        'cc.extern_source(ALERT_TTS, true)',
        'audio_ready = audio_drv.initAudioDevice()',
        'audio_ready = audio_ready',
        'status == "AUDIO_START"',
        'status == "EXT_SRC_DONE"',
        'reason = "invalid_phone"',
        'if call_phase == "manual_hangup" then',
        'version_number >= MIN_CC_TTS_FIRMWARE',
        'mobile[name] ~= nil and status == mobile[name]',
        'request_id <= 0xFFFFFFFF',
        'request.id ~= active_id',
        'sys.timerStart(tts_timeout, TTS_TIMEOUT_MS)',
        'call_phase = "failed_hangup"',
    )
    for marker in required_markers:
        assert marker in lua_source, f"missing companion behavior: {marker}"

    audio_source = Path(__file__).with_name("audio_drv.lua").read_text(encoding="utf-8")
    for marker in ('require("exaudio")', 'audio_mode = "new"', "exaudio.setup(AUDIO_CONFIG)"):
        assert marker in audio_source, f"missing audio driver behavior: {marker}"

    print("V100C protocol offline checks passed.")


def probe_status(port: str, baud: int, timeout: float) -> None:
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as exc:
        raise SystemExit("pyserial is required for --port: pip install pyserial") from exc

    request_id = int(time.time()) & 0x7FFFFFFF
    deadline = time.monotonic() + timeout
    with serial.Serial(port, baud, timeout=0.25) as device:
        device.reset_input_buffer()
        device.write(encode_request(request_id, "status"))
        device.flush()
        while time.monotonic() < deadline:
            line = device.readline()
            if not line:
                continue
            try:
                response = decode_response(line)
            except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
                continue
            if response["id"] == request_id and response["event"] == "status":
                print(json.dumps(response, ensure_ascii=False, indent=2))
                if not response.get("firmware_supported"):
                    raise SystemExit("V100C firmware does not support the required CC/TTS path")
                return
    raise SystemExit("timed out waiting for V100C status response")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial port for a safe status-only probe")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()

    run_offline_checks()
    if args.port:
        probe_status(args.port, args.baud, args.timeout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
