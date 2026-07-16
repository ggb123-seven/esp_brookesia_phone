#!/usr/bin/env python3
"""Mock HTTP server for the classroom schedule app.

This tool is intentionally dependency-free so it can run on Windows or Linux
with the Python that ESP-IDF already uses.
"""

from __future__ import annotations

import argparse
import json
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from socket import AF_INET, SOCK_DGRAM, socket
from urllib.parse import parse_qs, urlparse


DEFAULT_PATH = "/classroom-schedule/today"
DEFAULT_TOKEN = "change-me"


def guess_lan_ip() -> str:
    try:
        with socket(AF_INET, SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def build_schedule(classroom: str, date_text: str) -> dict:
    classroom_name = {
        "A101": "博知楼 A101",
        "B202": "综合楼 B202",
        "C303": "行知楼 C303",
    }.get(classroom, classroom)

    return {
        "date": date_text,
        "classroom": classroom,
        "classroom_name": classroom_name,
        "updated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "courses": [
            {
                "start": "08:00",
                "end": "08:45",
                "name": "高等数学",
                "teacher": "张老师",
                "group": "软件工程一班",
            },
            {
                "start": "09:00",
                "end": "09:45",
                "name": "大学英语",
                "teacher": "李老师",
                "group": "软件工程一班",
            },
            {
                "start": "10:10",
                "end": "11:40",
                "name": "计算机组成原理",
                "teacher": "王老师",
                "group": "软件工程一班",
            },
            {
                "start": "14:00",
                "end": "15:30",
                "name": "数据库实验",
                "teacher": "赵老师",
                "group": "软件工程一班",
            },
        ],
    }


class ScheduleHandler(BaseHTTPRequestHandler):
    server_version = "ClassroomScheduleMock/1.0"

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            self.send_json({"ok": True})
            return

        if parsed.path != self.server.api_path:
            self.send_json({"error": "not found"}, HTTPStatus.NOT_FOUND)
            return

        query = parse_qs(parsed.query)
        token = self.single(query, "token", "")
        classroom = self.single(query, "classroom", "A101").strip() or "A101"
        date_text = self.single(query, "date", datetime.now().strftime("%Y-%m-%d"))
        mode = self.single(query, "mode", "")

        if token != self.server.api_token:
            self.send_json({"error": "invalid token"}, HTTPStatus.UNAUTHORIZED)
            return

        if mode == "bad-json":
            body = b'{"date":'
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        data = build_schedule(classroom, date_text)
        if mode == "empty" or classroom.upper() == "EMPTY":
            data["courses"] = []

        self.send_json(data)

    def log_message(self, fmt: str, *args: object) -> None:
        print("%s - %s" % (self.address_string(), fmt % args))

    @staticmethod
    def single(query: dict[str, list[str]], name: str, default: str) -> str:
        values = query.get(name)
        if not values:
            return default
        return values[0]

    def send_json(self, data: dict, status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run a mock classroom schedule HTTP API.")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address. Use 0.0.0.0 for LAN access.")
    parser.add_argument("--port", default=8080, type=int, help="HTTP port.")
    parser.add_argument("--path", default=DEFAULT_PATH, help="API path expected by the firmware.")
    parser.add_argument("--token", default=DEFAULT_TOKEN, help="Shared API token.")
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), ScheduleHandler)
    server.api_path = args.path
    server.api_token = args.token

    lan_ip = guess_lan_ip()
    print("Mock classroom schedule server is running.")
    print(f"  Bind: http://{args.host}:{args.port}")
    print(f"  LAN:  http://{lan_ip}:{args.port}{args.path}?classroom=A101&token=<redacted>")
    print("Press Ctrl+C to stop.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping mock server.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
