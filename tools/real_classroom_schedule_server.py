#!/usr/bin/env python3
"""Real classroom schedule middleware for the ESP32 classroom schedule app.

The MVP keeps the firmware HTTP/JSON contract stable while isolating school
WebVPN/EAMS complexity on a server. It intentionally uses only the Python
standard library so it can run on a small Ubuntu host without extra packages.
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone, timedelta
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from socket import AF_INET, SOCK_DGRAM, socket
from typing import Any
from urllib.parse import parse_qs, urlencode, urljoin, urlparse, urlunparse
from urllib.request import Request, urlopen
from urllib.error import HTTPError


DEFAULT_PATH = "/classroom-schedule/today"
DEFAULT_ALERT_PATH = "/parent-call-alert/trigger"
DEFAULT_TOKEN = "change-me"
DEFAULT_FIXTURE = Path(__file__).with_name("fixtures") / "classroom_schedule_fixture.json"
MAX_COURSES = 16
MAX_ALERT_BODY_BYTES = 2048
ALERT_TEXT_RE = re.compile(r"^[\w .,:;!?+\-_/()\[\]\u4e00-\u9fff，。！？、；：（）【】]{0,160}$")
SHANGHAI_TZ = timezone(timedelta(hours=8))
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
TIME_RE = re.compile(r"^\d{2}:\d{2}$")
DEFAULT_SECTION_TIMES = {
    1: ("08:00", "08:45"),
    2: ("08:55", "09:40"),
    3: ("10:10", "10:55"),
    4: ("11:05", "11:50"),
    5: ("14:00", "14:45"),
    6: ("14:55", "15:40"),
    7: ("16:10", "16:55"),
    8: ("17:05", "17:50"),
    9: ("19:00", "19:45"),
    10: ("19:55", "20:40"),
}
EAMS_SYLLABUS_PAGE_SIZE = 500
EAMS_SYLLABUS_MAX_PAGES = 12
EAMS_BUILDING_IDS = {
    "博文楼": "7",
    "知行楼": "8",
    "主楼机房": "9",
    "静远楼": "11",
    "博雅楼": "13",
    "耘慧楼": "14",
    "物理实验室": "15",
    "葫芦岛物理实验室": "16",
    "中和楼": "17",
    "致远楼": "18",
    "新华楼": "19",
    "尔雅楼": "20",
    "葫芦岛机房": "21",
}
EAMS_BUILDING_QUERY_NAMES = {
    "博文楼": "null",
    "知行楼": "育龙主楼",
    "主楼机房": "主楼机房",
    "静远楼": "静远楼",
    "博雅楼": "博雅楼",
    "耘慧楼": "耘慧楼",
    "物理实验室": "物理实验室",
    "葫芦岛物理实验室": "葫芦岛物理实验室",
    "中和楼": "null",
    "致远楼": "null",
    "新华楼": "新华楼",
    "尔雅楼": "尔雅楼",
    "葫芦岛机房": "葫芦岛机房",
}
EAMS_WEEKDAY_NAMES = {
    "星期一": 1,
    "星期二": 2,
    "星期三": 3,
    "星期四": 4,
    "星期五": 5,
    "星期六": 6,
    "星期日": 7,
    "周一": 1,
    "周二": 2,
    "周三": 3,
    "周四": 4,
    "周五": 5,
    "周六": 6,
    "周日": 7,
}
EMBEDDED_JSON_RE = re.compile(
    r"<script[^>]+id=[\"']classroom-schedule-json[\"'][^>]*>(.*?)</script>",
    re.IGNORECASE | re.DOTALL,
)


class ScheduleError(Exception):
    """Base class for sanitized service errors."""

    status = HTTPStatus.INTERNAL_SERVER_ERROR
    code = "internal_error"

    def __init__(self, message: str) -> None:
        super().__init__(message)
        self.message = message


class BadRequest(ScheduleError):
    status = HTTPStatus.BAD_REQUEST
    code = "bad_request"


class NotFound(ScheduleError):
    status = HTTPStatus.NOT_FOUND
    code = "not_found"


class Unauthorized(ScheduleError):
    status = HTTPStatus.UNAUTHORIZED
    code = "unauthorized"


class ProviderUnavailable(ScheduleError):
    status = HTTPStatus.SERVICE_UNAVAILABLE
    code = "provider_unavailable"


class UpstreamError(ScheduleError):
    status = HTTPStatus.BAD_GATEWAY
    code = "upstream_error"


class PayloadTooLarge(ScheduleError):
    status = HTTPStatus.REQUEST_ENTITY_TOO_LARGE
    code = "payload_too_large"


@dataclass
class CacheEntry:
    data: dict[str, Any]
    stored_at: float


class ScheduleCache:
    def __init__(self, ttl_seconds: int) -> None:
        self.ttl_seconds = max(0, ttl_seconds)
        self._entries: dict[tuple[str, str], CacheEntry] = {}

    def get(self, classroom: str, date_text: str) -> dict[str, Any] | None:
        if self.ttl_seconds <= 0:
            return None
        entry = self._entries.get((classroom, date_text))
        if entry is None:
            return None
        if time.time() - entry.stored_at > self.ttl_seconds:
            return None
        cached = dict(entry.data)
        cached["cached"] = True
        return cached

    def set(self, classroom: str, date_text: str, data: dict[str, Any]) -> None:
        if self.ttl_seconds <= 0:
            return
        self._entries[(classroom, date_text)] = CacheEntry(dict(data), time.time())


class ScheduleProvider:
    name = "base"

    def fetch_schedule(self, classroom: str, date_text: str) -> dict[str, Any]:
        raise NotImplementedError


class FixtureScheduleProvider(ScheduleProvider):
    name = "fixture"

    def __init__(self, fixture_path: Path) -> None:
        self.fixture_path = fixture_path
        self._payload = self._load_fixture(fixture_path)

    @staticmethod
    def _load_fixture(fixture_path: Path) -> dict[str, Any]:
        try:
            with fixture_path.open("r", encoding="utf-8") as fp:
                payload = json.load(fp)
        except FileNotFoundError as exc:
            raise ProviderUnavailable("fixture file not found") from exc
        except json.JSONDecodeError as exc:
            raise ProviderUnavailable("fixture file is not valid JSON") from exc
        if not isinstance(payload, dict) or not isinstance(payload.get("classrooms"), dict):
            raise ProviderUnavailable("fixture file must contain classrooms object")
        return payload

    def fetch_schedule(self, classroom: str, date_text: str) -> dict[str, Any]:
        classrooms = self._payload["classrooms"]
        record = classrooms.get(classroom, {"classroom_name": classroom, "courses": []})
        if not isinstance(record, dict):
            raise UpstreamError("fixture classroom record is invalid")
        return normalize_schedule(classroom, date_text, record)


class ManualSessionEamsProvider(ScheduleProvider):
    name = "manual-session"

    def __init__(self, session_file: Path | None, timeout_seconds: int = 10) -> None:
        self.timeout_seconds = max(1, timeout_seconds)
        self.config = self._load_session_config(session_file)

    @staticmethod
    def _load_session_config(session_file: Path | None) -> dict[str, Any]:
        if session_file is None:
            raise ProviderUnavailable("manual WebVPN/EAMS session file is not configured")
        if not session_file.exists():
            raise ProviderUnavailable("manual WebVPN/EAMS session file is missing")
        try:
            with session_file.open("r", encoding="utf-8") as fp:
                config = json.load(fp)
        except json.JSONDecodeError as exc:
            raise ProviderUnavailable("manual WebVPN/EAMS session file is not valid JSON") from exc
        if not isinstance(config, dict):
            raise ProviderUnavailable("manual WebVPN/EAMS session file must be a JSON object")
        if not isinstance(config.get("upstream_url"), str) or not config["upstream_url"].strip():
            raise ProviderUnavailable("manual WebVPN/EAMS session requires upstream_url")
        apply_playwright_storage_state(config, session_file)
        return config

    def fetch_schedule(self, classroom: str, date_text: str) -> dict[str, Any]:
        upstream_url = self._build_url(classroom, date_text)
        headers = self._build_headers()
        request = Request(upstream_url, headers=headers, method="GET")
        try:
            with urlopen(request, timeout=self.timeout_seconds) as response:
                raw = response.read()
                content_type = response.headers.get("Content-Type", "")
        except HTTPError as exc:
            if exc.code in (HTTPStatus.UNAUTHORIZED, HTTPStatus.FORBIDDEN):
                raise ProviderUnavailable("manual WebVPN/EAMS session is rejected or expired") from exc
            raise UpstreamError("manual WebVPN/EAMS upstream returned an error") from exc
        except OSError as exc:
            raise ProviderUnavailable("manual WebVPN/EAMS upstream request failed") from exc

        text = decode_response_text(raw, content_type)
        record = parse_upstream_payload(text, content_type)
        return normalize_schedule(classroom, date_text, record)

    def _build_url(self, classroom: str, date_text: str) -> str:
        upstream_url = str(self.config["upstream_url"]).strip()
        query_template = self.config.get("query", {})
        if not isinstance(query_template, dict):
            raise ProviderUnavailable("manual WebVPN/EAMS query config must be an object")
        query = {
            str(key): render_template_value(value, classroom, date_text)
            for key, value in query_template.items()
        }
        if not query:
            return upstream_url
        separator = "&" if "?" in upstream_url else "?"
        return upstream_url + separator + urlencode(query)

    def _build_headers(self) -> dict[str, str]:
        configured = self.config.get("headers", {})
        if configured is None:
            configured = {}
        if not isinstance(configured, dict):
            raise ProviderUnavailable("manual WebVPN/EAMS headers config must be an object")
        headers = {
            str(key): str(value)
            for key, value in configured.items()
            if isinstance(key, str) and value is not None
        }
        headers.setdefault("User-Agent", "ClassroomScheduleMiddleware/0.1")
        return headers


def apply_playwright_storage_state(config: dict[str, Any], session_file: Path) -> None:
    state_file_text = str(config.get("playwright_storage_state") or "").strip()
    if not state_file_text:
        return

    state_file = Path(state_file_text)
    if not state_file.is_absolute():
        state_file = session_file.parent / state_file
    if not state_file.exists():
        raise ProviderUnavailable("Playwright EAMS storage state file is missing")
    try:
        with state_file.open("r", encoding="utf-8") as fp:
            state = json.load(fp)
    except json.JSONDecodeError as exc:
        raise ProviderUnavailable("Playwright EAMS storage state is not valid JSON") from exc
    if not isinstance(state, dict) or not isinstance(state.get("cookies"), list):
        raise ProviderUnavailable("Playwright EAMS storage state has no cookies array")

    upstream_host = (urlparse(str(config["upstream_url"])).hostname or "").lower()
    cookie_pairs: list[str] = []
    now = time.time()
    for cookie in state["cookies"]:
        if not isinstance(cookie, dict):
            continue
        domain = str(cookie.get("domain") or "").lstrip(".").lower()
        if not domain or (upstream_host != domain and not upstream_host.endswith("." + domain)):
            continue
        expires = cookie.get("expires")
        if isinstance(expires, (int, float)) and expires > 0 and expires <= now:
            continue
        name = cookie.get("name")
        value = cookie.get("value")
        if not isinstance(name, str) or not name or not isinstance(value, str):
            continue
        cookie_pairs.append(f"{name}={value}")
    if not cookie_pairs:
        raise ProviderUnavailable("Playwright EAMS storage state has no usable upstream cookies")

    configured_headers = config.get("headers")
    if configured_headers is None:
        configured_headers = {}
    if not isinstance(configured_headers, dict):
        raise ProviderUnavailable("manual WebVPN/EAMS headers config must be an object")
    configured_headers["Cookie"] = "; ".join(cookie_pairs)
    config["headers"] = configured_headers


class EamsRoomOccupancyProvider(ScheduleProvider):
    name = "eams-room-occupancy"

    def __init__(self, session_file: Path | None, timeout_seconds: int = 10) -> None:
        self.timeout_seconds = max(1, timeout_seconds)
        self.config = ManualSessionEamsProvider._load_session_config(session_file)
        self._syllabus_lock = threading.Lock()

    def fetch_schedule(self, classroom: str, date_text: str) -> dict[str, Any]:
        classroom_name = normalize_eams_classroom_name(classroom)
        target_week = self._week_for_date(date_text)
        target_day = datetime.strptime(date_text, "%Y-%m-%d").isoweekday()
        detailed_courses = self._fetch_syllabus_courses(classroom_name, target_week, target_day)

        detail_url = self._build_detail_url(classroom_name, date_text)
        request = Request(detail_url, headers=self._build_headers(), method="GET")
        try:
            with urlopen(request, timeout=self.timeout_seconds) as response:
                raw = response.read()
                content_type = response.headers.get("Content-Type", "")
        except HTTPError as exc:
            if exc.code in (HTTPStatus.UNAUTHORIZED, HTTPStatus.FORBIDDEN) or 300 <= exc.code < 400:
                raise ProviderUnavailable("EAMS room occupancy session is rejected or expired") from exc
            raise UpstreamError("EAMS room occupancy upstream returned an error") from exc
        except OSError as exc:
            raise ProviderUnavailable("EAMS room occupancy upstream request failed") from exc

        text = decode_eams_html(raw, content_type)
        sections_by_day = extract_room_occupancy_sections(text, classroom_name)
        occupied_sections = sections_by_day.get(target_day, [])
        record = {
            "classroom_name": classroom_name,
            "courses": merge_syllabus_and_occupancy_courses(detailed_courses, occupied_sections),
        }
        return normalize_schedule(classroom, date_text, record)

    def _fetch_syllabus_courses(self, classroom_name: str, target_week: int, target_day: int) -> list[dict[str, str]]:
        with self._syllabus_lock:
            return self._fetch_syllabus_courses_locked(classroom_name, target_week, target_day)

    def _fetch_syllabus_courses_locked(
        self,
        classroom_name: str,
        target_week: int,
        target_day: int,
    ) -> list[dict[str, str]]:
        courses: list[dict[str, str]] = []
        total_pages = 1
        for page_no in range(1, EAMS_SYLLABUS_MAX_PAGES + 1):
            page_url = self._build_syllabus_url(page_no)
            request = Request(page_url, headers=self._build_headers(), method="GET")
            try:
                with urlopen(request, timeout=self.timeout_seconds) as response:
                    raw = response.read()
                    content_type = response.headers.get("Content-Type", "")
            except HTTPError as exc:
                if exc.code in (HTTPStatus.UNAUTHORIZED, HTTPStatus.FORBIDDEN) or 300 <= exc.code < 400:
                    raise ProviderUnavailable("EAMS syllabus session is rejected or expired") from exc
                if courses:
                    break
                continue
            except (OSError, TimeoutError):
                if courses:
                    break
                continue

            text = decode_eams_html(raw, content_type)
            lessons = extract_syllabus_lessons(text)
            courses.extend(
                build_syllabus_courses(
                    lessons,
                    classroom_name=classroom_name,
                    target_week=target_week,
                    target_day=target_day,
                )
            )
            total_pages = syllabus_total_pages(text, EAMS_SYLLABUS_PAGE_SIZE) or total_pages
            if page_no >= total_pages:
                break
        return dedupe_courses(courses)[:MAX_COURSES]

    def _build_detail_url(self, classroom_name: str, date_text: str) -> str:
        upstream_url = str(self.config["upstream_url"]).strip()
        parsed = urlparse(upstream_url)
        if not parsed.scheme or not parsed.netloc:
            raise ProviderUnavailable("EAMS session upstream_url must be absolute")
        prefix = parsed.path.split("/eams/", 1)[0] if "/eams/" in parsed.path else ""
        query = {
            "semesterId": str(self.config.get("semesterId") or self.config.get("semester_id") or "723"),
            "iWeek": str(self._week_for_date(date_text)),
            "room.building.id": str(self._building_id_for(classroom_name)),
            "buildingname": self._building_query_name_for(classroom_name),
        }
        path = prefix + "/eams/classroom/occupy/class-details!unitDetail.action"
        return urlunparse((parsed.scheme, parsed.netloc, path, "", urlencode(query), ""))

    def _build_syllabus_url(self, page_no: int) -> str:
        upstream_url = str(self.config["upstream_url"]).strip()
        parsed = urlparse(upstream_url)
        if not parsed.scheme or not parsed.netloc:
            raise ProviderUnavailable("EAMS session upstream_url must be absolute")
        prefix = parsed.path.split("/eams/", 1)[0] if "/eams/" in parsed.path else ""
        query = {
            "lesson.project.id": str(self.config.get("projectId") or self.config.get("project_id") or "1"),
            "lesson.semester.id": str(self.config.get("semesterId") or self.config.get("semester_id") or "723"),
            "pageNo": str(page_no),
            "pageSize": str(int(self.config.get("syllabus_page_size") or EAMS_SYLLABUS_PAGE_SIZE)),
        }
        path = prefix + "/eams/stdSyllabus!search.action"
        return urlunparse((parsed.scheme, parsed.netloc, path, "", urlencode(query), ""))

    def _week_for_date(self, date_text: str) -> int:
        first_week_start = str(
            self.config.get("first_week_start")
            or self.config.get("semester_start_date")
            or ""
        ).strip()
        if first_week_start:
            try:
                start_date = datetime.strptime(first_week_start, "%Y-%m-%d").date()
                target_date = datetime.strptime(date_text, "%Y-%m-%d").date()
            except ValueError as exc:
                raise ProviderUnavailable("EAMS room occupancy first_week_start must use YYYY-MM-DD") from exc
            week = ((target_date - start_date).days // 7) + 1
            return max(1, week)
        if self.config.get("iWeek") or self.config.get("week"):
            return int(self.config.get("iWeek") or self.config.get("week"))
        return 1

    def _building_id_for(self, classroom_name: str) -> str:
        buildings = self.config.get("buildings", {})
        building_name = self._building_name_for(classroom_name)
        if isinstance(buildings, dict):
            configured = buildings.get(building_name) or buildings.get(classroom_name)
            if configured:
                return str(configured)
        default_id = EAMS_BUILDING_IDS.get(building_name)
        if default_id:
            return default_id
        raise ProviderUnavailable("EAMS room occupancy building id is not configured")

    def _building_name_for(self, classroom_name: str) -> str:
        buildings_by_prefix = self.config.get("building_prefixes", {})
        if isinstance(buildings_by_prefix, dict):
            for prefix, building_name in sorted(
                buildings_by_prefix.items(), key=lambda item: len(str(item[0])), reverse=True
            ):
                if classroom_name.startswith(str(prefix)):
                    return str(building_name)
        for building_name in sorted(EAMS_BUILDING_IDS, key=len, reverse=True):
            if classroom_name.startswith(building_name):
                return building_name
        raise ProviderUnavailable("EAMS room occupancy building name is not configured")

    def _building_query_name_for(self, classroom_name: str) -> str:
        building_name = self._building_name_for(classroom_name)
        configured = self.config.get("building_query_names", {})
        if isinstance(configured, dict) and building_name in configured:
            return str(configured[building_name])
        query_name = EAMS_BUILDING_QUERY_NAMES.get(building_name)
        if query_name is not None:
            return query_name
        raise ProviderUnavailable("EAMS room occupancy building query name is not configured")

    def _build_headers(self) -> dict[str, str]:
        configured = self.config.get("headers", {})
        if not isinstance(configured, dict):
            raise ProviderUnavailable("EAMS room occupancy headers config must be an object")
        headers = {
            str(key): str(value)
            for key, value in configured.items()
            if isinstance(key, str) and value is not None
        }
        headers.setdefault("User-Agent", "ClassroomScheduleMiddleware/0.1")
        headers.setdefault("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8")
        return headers


class EamsProbeResult(dict[str, Any]):
    """Dictionary marker for sanitized EAMS probe output."""


def sanitize_url(url: str | None) -> str | None:
    if not url:
        return None
    parsed = urlparse(url)
    if parsed.scheme and parsed.netloc:
        path = parsed.path or "/"
        path = re.sub(r";jsessionid=[^/;?#]*", "", path, flags=re.IGNORECASE)
        return urlunparse((parsed.scheme, parsed.netloc, path, "", "", ""))
    return str(url).split("?", 1)[0].split("#", 1)[0]


def load_json_file(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8-sig") as fp:
            payload = json.load(fp)
    except FileNotFoundError as exc:
        raise ProviderUnavailable(f"{path.name} is missing") from exc
    except json.JSONDecodeError as exc:
        raise ProviderUnavailable(f"{path.name} is not valid JSON") from exc
    if not isinstance(payload, dict):
        raise ProviderUnavailable(f"{path.name} must be a JSON object")
    return payload


def looks_like_placeholder(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    markers = ("example.invalid", "浏览器人工登录后确认", "人工登录后导出", "<browser-cookie-header>")
    return any(marker in value for marker in markers)


def render_query_url(base_url: str, query_template: Any, classroom: str, date_text: str) -> str:
    if not isinstance(query_template, dict) or not query_template:
        return base_url
    query = {
        str(key): render_template_value(value, classroom, date_text)
        for key, value in query_template.items()
    }
    separator = "&" if "?" in base_url else "?"
    return base_url + separator + urlencode(query)


def text_title(text: str) -> str | None:
    match = re.search(r"<title[^>]*>(.*?)</title>", text, re.IGNORECASE | re.DOTALL)
    if match is None:
        return None
    return html.unescape(" ".join(match.group(1).split()))[:120]


def extract_attr(tag: str, attr: str) -> str | None:
    match = re.search(
        rf"{re.escape(attr)}\s*=\s*([\"'])(.*?)\1",
        tag,
        re.IGNORECASE | re.DOTALL,
    )
    if match is None:
        return None
    return html.unescape(match.group(2))


def scan_links(text: str, base_url: str, limit: int = 40) -> list[dict[str, str]]:
    links: list[dict[str, str]] = []
    for match in re.finditer(r"<a\b[^>]*>(.*?)</a>", text, re.IGNORECASE | re.DOTALL):
        open_tag = match.group(0).split(">", 1)[0]
        href = extract_attr(open_tag, "href")
        if not href or href.startswith(("javascript:", "#")):
            continue
        label = html.unescape(re.sub(r"<[^>]+>", "", match.group(1)))
        label = " ".join(label.split())[:80] or "<empty>"
        full_url = urljoin(base_url, href)
        lower_url = full_url.lower()
        if (
            len(links) < 12
            or any(word in label for word in ("课表", "教室", "空闲", "课程", "教学安排", "查询"))
            or any(word in lower_url for word in ("eams", "schedule", "course", "lesson", "classroom", "room"))
        ):
            links.append({"label": label, "url": sanitize_url(full_url) or ""})
        if len(links) >= limit:
            break
    return links


def scan_forms(text: str, limit: int = 8) -> list[dict[str, Any]]:
    forms: list[dict[str, Any]] = []
    for match in re.finditer(r"<form\b[^>]*>(.*?)</form>", text, re.IGNORECASE | re.DOTALL):
        open_tag = match.group(0).split(">", 1)[0]
        fields: list[str] = []
        for field_match in re.finditer(r"<(?:input|select|button)\b[^>]*>", match.group(1), re.IGNORECASE):
            name = extract_attr(field_match.group(0), "name") or extract_attr(field_match.group(0), "id")
            if name:
                fields.append(name[:80])
        forms.append(
            {
                "method": (extract_attr(open_tag, "method") or "GET").upper(),
                "action": sanitize_url(extract_attr(open_tag, "action")),
                "fields": fields[:30],
            }
        )
        if len(forms) >= limit:
            break
    return forms


def scan_interesting_lines(text: str, limit: int = 40) -> list[str]:
    words = ("统一身份认证", "验证码", "登录", "EAMS", "教务", "课表", "教室", "空闲", "课程", "教学安排", "公共")
    lines: list[str] = []
    for line in text.splitlines():
        compact = " ".join(line.strip().split())
        if not compact or not any(word in compact for word in words):
            continue
        compact = re.sub(
            r"(JSESSIONID|Cookie|token|password|pwd)[^\s<>&;]*",
            "<redacted>",
            compact,
            flags=re.IGNORECASE,
        )
        lines.append(compact[:180])
        if len(lines) >= limit:
            break
    return lines


def build_probe_headers(headers_config: Any) -> dict[str, str]:
    headers: dict[str, str] = {}
    if isinstance(headers_config, dict):
        headers = {
            str(key): str(value)
            for key, value in headers_config.items()
            if isinstance(key, str) and value is not None
        }
    headers.setdefault(
        "User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/126 Safari/537.36",
    )
    headers.setdefault("Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8")
    return headers


def probe_login_config(login_file: Path, timeout_seconds: int) -> EamsProbeResult:
    config = load_json_file(login_file)
    result = EamsProbeResult(
        {
            "file": str(login_file),
            "has_username": bool(config.get("username")),
            "has_password": bool(config.get("password")),
            "has_classroom": bool(config.get("classroom")),
            "date": config.get("date") or "auto",
            "login_url": sanitize_url(config.get("login_url")),
            "start_url": sanitize_url(config.get("start_url")),
        }
    )
    login_url = config.get("login_url")
    if not isinstance(login_url, str) or not login_url.strip():
        result["status"] = "missing_login_url"
        return result

    try:
        request = Request(login_url, headers=build_probe_headers({}), method="GET")
        with urlopen(request, timeout=timeout_seconds) as response:
            raw = response.read(800_000)
            content_type = response.headers.get("Content-Type", "")
            final_url = response.geturl()
    except OSError as exc:
        result["status"] = "request_failed"
        result["error"] = type(exc).__name__
        return result

    text = decode_probe_text(raw, content_type)
    result.update(
        {
            "status": "ok",
            "http_status": getattr(response, "status", None),
            "final_url": sanitize_url(final_url),
            "title": text_title(text),
            "has_cas_fields": "execution" in text and "_eventId" in text,
            "has_password_salt": "pwdEncryptSalt" in text,
            "has_captcha_marker": has_captcha_marker(text),
        }
    )
    username = config.get("username")
    if isinstance(username, str) and username:
        result["need_captcha"] = probe_need_captcha(final_url, username, timeout_seconds)
    return result


def decode_probe_text(raw: bytes, content_type: str) -> str:
    match = re.search(r"charset=([\w.-]+)", content_type, re.IGNORECASE)
    candidates = [match.group(1)] if match else []
    candidates += ["utf-8", "gb18030", "gbk"]
    for charset in candidates:
        try:
            return raw.decode(charset, errors="replace")
        except LookupError:
            continue
    return raw.decode("utf-8", errors="replace")


def has_captcha_marker(text: str) -> bool:
    lower = text.lower()
    return (
        "captcha" in lower
        or "verifycode" in lower
        or "randcode" in lower
        or any(word in text for word in ("验证码", "滑块", "安全验证"))
    )


def probe_need_captcha(login_url: str, username: str, timeout_seconds: int) -> bool | str:
    parsed = urlparse(login_url)
    context_path = parsed.path.rsplit("/", 1)[0]
    check_url = urljoin(login_url, context_path + "/checkNeedCaptcha.htl")
    body = urlencode({"username": username}).encode("utf-8")
    headers = build_probe_headers({})
    headers.update(
        {
            "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
            "X-Requested-With": "XMLHttpRequest",
        }
    )
    try:
        request = Request(check_url, data=body, headers=headers, method="POST")
        with urlopen(request, timeout=timeout_seconds) as response:
            text = decode_probe_text(response.read(64_000), response.headers.get("Content-Type", ""))
    except OSError as exc:
        return f"unknown:{type(exc).__name__}"
    lower = text.lower()
    if "true" in lower:
        return True
    if "false" in lower:
        return False
    return "unknown"


def probe_session_config(
    session_file: Path,
    classroom: str,
    date_text: str,
    timeout_seconds: int,
) -> EamsProbeResult:
    config = load_json_file(session_file)
    apply_playwright_storage_state(config, session_file)
    upstream_url = config.get("upstream_url")
    headers_config = config.get("headers", {})
    result = EamsProbeResult(
        {
            "file": str(session_file),
            "upstream_url": sanitize_url(upstream_url if isinstance(upstream_url, str) else None),
            "has_cookie_header": isinstance(headers_config, dict) and bool(headers_config.get("Cookie")),
            "date": date_text,
            "classroom_present": bool(classroom),
        }
    )
    if not isinstance(upstream_url, str) or not upstream_url.strip() or looks_like_placeholder(upstream_url):
        result["status"] = "placeholder_upstream_url"
        return result
    if isinstance(headers_config, dict) and any(looks_like_placeholder(value) for value in headers_config.values()):
        result["status"] = "placeholder_headers"
        return result

    url = render_query_url(upstream_url, config.get("query", {}), classroom, date_text)
    headers = build_probe_headers(headers_config)
    try:
        request = Request(url, headers=headers, method="GET")
        with urlopen(request, timeout=timeout_seconds) as response:
            raw = response.read(1_000_000)
            content_type = response.headers.get("Content-Type", "")
            final_url = response.geturl()
    except HTTPError as exc:
        text = decode_probe_text(exc.read(300_000), exc.headers.get("Content-Type", ""))
        result.update(
            {
                "status": "http_error",
                "http_status": exc.code,
                "final_url": sanitize_url(exc.geturl()),
                "title": text_title(text),
                "looks_like_login": "统一身份认证" in text or "authserver/login" in (sanitize_url(exc.geturl()) or ""),
                "interesting_lines": scan_interesting_lines(text),
            }
        )
        return result
    except OSError as exc:
        result["status"] = "request_failed"
        result["error"] = type(exc).__name__
        return result

    text = decode_probe_text(raw, content_type)
    sanitized_final_url = sanitize_url(final_url) or ""
    result.update(
        {
            "status": "ok",
            "http_status": getattr(response, "status", None),
            "final_url": sanitized_final_url,
            "content_type": content_type.split(";", 1)[0],
            "bytes": len(raw),
            "title": text_title(text),
            "looks_like_login": "统一身份认证" in text or "authserver/login" in sanitized_final_url,
            "looks_like_eams": any(word in text for word in ("EAMS", "教务", "课表", "教室", "课程", "教学安排")),
            "starts_json": text.lstrip().startswith(("{", "[")),
            "forms": scan_forms(text),
            "links_sample": scan_links(text, final_url),
            "interesting_lines": scan_interesting_lines(text),
        }
    )
    if result["starts_json"]:
        try:
            parsed = json.loads(text)
            result["json_shape"] = list(parsed.keys())[:30] if isinstance(parsed, dict) else f"list[{len(parsed)}]"
        except json.JSONDecodeError:
            result["json_shape"] = "invalid"
    return result


def run_eams_provider_probe(args: argparse.Namespace) -> None:
    if not args.session_file:
        raise ProviderUnavailable("EAMS room occupancy probe requires --session-file")
    login_config: dict[str, Any] = {}
    if args.eams_login_file:
        try:
            login_config = load_json_file(Path(args.eams_login_file))
        except ScheduleError:
            login_config = {}
    classroom = args.probe_classroom or str(login_config.get("classroom") or "??103")
    date_text = args.probe_date or str(login_config.get("date") or "auto")
    if date_text == "auto":
        date_text = today_text()
    provider = EamsRoomOccupancyProvider(Path(args.session_file), args.upstream_timeout_seconds)
    data = provider.fetch_schedule(classroom, validate_date(date_text))
    output = {
        "ok": True,
        "provider": provider.name,
        "date": data["date"],
        "classroom": data["classroom"],
        "classroom_name": data["classroom_name"],
        "course_count": len(data["courses"]),
        "courses": data["courses"],
    }
    print(json.dumps(output, ensure_ascii=False, indent=2))


def run_eams_probe(args: argparse.Namespace) -> None:
    login_config: dict[str, Any] = {}
    if args.eams_login_file:
        try:
            login_config = load_json_file(Path(args.eams_login_file))
        except ScheduleError:
            login_config = {}
    classroom = args.probe_classroom or str(login_config.get("classroom") or "A101")
    date_text = args.probe_date or str(login_config.get("date") or "auto")
    if date_text == "auto":
        date_text = today_text()

    output: dict[str, Any] = {"classroom_present": bool(classroom), "date": date_text}
    if args.eams_login_file:
        output["login"] = probe_login_config(Path(args.eams_login_file), args.upstream_timeout_seconds)
    if args.session_file:
        output["session"] = probe_session_config(
            Path(args.session_file),
            classroom,
            date_text,
            args.upstream_timeout_seconds,
        )
    print(json.dumps(output, ensure_ascii=False, indent=2))


class FailingScheduleProvider(ScheduleProvider):
    name = "failing-self-test"

    def fetch_schedule(self, classroom: str, date_text: str) -> dict[str, Any]:
        raise ProviderUnavailable("self-test provider failure")


class FakeUpstreamHandler(BaseHTTPRequestHandler):
    server_version = "ClassroomScheduleFakeUpstream/0.1"

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        classroom = single(query, "room", "A101")
        date_text = single(query, "day", "2026-07-05")
        payload = {
            "classroom_name": f"真实接入测试 {classroom}",
            "courses": [
                {
                    "start": "13:00",
                    "end": "14:30",
                    "name": "真实接入路径测试",
                    "teacher": "测试教师",
                    "group": date_text,
                }
            ],
        }
        if parsed.path == "/json":
            self.send_payload(payload, "application/json; charset=utf-8")
            return
        if parsed.path == "/html":
            body = (
                "<html><body><script id=\"classroom-schedule-json\" type=\"application/json\">"
                + html.escape(json.dumps(payload, ensure_ascii=False))
                + "</script></body></html>"
            )
            self.send_payload(body, "text/html; charset=utf-8")
            return
        self.send_response(HTTPStatus.NOT_FOUND)
        self.end_headers()

    def log_request(self, code: int | str = "-", size: int | str = "-") -> None:
        parsed = urlparse(self.path)
        request_line = f"{self.command} {parsed.path} {self.request_version}"
        print(f'{self.address_string()} - "{request_line}" {code} {size}')

    def send_payload(self, payload: dict[str, Any] | str, content_type: str) -> None:
        if isinstance(payload, str):
            body = payload.encode("utf-8")
        else:
            body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def now_shanghai() -> datetime:
    return datetime.now(SHANGHAI_TZ)


def today_text() -> str:
    return now_shanghai().strftime("%Y-%m-%d")


def updated_at_text() -> str:
    return now_shanghai().isoformat(timespec="seconds")


def render_template_value(value: Any, classroom: str, date_text: str) -> str:
    text = str(value)
    return text.replace("{classroom}", classroom).replace("{date}", date_text)


def decode_response_text(raw: bytes, content_type: str) -> str:
    charset = "utf-8"
    match = re.search(r"charset=([\w.-]+)", content_type, re.IGNORECASE)
    if match:
        charset = match.group(1)
    try:
        return raw.decode(charset)
    except (LookupError, UnicodeDecodeError) as exc:
        raise UpstreamError("manual WebVPN/EAMS response text decoding failed") from exc


def decode_eams_html(raw: bytes, content_type: str) -> str:
    candidates: list[str] = []
    header_match = re.search(r"charset=([^;\s]+)", content_type, re.IGNORECASE)
    if header_match:
        candidates.append(header_match.group(1).strip("\"'"))
    head = raw[:4096].decode("ascii", errors="ignore")
    meta_match = re.search(r"charset\s*=\s*[\"']?([^\"'\s/>;]+)", head, re.IGNORECASE)
    if meta_match:
        candidates.append(meta_match.group(1).strip("\"'"))
    candidates.extend(("utf-8", "gb18030", "gbk"))

    seen: set[str] = set()
    for charset in candidates:
        key = charset.lower()
        if not charset or key in seen:
            continue
        seen.add(key)
        try:
            return raw.decode(charset, errors="replace")
        except LookupError:
            continue
    return raw.decode("utf-8", errors="replace")


def normalize_eams_classroom_name(classroom: str) -> str:
    classroom = classroom.strip()
    if classroom.startswith("尔雅楼"):
        return classroom
    if classroom.startswith("尔雅"):
        suffix = classroom.removeprefix("尔雅").strip()
        if suffix:
            return "尔雅楼" + suffix
    return classroom


def strip_html_text(value: str) -> str:
    value = re.sub(r"<br\s*/?>", "\n", value, flags=re.IGNORECASE)
    value = re.sub(r"<[^>]+>", " ", value)
    return html.unescape(" ".join(value.split()))


def extract_table_rows(text: str) -> list[list[str]]:
    table_matches = list(re.finditer(r"<table\b[^>]*>(.*?)</table>", text, re.IGNORECASE | re.DOTALL))
    if not table_matches:
        raise UpstreamError("EAMS room occupancy page has no table")

    best_rows: list[list[str]] = []
    best_score = -1
    for table_match in table_matches:
        rows: list[list[str]] = []
        for row_match in re.finditer(r"<tr\b[^>]*>(.*?)</tr>", table_match.group(1), re.IGNORECASE | re.DOTALL):
            cells = [
                strip_html_text(cell_match.group(2))
                for cell_match in re.finditer(
                    r"<t[dh]\b([^>]*)>(.*?)</t[dh]>",
                    row_match.group(1),
                    re.IGNORECASE | re.DOTALL,
                )
            ]
            if cells:
                rows.append(cells)
        flattened = " ".join(" ".join(row) for row in rows)
        score = sum(1 for word in ("教室", "周一", "周二", "周三", "周四", "周五", "周六", "周日") if word in flattened)
        if score > best_score:
            best_score = score
            best_rows = rows
    if not best_rows:
        raise UpstreamError("EAMS room occupancy table is empty")
    return best_rows


def parse_section_numbers(value: str) -> list[int]:
    sections: list[int] = []
    for match in re.finditer(r"\d+", value):
        number = int(match.group(0))
        if number in DEFAULT_SECTION_TIMES and number not in sections:
            sections.append(number)
    return sections


def extract_room_occupancy_sections(text: str, classroom_name: str) -> dict[int, list[int]]:
    rows = extract_table_rows(text)
    for row in rows:
        if not row or row[0].strip() != classroom_name:
            continue
        if len(row) < 10:
            raise UpstreamError("EAMS room occupancy row is incomplete")
        return {
            day_index: parse_section_numbers(row[2 + day_index])
            for day_index in range(1, 8)
        }
    raise UpstreamError("EAMS room occupancy page does not contain requested classroom")


def merge_sections(sections: list[int]) -> list[tuple[int, int]]:
    if not sections:
        return []
    ordered = sorted(set(sections))
    merged: list[tuple[int, int]] = []
    start = ordered[0]
    previous = ordered[0]
    for section in ordered[1:]:
        if section == previous + 1:
            previous = section
            continue
        merged.append((start, previous))
        start = previous = section
    merged.append((start, previous))
    return merged


def build_occupancy_courses(sections: list[int]) -> list[dict[str, str]]:
    courses: list[dict[str, str]] = []
    for start_section, end_section in merge_sections(sections):
        start_time = DEFAULT_SECTION_TIMES[start_section][0]
        end_time = DEFAULT_SECTION_TIMES[end_section][1]
        if start_section == end_section:
            name = f"第{start_section}节占用"
        else:
            name = f"第{start_section}-{end_section}节占用"
        courses.append(
            {
                "start": start_time,
                "end": end_time,
                "name": name,
                "teacher": "EAMS 教室资源",
                "group": "真实占用",
            }
        )
    return courses


def merge_syllabus_and_occupancy_courses(
    detailed_courses: list[dict[str, str]],
    occupied_sections: list[int],
) -> list[dict[str, str]]:
    covered_sections: set[int] = set()
    section_by_start = {times[0]: section for section, times in DEFAULT_SECTION_TIMES.items()}
    section_by_end = {times[1]: section for section, times in DEFAULT_SECTION_TIMES.items()}
    for course in detailed_courses:
        first_section = section_by_start.get(course.get("start", ""))
        last_section = section_by_end.get(course.get("end", ""))
        if first_section is None or last_section is None or last_section < first_section:
            continue
        covered_sections.update(range(first_section, last_section + 1))

    unresolved_sections = [
        section
        for section in occupied_sections
        if section not in covered_sections
    ]
    return dedupe_courses(
        [*detailed_courses, *build_occupancy_courses(unresolved_sections)]
    )[:MAX_COURSES]


def syllabus_total_pages(text: str, fallback_page_size: int) -> int | None:
    match = re.search(r"page_grid\w+\.pageInfo\((\d+),(\d+),(\d+)\)", text)
    if match is None:
        return None
    page_size = int(match.group(2) or fallback_page_size)
    total_rows = int(match.group(3))
    if page_size <= 0:
        page_size = fallback_page_size
    return max(1, ((total_rows + page_size - 1) // page_size))


def extract_syllabus_lessons(text: str) -> list[dict[str, Any]]:
    contents = {
        match.group(1): html.unescape(match.group(2))
        for match in re.finditer(r"contents\['(\d+)'\]\s*=\s*'([^']*)'", text)
    }
    lessons_by_id: dict[str, dict[str, Any]] = {}
    for row_match in re.finditer(r"<tr\b[^>]*>(.*?)</tr>", text, re.IGNORECASE | re.DOTALL):
        row_html = row_match.group(1)
        lesson_id = ""
        for input_match in re.finditer(r"<input\b[^>]*>", row_html, re.IGNORECASE):
            attrs = html_attrs(input_match.group(0))
            if attrs.get("name") == "lesson.id" and attrs.get("value", "").isdigit():
                lesson_id = attrs["value"]
                break
        if not lesson_id:
            continue
        cells = [
            strip_html_text(cell_match.group(2))
            for cell_match in re.finditer(
                r"<t[dh]\b([^>]*)>(.*?)</t[dh]>",
                row_html,
                re.IGNORECASE | re.DOTALL,
            )
        ]
        if len(cells) < 6:
            continue
        lessons_by_id[lesson_id] = {
            "lesson_id": lesson_id,
            "course_no": cells[1] if len(cells) > 1 else "",
            "course_name": cells[2] if len(cells) > 2 else "",
            "course_type": cells[3] if len(cells) > 3 else "",
            "group": cells[4] if len(cells) > 4 else "",
            "teacher": cells[5] if len(cells) > 5 else "",
            "arrange_text": contents.get(lesson_id, ""),
        }
    for lesson_id, arrange_text in contents.items():
        lessons_by_id.setdefault(
            lesson_id,
            {
                "lesson_id": lesson_id,
                "course_no": "",
                "course_name": "",
                "course_type": "",
                "group": "",
                "teacher": "",
                "arrange_text": arrange_text,
            },
        )
    return list(lessons_by_id.values())


def html_attrs(tag: str) -> dict[str, str]:
    return {
        match.group(1).lower(): html.unescape(match.group(2) or match.group(3) or match.group(4) or "")
        for match in re.finditer(
            r"([\w.:-]+)\s*=\s*(?:\"([^\"]*)\"|'([^']*)'|([^\s>]+))",
            tag,
        )
    }


def week_spec_matches(spec: str, target_week: int) -> bool:
    spec = spec.strip().strip("[]")
    if not spec:
        return False
    for part in (item.strip() for item in spec.split(",") if item.strip()):
        parity: int | None = None
        if part.endswith("单"):
            parity = 1
            part = part[:-1]
        elif part.endswith("双"):
            parity = 0
            part = part[:-1]

        numbers = [int(number) for number in re.findall(r"\d+", part)]
        if not numbers:
            continue
        if len(numbers) == 1:
            in_range = target_week == numbers[0]
        else:
            in_range = numbers[0] <= target_week <= numbers[1]
        if in_range and (parity is None or target_week % 2 == parity):
            return True
    return False


def iter_syllabus_arrangements(arrange_text: str) -> list[dict[str, str]]:
    normalized = re.sub(r"<br\s*/?>", "\n", arrange_text, flags=re.IGNORECASE)
    normalized = strip_html_text(normalized)
    pattern = re.compile(
        r"(?P<teacher>\S+)\s+"
        r"(?P<weekday>星期[一二三四五六日]|周[一二三四五六日])\s+"
        r"(?P<section>\d{1,2}-\d{1,2})\s+"
        r"(?P<weeks>(?:\[[^\]]+\]|\d+(?:,\d+)*)[单双]?)\s+"
        r"(?P<classroom>\S+)"
    )
    return [match.groupdict() for match in pattern.finditer(normalized)]


def build_syllabus_courses(
    lessons: list[dict[str, Any]],
    classroom_name: str,
    target_week: int,
    target_day: int,
) -> list[dict[str, str]]:
    courses: list[dict[str, str]] = []
    for lesson in lessons:
        arrange_text = str(lesson.get("arrange_text") or "")
        if classroom_name not in arrange_text:
            continue
        for item in iter_syllabus_arrangements(arrange_text):
            if item["classroom"] != classroom_name:
                continue
            if EAMS_WEEKDAY_NAMES.get(item["weekday"]) != target_day:
                continue
            if not week_spec_matches(item["weeks"], target_week):
                continue

            section_numbers = parse_section_numbers(item["section"])
            if not section_numbers:
                continue
            first_section = min(section_numbers)
            last_section = max(section_numbers)
            if first_section not in DEFAULT_SECTION_TIMES or last_section not in DEFAULT_SECTION_TIMES:
                continue

            course_name = str(lesson.get("course_name") or "").strip()
            if not course_name:
                course_name = "未命名课程"
            group = str(lesson.get("group") or "").strip()
            teacher = str(item.get("teacher") or lesson.get("teacher") or "").strip()
            courses.append(
                {
                    "start": DEFAULT_SECTION_TIMES[first_section][0],
                    "end": DEFAULT_SECTION_TIMES[last_section][1],
                    "name": course_name,
                    "teacher": teacher or "未知教师",
                    "group": group or "真实课程",
                }
            )
    return courses


def dedupe_courses(courses: list[dict[str, str]]) -> list[dict[str, str]]:
    seen: set[tuple[str, str, str, str, str]] = set()
    unique: list[dict[str, str]] = []
    for course in sorted(courses, key=lambda item: (item["start"], item["end"], item["name"])):
        key = (
            course.get("start", ""),
            course.get("end", ""),
            course.get("name", ""),
            course.get("teacher", ""),
            course.get("group", ""),
        )
        if key in seen:
            continue
        seen.add(key)
        unique.append(course)
    return unique


def parse_upstream_payload(text: str, content_type: str) -> dict[str, Any]:
    stripped = text.strip()
    if "json" in content_type.lower() or stripped.startswith("{"):
        try:
            payload = json.loads(stripped)
        except json.JSONDecodeError as exc:
            raise UpstreamError("manual WebVPN/EAMS JSON response is invalid") from exc
        return extract_schedule_record(payload)

    match = EMBEDDED_JSON_RE.search(text)
    if match is None:
        raise UpstreamError("manual WebVPN/EAMS HTML response does not contain schedule JSON")
    embedded = html.unescape(match.group(1)).strip()
    try:
        payload = json.loads(embedded)
    except json.JSONDecodeError as exc:
        raise UpstreamError("manual WebVPN/EAMS embedded schedule JSON is invalid") from exc
    return extract_schedule_record(payload)


def extract_schedule_record(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise UpstreamError("manual WebVPN/EAMS schedule payload must be an object")
    if isinstance(payload.get("schedule"), dict):
        return payload["schedule"]
    if isinstance(payload.get("data"), dict):
        return payload["data"]
    return payload


def validate_date(date_text: str) -> str:
    if not DATE_RE.match(date_text):
        raise BadRequest("date must use YYYY-MM-DD")
    try:
        datetime.strptime(date_text, "%Y-%m-%d")
    except ValueError as exc:
        raise BadRequest("date is not a valid calendar day") from exc
    return date_text


def validate_classroom(classroom: str) -> str:
    classroom = classroom.strip()
    if not classroom:
        raise BadRequest("classroom is required")
    if len(classroom) > 64:
        raise BadRequest("classroom is too long")
    return classroom


def require_text(record: dict[str, Any], field: str) -> str:
    value = record.get(field)
    if not isinstance(value, str) or not value.strip():
        raise UpstreamError(f"course field {field} is required")
    return value.strip()


def optional_text(record: dict[str, Any], field: str) -> str | None:
    value = record.get(field)
    if value is None:
        return None
    if not isinstance(value, str):
        return None
    value = value.strip()
    return value or None


def normalize_course(record: dict[str, Any]) -> dict[str, str]:
    start = require_text(record, "start")
    end = require_text(record, "end")
    name = require_text(record, "name")
    if not TIME_RE.match(start) or not TIME_RE.match(end):
        raise UpstreamError("course time must use HH:MM")

    course = {"start": start, "end": end, "name": name}
    for field in ("teacher", "group"):
        value = optional_text(record, field)
        if value is not None:
            course[field] = value
    return course


def normalize_schedule(classroom: str, date_text: str, record: dict[str, Any]) -> dict[str, Any]:
    classroom_name = optional_text(record, "classroom_name") or classroom
    courses_value = record.get("courses", [])
    if not isinstance(courses_value, list):
        raise UpstreamError("courses must be an array")

    courses: list[dict[str, str]] = []
    for item in courses_value[:MAX_COURSES]:
        if not isinstance(item, dict):
            raise UpstreamError("course item must be an object")
        courses.append(normalize_course(item))
    courses.sort(key=lambda item: item["start"])

    return {
        "date": date_text,
        "classroom": classroom,
        "classroom_name": classroom_name,
        "updated_at": updated_at_text(),
        "courses": courses,
    }


def guess_lan_ip() -> str:
    try:
        with socket(AF_INET, SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def single(query: dict[str, list[str]], name: str, default: str = "") -> str:
    values = query.get(name)
    if not values:
        return default
    return values[0]


def validate_alert_text(value: Any, field: str, max_len: int) -> str:
    if value is None:
        return ""
    if not isinstance(value, str):
        raise BadRequest(f"{field} must be a string")
    text = value.strip()
    if len(text) > max_len:
        raise BadRequest(f"{field} is too long")
    if not ALERT_TEXT_RE.fullmatch(text):
        raise BadRequest(f"{field} contains unsupported characters")
    return text


def validate_alert_payload(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise BadRequest("alert payload must be a JSON object")

    reason = validate_alert_text(payload.get("reason"), "reason", 48)
    detail = validate_alert_text(payload.get("detail"), "detail", 96)
    message = validate_alert_text(payload.get("message"), "message", 160)
    if not reason:
        raise BadRequest("reason is required")

    timestamp_ms = payload.get("timestamp_ms", 0)
    if not isinstance(timestamp_ms, (int, float)) or timestamp_ms < 0:
        raise BadRequest("timestamp_ms must be a non-negative number")

    return {
        "reason": reason,
        "detail": detail,
        "message": message,
        "timestamp_ms": int(timestamp_ms),
    }


class ScheduleHandler(BaseHTTPRequestHandler):
    server_version = "ClassroomScheduleReal/0.1"

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        try:
            if parsed.path == "/health":
                self.send_json(
                    {
                        "ok": True,
                        "service": "real-classroom-schedule",
                        "provider": self.server.provider.name,
                    }
                )
                return
            if parsed.path != self.server.api_path:
                raise NotFound("not found")
            self.handle_schedule(parse_qs(parsed.query))
        except ScheduleError as exc:
            self.send_error_json(exc)
        except Exception:
            self.log_message("unexpected server error")
            self.send_error_json(ScheduleError("internal server error"))

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        try:
            if parsed.path != self.server.alert_path:
                raise NotFound("not found")
            self.handle_parent_call_alert()
        except ScheduleError as exc:
            self.send_error_json(exc)
        except Exception:
            self.log_message("unexpected alert server error")
            self.send_error_json(ScheduleError("internal server error"))

    def handle_schedule(self, query: dict[str, list[str]]) -> None:
        token = single(query, "token")
        if token != self.server.api_token:
            raise Unauthorized("invalid token")

        classroom = validate_classroom(single(query, "classroom"))
        date_text = validate_date(single(query, "date", today_text()))

        try:
            data = self.server.provider.fetch_schedule(classroom, date_text)
            self.server.cache.set(classroom, date_text, data)
            self.send_json(data)
        except ScheduleError as exc:
            cached = self.server.cache.get(classroom, date_text)
            if cached is not None:
                self.send_json(cached)
                return
            raise exc

    def handle_parent_call_alert(self) -> None:
        token = self.headers.get("X-Alert-Token", "")
        if token != self.server.alert_token:
            raise Unauthorized("invalid token")

        content_type = self.headers.get("Content-Type", "")
        if "application/json" not in content_type.lower():
            raise BadRequest("content type must be application/json")

        content_length_text = self.headers.get("Content-Length", "")
        try:
            content_length = int(content_length_text)
        except ValueError as exc:
            raise BadRequest("content length is required") from exc
        if content_length <= 0:
            raise BadRequest("request body is required")
        if content_length > MAX_ALERT_BODY_BYTES:
            raise PayloadTooLarge("alert payload is too large")

        try:
            payload = json.loads(self.rfile.read(content_length).decode("utf-8-sig"))
        except UnicodeDecodeError as exc:
            raise BadRequest("request body must be UTF-8") from exc
        except json.JSONDecodeError as exc:
            raise BadRequest("request body must be valid JSON") from exc

        alert = validate_alert_payload(payload)
        now = updated_at_text()
        self.log_message(
            "parent call alert accepted reason=%s detail_len=%d message_len=%d",
            alert["reason"],
            len(alert["detail"]),
            len(alert["message"]),
        )
        self.send_json(
            {
                "ok": True,
                "status": "accepted",
                "mode": self.server.alert_mode,
                "received_at": now,
            }
        )

    def log_message(self, fmt: str, *args: object) -> None:
        message = fmt % args if args else fmt
        print(f"{self.address_string()} - {message}")

    def log_request(self, code: int | str = "-", size: int | str = "-") -> None:
        parsed = urlparse(self.path)
        request_line = f"{self.command} {parsed.path} {self.request_version}"
        print(f'{self.address_string()} - "{request_line}" {code} {size}')

    def send_error_json(self, exc: ScheduleError) -> None:
        self.send_json({"error": exc.code, "message": exc.message}, exc.status)

    def send_json(self, data: dict[str, Any], status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def make_provider(args: argparse.Namespace) -> ScheduleProvider:
    if args.provider == "fixture":
        return FixtureScheduleProvider(Path(args.fixture))
    if args.provider == "manual-session":
        session_file = Path(args.session_file) if args.session_file else None
        return ManualSessionEamsProvider(session_file, args.upstream_timeout_seconds)
    if args.provider == "eams-room-occupancy":
        session_file = Path(args.session_file) if args.session_file else None
        return EamsRoomOccupancyProvider(session_file, args.upstream_timeout_seconds)
    raise BadRequest(f"unsupported provider: {args.provider}")


def run_self_test(args: argparse.Namespace) -> None:
    if args.provider not in ("fixture", "manual-session"):
        raise BadRequest("self-test only supports fixture or manual-session providers")
    provider = make_provider(args)
    sample = provider.fetch_schedule("A101", "2026-07-05")
    assert sample["date"] == "2026-07-05"
    assert sample["classroom"] == "A101"
    assert isinstance(sample["courses"], list)
    assert sample["courses"] == sorted(sample["courses"], key=lambda item: item["start"])
    sanitized = sanitize_url("https://example.invalid/eams/home;jsessionid=secret?token=secret")
    assert sanitized == "https://example.invalid/eams/home"
    assert "secret" not in sanitized

    if provider.name == "fixture":
        for date_text in ("2026-07-16", "2026-07-17"):
            device_sample = provider.fetch_schedule("尔雅楼103", date_text)
            assert device_sample["classroom"] == "尔雅楼103"
            assert device_sample["date"] == date_text
            assert device_sample["courses"]

    empty = provider.fetch_schedule("EMPTY", "2026-07-05")
    assert empty["courses"] == []

    unknown = provider.fetch_schedule("UNKNOWN", "2026-07-05")
    assert unknown["classroom"] == "UNKNOWN"
    assert unknown["courses"] == []

    validate_date("2026-07-05")
    try:
        validate_date("2026-99-99")
    except BadRequest:
        pass
    else:
        raise AssertionError("invalid date should fail")

    run_http_self_test(args)
    run_syllabus_parser_self_test()
    print("Self-test passed.")


def read_json_url(url: str) -> tuple[int, dict[str, Any]]:
    try:
        with urlopen(url, timeout=5) as response:
            body = response.read().decode("utf-8")
            return response.status, json.loads(body)
    except HTTPError as exc:
        body = exc.read().decode("utf-8")
        return exc.code, json.loads(body)


def run_http_self_test(args: argparse.Namespace) -> None:
    provider = make_provider(args)
    server = ThreadingHTTPServer(("127.0.0.1", 0), ScheduleHandler)
    server.api_path = args.path
    server.api_token = "self-test-token"
    server.alert_path = args.alert_path
    server.alert_token = "self-test-alert-token"
    server.alert_mode = args.alert_mode
    server.provider = provider
    server.cache = ScheduleCache(args.cache_ttl_seconds)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    base_url = f"http://127.0.0.1:{server.server_port}"
    try:
        status, body = read_json_url(f"{base_url}/health")
        assert status == HTTPStatus.OK
        assert body["ok"] is True
        assert body["provider"] == provider.name

        status, body = read_json_url(
            f"{base_url}{args.path}?classroom=A101&date=2026-07-05&token=self-test-token"
        )
        assert status == HTTPStatus.OK
        assert body["classroom"] == "A101"
        assert body["date"] == "2026-07-05"
        assert body["courses"]

        if provider.name == "fixture":
            query = urlencode(
                {
                    "classroom": "尔雅楼103",
                    "date": "2026-07-17",
                    "token": "self-test-token",
                }
            )
            status, body = read_json_url(f"{base_url}{args.path}?{query}")
            assert status == HTTPStatus.OK
            assert body["classroom"] == "尔雅楼103"
            assert body["date"] == "2026-07-17"
            assert body["courses"]

        status, body = read_json_url(
            f"{base_url}{args.path}?classroom=EMPTY&date=2026-07-05&token=self-test-token"
        )
        assert status == HTTPStatus.OK
        assert body["courses"] == []

        status, body = read_json_url(f"{base_url}{args.path}?classroom=A101&token=wrong")
        assert status == HTTPStatus.UNAUTHORIZED
        assert body["error"] == "unauthorized"
        assert "self-test-token" not in json.dumps(body)

        status, body = read_json_url(
            f"{base_url}{args.path}?classroom=A101&date=2026-99-99&token=self-test-token"
        )
        assert status == HTTPStatus.BAD_REQUEST
        assert body["error"] == "bad_request"

        status, body = read_json_url(f"{base_url}/missing")
        assert status == HTTPStatus.NOT_FOUND
        assert body["error"] == "not_found"

        alert_body = json.dumps(
            {
                "reason": "mq2_alarm",
                "detail": "MQ-2 detected smoke or combustible gas",
                "message": "环境监测检测到烟雾或可燃气体异常，请及时确认。",
                "timestamp_ms": 123456,
            }
        ).encode("utf-8")
        alert_request = Request(
            f"{base_url}{args.alert_path}",
            data=alert_body,
            headers={
                "Content-Type": "application/json",
                "X-Alert-Token": "self-test-alert-token",
            },
            method="POST",
        )
        with urlopen(alert_request, timeout=5) as response:
            body = json.loads(response.read().decode("utf-8"))
            assert response.status == HTTPStatus.OK
            assert body["ok"] is True
            assert body["mode"] == args.alert_mode

        bom_alert_request = Request(
            f"{base_url}{args.alert_path}",
            data=b"\xef\xbb\xbf" + alert_body,
            headers={
                "Content-Type": "application/json",
                "X-Alert-Token": "self-test-alert-token",
            },
            method="POST",
        )
        with urlopen(bom_alert_request, timeout=5) as response:
            body = json.loads(response.read().decode("utf-8"))
            assert response.status == HTTPStatus.OK
            assert body["ok"] is True

        bad_alert_request = Request(
            f"{base_url}{args.alert_path}",
            data=alert_body,
            headers={
                "Content-Type": "application/json",
                "X-Alert-Token": "wrong",
            },
            method="POST",
        )
        status, body = read_json_url_request(bad_alert_request)
        assert status == HTTPStatus.UNAUTHORIZED
        assert body["error"] == "unauthorized"
        assert "self-test-alert-token" not in json.dumps(body)

        missing_reason_alert_request = Request(
            f"{base_url}{args.alert_path}",
            data=json.dumps({"detail": "self-test"}).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "X-Alert-Token": "self-test-alert-token",
            },
            method="POST",
        )
        status, body = read_json_url_request(missing_reason_alert_request)
        assert status == HTTPStatus.BAD_REQUEST
        assert body["error"] == "bad_request"

        oversized_alert_request = Request(
            f"{base_url}{args.alert_path}",
            data=(b"{" + b'"reason":"mq2_alarm","detail":"' + (b"x" * MAX_ALERT_BODY_BYTES) + b'"}'),
            headers={
                "Content-Type": "application/json",
                "X-Alert-Token": "self-test-alert-token",
            },
            method="POST",
        )
        status, body = read_json_url_request(oversized_alert_request)
        assert status == HTTPStatus.REQUEST_ENTITY_TOO_LARGE
        assert body["error"] == "payload_too_large"

        non_json_alert_request = Request(
            f"{base_url}{args.alert_path}",
            data=b"reason=mq2_alarm",
            headers={
                "Content-Type": "text/plain",
                "X-Alert-Token": "self-test-alert-token",
            },
            method="POST",
        )
        status, body = read_json_url_request(non_json_alert_request)
        assert status == HTTPStatus.BAD_REQUEST
        assert body["error"] == "bad_request"

        server.provider = FailingScheduleProvider()
        status, body = read_json_url(
            f"{base_url}{args.path}?classroom=A101&date=2026-07-05&token=self-test-token"
        )
        assert status == HTTPStatus.OK
        assert body["cached"] is True
        assert body["classroom"] == "A101"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)

    run_manual_session_self_test(args)


def read_json_url_request(request: Request) -> tuple[int, dict[str, Any]]:
    try:
        with urlopen(request, timeout=5) as response:
            body = response.read().decode("utf-8")
            return response.status, json.loads(body)
    except HTTPError as exc:
        body = exc.read().decode("utf-8")
        return exc.code, json.loads(body)


def run_manual_session_self_test(args: argparse.Namespace) -> None:
    upstream = ThreadingHTTPServer(("127.0.0.1", 0), FakeUpstreamHandler)
    thread = threading.Thread(target=upstream.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as temp_dir:
            session_path = Path(temp_dir) / "eams-session.json"
            storage_state_path = Path(temp_dir) / "eams-playwright-state.json"
            storage_state_path.write_text(
                json.dumps(
                    {
                        "cookies": [
                            {
                                "name": "SESSION",
                                "value": "fresh-self-test",
                                "domain": "127.0.0.1",
                                "path": "/",
                                "expires": -1,
                            },
                            {
                                "name": "OTHER",
                                "value": "ignored",
                                "domain": "example.invalid",
                                "path": "/",
                                "expires": -1,
                            },
                        ],
                        "origins": [],
                    }
                ),
                encoding="utf-8",
            )
            session = {
                "upstream_url": f"http://127.0.0.1:{upstream.server_port}/json",
                "query": {"room": "{classroom}", "day": "{date}"},
                "headers": {"Cookie": "SESSION=stale-self-test"},
                "playwright_storage_state": storage_state_path.name,
            }
            session_path.write_text(json.dumps(session), encoding="utf-8")
            provider = ManualSessionEamsProvider(session_path, timeout_seconds=5)
            assert provider._build_headers()["Cookie"] == "SESSION=fresh-self-test"
            data = provider.fetch_schedule("A101", "2026-07-05")
            assert data["classroom"] == "A101"
            assert data["classroom_name"] == "真实接入测试 A101"
            assert data["courses"][0]["name"] == "真实接入路径测试"

            session["upstream_url"] = f"http://127.0.0.1:{upstream.server_port}/html"
            session_path.write_text(json.dumps(session), encoding="utf-8")
            provider = ManualSessionEamsProvider(session_path, timeout_seconds=5)
            data = provider.fetch_schedule("B202", "2026-07-06")
            assert data["classroom"] == "B202"
            assert data["classroom_name"] == "真实接入测试 B202"
            assert data["courses"][0]["group"] == "2026-07-06"
    finally:
        upstream.shutdown()
        upstream.server_close()
        thread.join(timeout=5)


def run_syllabus_parser_self_test() -> None:
    html_text = """
    <script>
      contents['1528639']='符萌萌 星期一 7-8 18  尔雅楼103  <br>符萌萌 星期三 5-6 [9-18]  尔雅楼402  ';
      contents['1527793']='尚航 星期日 5-6 6  尔雅楼103  ';
      page_grid10181745991.pageInfo(1,500,3333);
    </script>
    <table>
      <tr><th></th><th>课程代码</th><th>课程名称</th><th>课程类别</th><th>教学班</th><th>教师</th></tr>
      <tr>
        <td><input value="1528639" type="checkbox" name="lesson.id"></td>
        <td>120001.01</td><td>组织行为学</td><td>专业课</td>
        <td>班级:会计25-3 会计25-4 会计25-5</td><td>符萌萌</td>
      </tr>
      <tr>
        <td><input type="checkbox" name="lesson.id" value="1527793"></td>
        <td>120002.01</td><td>形势与政策（8）</td><td>公共课</td>
        <td>班级:计算22-1 计算22-2</td><td>尚航</td>
      </tr>
    </table>
    """
    lessons = extract_syllabus_lessons(html_text)
    monday_courses = build_syllabus_courses(
        lessons,
        classroom_name="尔雅楼103",
        target_week=18,
        target_day=1,
    )
    assert monday_courses == [
        {
            "start": "16:10",
            "end": "17:50",
            "name": "组织行为学",
            "teacher": "符萌萌",
            "group": "班级:会计25-3 会计25-4 会计25-5",
        }
    ]

    merged_courses = merge_syllabus_and_occupancy_courses(
        monday_courses,
        [1, 2, 3, 4, 7, 8, 9, 10],
    )
    assert merged_courses == [
        {
            "start": "08:00",
            "end": "11:50",
            "name": "第1-4节占用",
            "teacher": "EAMS 教室资源",
            "group": "真实占用",
        },
        {
            "start": "16:10",
            "end": "17:50",
            "name": "组织行为学",
            "teacher": "符萌萌",
            "group": "班级:会计25-3 会计25-4 会计25-5",
        },
        {
            "start": "19:00",
            "end": "20:40",
            "name": "第9-10节占用",
            "teacher": "EAMS 教室资源",
            "group": "真实占用",
        },
    ]

    occupancy_only = merge_syllabus_and_occupancy_courses([], [5, 6, 7, 8])
    assert occupancy_only == [
        {
            "start": "14:00",
            "end": "17:50",
            "name": "第5-8节占用",
            "teacher": "EAMS 教室资源",
            "group": "真实占用",
        }
    ]

    provider = object.__new__(EamsRoomOccupancyProvider)
    provider.config = {
        "first_week_start": "2026-03-02",
        "iWeek": 20,
    }
    assert provider._week_for_date("2026-03-02") == 1
    assert provider._week_for_date("2026-04-06") == 6
    assert provider._week_for_date("2026-07-16") == 20
    for building_name, building_id in EAMS_BUILDING_IDS.items():
        assert provider._building_name_for(f"{building_name}103") == building_name
        assert provider._building_id_for(f"{building_name}103") == building_id
        assert provider._building_query_name_for(f"{building_name}103") == EAMS_BUILDING_QUERY_NAMES[building_name]

    provider.config["buildings"] = {"博文楼": "107"}
    provider.config["building_query_names"] = {"博文楼": "自定义博文楼"}
    assert provider._building_id_for("博文楼105") == "107"
    assert provider._building_query_name_for("博文楼105") == "自定义博文楼"

    parity_arrangements = iter_syllabus_arrangements(
        "韩旭 星期一 9-10 [6-12]双 尔雅楼101"
    )
    assert parity_arrangements == [
        {
            "teacher": "韩旭",
            "weekday": "星期一",
            "section": "9-10",
            "weeks": "[6-12]双",
            "classroom": "尔雅楼101",
        }
    ]
    assert week_spec_matches(parity_arrangements[0]["weeks"], 6)
    assert not week_spec_matches(parity_arrangements[0]["weeks"], 7)

    sunday_courses = build_syllabus_courses(
        lessons,
        classroom_name="尔雅楼103",
        target_week=18,
        target_day=7,
    )
    assert sunday_courses == []
    assert syllabus_total_pages(html_text, EAMS_SYLLABUS_PAGE_SIZE) == 7


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the real classroom schedule middleware.")
    parser.add_argument("--host", default=os.getenv("SCHEDULE_HOST", "0.0.0.0"))
    parser.add_argument("--port", default=int(os.getenv("SCHEDULE_PORT", "8080")), type=int)
    parser.add_argument("--path", default=os.getenv("SCHEDULE_API_PATH", DEFAULT_PATH))
    parser.add_argument("--token", default=os.getenv("SCHEDULE_API_TOKEN", DEFAULT_TOKEN))
    parser.add_argument("--alert-path", default=os.getenv("PARENT_CALL_ALERT_API_PATH", DEFAULT_ALERT_PATH))
    parser.add_argument("--alert-token", default=os.getenv("PARENT_CALL_ALERT_API_TOKEN", DEFAULT_TOKEN))
    parser.add_argument(
        "--alert-mode",
        choices=("mock",),
        default=os.getenv("PARENT_CALL_ALERT_MODE", "mock"),
        help="Parent call alert handling mode. mock accepts and records without dialing.",
    )
    parser.add_argument(
        "--provider",
        choices=("fixture", "manual-session", "eams-room-occupancy"),
        default=os.getenv("SCHEDULE_PROVIDER", "fixture"),
    )
    parser.add_argument(
        "--fixture",
        default=os.getenv("SCHEDULE_FIXTURE", str(DEFAULT_FIXTURE)),
        help="Path to sanitized fixture JSON for fixture provider.",
    )
    parser.add_argument(
        "--session-file",
        default=os.getenv("SCHEDULE_EAMS_SESSION_FILE", ""),
        help="Path to an untracked manual WebVPN/EAMS session config.",
    )
    parser.add_argument(
        "--upstream-timeout-seconds",
        default=int(os.getenv("SCHEDULE_UPSTREAM_TIMEOUT_SECONDS", "10")),
        type=int,
        help="Timeout for manual-session upstream requests.",
    )
    parser.add_argument(
        "--cache-ttl-seconds",
        default=int(os.getenv("SCHEDULE_CACHE_TTL_SECONDS", "600")),
        type=int,
    )
    parser.add_argument("--self-test", action="store_true", help="Run fixture/provider contract checks.")
    parser.add_argument(
        "--probe-eams",
        action="store_true",
        help="Run a sanitized WebVPN/EAMS login/session probe and exit.",
    )
    parser.add_argument(
        "--probe-eams-provider",
        action="store_true",
        help="Fetch one sanitized EAMS room occupancy result and exit.",
    )
    parser.add_argument(
        "--eams-login-file",
        default=os.getenv("SCHEDULE_EAMS_LOGIN_FILE", ""),
        help="Path to an untracked login probe config.",
    )
    parser.add_argument(
        "--probe-classroom",
        default=os.getenv("SCHEDULE_PROBE_CLASSROOM", ""),
        help="Classroom value for sanitized EAMS probe; defaults to login config.",
    )
    parser.add_argument(
        "--probe-date",
        default=os.getenv("SCHEDULE_PROBE_DATE", ""),
        help="YYYY-MM-DD date for sanitized EAMS probe; defaults to login config or today.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])

    if args.self_test:
        run_self_test(args)
        return 0

    if args.probe_eams:
        run_eams_probe(args)
        return 0

    if args.probe_eams_provider:
        run_eams_provider_probe(args)
        return 0

    provider = make_provider(args)
    server = ThreadingHTTPServer((args.host, args.port), ScheduleHandler)
    server.api_path = args.path
    server.api_token = args.token
    server.alert_path = args.alert_path
    server.alert_token = args.alert_token
    server.alert_mode = args.alert_mode
    server.provider = provider
    server.cache = ScheduleCache(args.cache_ttl_seconds)

    lan_ip = guess_lan_ip()
    print("Real classroom schedule middleware is running.")
    print(f"  Bind: http://{args.host}:{args.port}")
    print(f"  LAN:  http://{lan_ip}:{args.port}{args.path}?classroom=A101&token=<redacted>")
    print(f"  Alert: http://{lan_ip}:{args.port}{args.alert_path} X-Alert-Token=<redacted>")
    print(f"  Provider: {provider.name}")
    print(f"  Alert mode: {args.alert_mode}")
    print("Press Ctrl+C to stop.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping real classroom schedule middleware.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
