#!/usr/bin/env python3
"""Refresh the local EAMS browser session after user-completed verification."""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from real_classroom_schedule_server import (
    ScheduleError,
    load_json_file,
    probe_session_config,
    today_text,
)


DEFAULT_LOGIN_TIMEOUT_SECONDS = 600
DEFAULT_UPSTREAM_TIMEOUT_SECONDS = 12
USERNAME_SELECTORS = ("#username", "input[name='username']", "input[type='text']")
PASSWORD_SELECTORS = ("#password", "input[name='password']", "input[type='password']")


class SessionRefreshError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def find_edge_executable(configured: str = "") -> Path:
    if configured:
        path = Path(configured).expanduser()
        if path.is_file():
            return path.resolve()
        raise SessionRefreshError("edge_missing", "The configured Microsoft Edge executable was not found.")

    candidates = []
    for variable in ("PROGRAMFILES(X86)", "PROGRAMFILES", "LOCALAPPDATA"):
        root = os.getenv(variable)
        if root:
            candidates.append(Path(root) / "Microsoft" / "Edge" / "Application" / "msedge.exe")
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise SessionRefreshError("edge_missing", "Microsoft Edge was not found in a standard installation path.")


def resolve_storage_state_path(session_file: Path, session_config: dict[str, Any]) -> Path:
    configured = str(session_config.get("playwright_storage_state") or "").strip()
    if not configured:
        raise SessionRefreshError(
            "storage_state_missing",
            "The EAMS session config does not define playwright_storage_state.",
        )
    path = Path(configured).expanduser()
    if not path.is_absolute():
        path = session_file.parent / path
    return path.resolve()


def is_login_url(current_url: str, login_url: str) -> bool:
    current = urlparse(current_url)
    login = urlparse(login_url)
    return bool(
        current.hostname
        and login.hostname
        and current.hostname.lower() == login.hostname.lower()
        and current.path.startswith(login.path.rsplit("/", 1)[0])
    )


def is_eams_url(current_url: str, start_url: str) -> bool:
    current = urlparse(current_url)
    start = urlparse(start_url)
    return bool(
        current.hostname
        and start.hostname
        and current.hostname.lower() == start.hostname.lower()
        and "/eams/" in current.path.lower()
        and "login" not in current.path.lower()
    )


def title_looks_like_eams(title: str) -> bool:
    normalized = title.strip().lower()
    return "教学管理系统" in title or "eams" in normalized


def page_looks_like_eams(page: Any, start_url: str) -> bool:
    if is_eams_url(page.url, start_url):
        return True
    try:
        return title_looks_like_eams(page.title(timeout=1_000))
    except Exception:
        return False


def fill_first_visible(page: Any, selectors: tuple[str, ...], value: str) -> bool:
    for selector in selectors:
        locator = page.locator(selector).first
        try:
            if locator.count() and locator.is_visible(timeout=250):
                locator.fill(value, timeout=2_000)
                return True
        except Exception:
            continue
    return False


def fill_login_credentials(page: Any, username: str, password: str) -> bool:
    username_filled = fill_first_visible(page, USERNAME_SELECTORS, username)
    password_filled = fill_first_visible(page, PASSWORD_SELECTORS, password)
    return username_filled and password_filled


def session_probe_is_valid(result: dict[str, Any]) -> bool:
    return bool(
        result.get("status") == "ok"
        and not result.get("looks_like_login")
        and (result.get("looks_like_eams") or result.get("starts_json"))
    )


def write_json(path: Path, payload: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def validate_and_replace_storage_state(
    candidate_state: Path,
    final_state: Path,
    session_file: Path,
    session_config: dict[str, Any],
    classroom: str,
    date_text: str,
    timeout_seconds: int,
) -> None:
    temporary_session: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            suffix=".json",
            prefix="eams-session-candidate-",
            dir=session_file.parent,
            delete=False,
        ) as stream:
            temporary_session = Path(stream.name)
            candidate_config = dict(session_config)
            candidate_config["playwright_storage_state"] = str(candidate_state)
            json.dump(candidate_config, stream, ensure_ascii=False, indent=2)
            stream.write("\n")

        result = probe_session_config(temporary_session, classroom, date_text, timeout_seconds)
        if not session_probe_is_valid(result):
            raise SessionRefreshError(
                "candidate_rejected",
                "The updated browser session was not accepted by EAMS.",
            )

        final_state.parent.mkdir(parents=True, exist_ok=True)
        os.replace(candidate_state, final_state)
    finally:
        if temporary_session is not None:
            temporary_session.unlink(missing_ok=True)


def refresh_session(args: argparse.Namespace) -> dict[str, Any]:
    session_file = Path(args.session_file).expanduser().resolve()
    login_file = Path(args.login_file).expanduser().resolve()
    profile_dir = Path(args.profile_dir).expanduser().resolve()

    try:
        session_config = load_json_file(session_file)
        login_config = load_json_file(login_file)
    except ScheduleError as exc:
        raise SessionRefreshError("config_invalid", str(exc)) from exc

    username = str(login_config.get("username") or "")
    password = str(login_config.get("password") or "")
    login_url = str(login_config.get("login_url") or "")
    start_url = str(login_config.get("start_url") or "")
    classroom = str(login_config.get("classroom") or "A101")
    date_text = str(login_config.get("date") or "auto")
    if date_text == "auto":
        date_text = today_text()

    if not username or not password:
        raise SessionRefreshError("credentials_missing", "The local EAMS username or password is missing.")
    if not login_url or not start_url:
        raise SessionRefreshError("login_url_missing", "The local EAMS login_url or start_url is missing.")
    if args.timeout_seconds < 30 or args.timeout_seconds > 3600:
        raise SessionRefreshError("timeout_invalid", "The login timeout must be between 30 and 3600 seconds.")

    final_state = resolve_storage_state_path(session_file, session_config)
    edge_executable = find_edge_executable(args.edge_executable)
    profile_dir.mkdir(parents=True, exist_ok=True)
    final_state.parent.mkdir(parents=True, exist_ok=True)

    try:
        from playwright.sync_api import Error as PlaywrightError
        from playwright.sync_api import sync_playwright
    except ImportError as exc:
        raise SessionRefreshError(
            "playwright_missing",
            "Python Playwright is not installed. Install it before refreshing the EAMS session.",
        ) from exc

    candidate_state: Path | None = None
    context = None
    try:
        with sync_playwright() as playwright:
            context = playwright.chromium.launch_persistent_context(
                str(profile_dir),
                executable_path=str(edge_executable),
                headless=False,
                no_viewport=True,
                args=("--start-maximized",),
            )
            page = context.pages[0] if context.pages else context.new_page()
            try:
                page.goto(start_url, wait_until="domcontentloaded", timeout=30_000)
            except PlaywrightError:
                # A slow WebVPN redirect can outlive the navigation timeout; keep waiting visibly.
                pass

            deadline = time.monotonic() + args.timeout_seconds
            credentials_filled = False
            next_candidate_probe_at = 0.0
            while time.monotonic() < deadline:
                pages = context.pages
                if not pages:
                    raise SessionRefreshError("browser_closed", "The EAMS login browser was closed before login completed.")
                page = pages[-1]
                current_url = page.url
                now = time.monotonic()
                if page_looks_like_eams(page, start_url) and now >= next_candidate_probe_at:
                    time.sleep(1.0)
                    fd, candidate_name = tempfile.mkstemp(
                        prefix="eams-storage-candidate-",
                        suffix=".json",
                        dir=final_state.parent,
                    )
                    os.close(fd)
                    candidate_state = Path(candidate_name)
                    context.storage_state(path=str(candidate_state))
                    try:
                        validate_and_replace_storage_state(
                            candidate_state,
                            final_state,
                            session_file,
                            session_config,
                            classroom,
                            date_text,
                            args.upstream_timeout_seconds,
                        )
                    except SessionRefreshError as exc:
                        if exc.code != "candidate_rejected":
                            raise
                        candidate_state.unlink(missing_ok=True)
                        candidate_state = None
                        next_candidate_probe_at = time.monotonic() + 5.0
                        continue
                    candidate_state = None
                    return {"ok": True, "status": "updated"}

                if not credentials_filled and is_login_url(current_url, login_url):
                    credentials_filled = fill_login_credentials(page, username, password)
                time.sleep(0.5)

        raise SessionRefreshError("login_timeout", "Timed out waiting for the user to complete EAMS login.")
    except SessionRefreshError:
        raise
    except PlaywrightError as exc:
        raise SessionRefreshError("browser_failed", "The EAMS login browser could not complete the session update.") from exc
    finally:
        if candidate_state is not None:
            candidate_state.unlink(missing_ok=True)
        if context is not None:
            try:
                context.close()
            except Exception:
                pass


def run_self_test() -> None:
    assert is_login_url(
        "https://authserver.lntu.edu.cn/authserver/login?service=test",
        "https://authserver.lntu.edu.cn/authserver/login",
    )
    assert is_eams_url(
        "https://webvpn.lntu.edu.cn/proxy/eams/homeExt.action;jsessionid=redacted",
        "https://webvpn.lntu.edu.cn/proxy/eams/homeExt.action",
    )
    assert not is_eams_url(
        "https://authserver.lntu.edu.cn/authserver/login",
        "https://webvpn.lntu.edu.cn/proxy/eams/homeExt.action",
    )
    assert title_looks_like_eams("辽宁工程技术大学教学管理系统 - 个人")
    assert not title_looks_like_eams("统一身份认证平台")
    assert session_probe_is_valid({"status": "ok", "looks_like_login": False, "looks_like_eams": True})
    assert not session_probe_is_valid({"status": "ok", "looks_like_login": True, "looks_like_eams": True})

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        session_file = root / "session.json"
        state_file = root / "state.json"
        write_json(session_file, {"playwright_storage_state": "state.json"})
        write_json(state_file, {"cookies": []})
        assert resolve_storage_state_path(session_file, load_json_file(session_file)) == state_file.resolve()
        assert find_edge_executable(str(state_file)) == state_file.resolve()

        original_state = {"cookies": [{"name": "old", "value": "kept"}]}
        candidate_state = root / "candidate.json"
        write_json(state_file, original_state)
        write_json(
            candidate_state,
            {
                "cookies": [
                    {
                        "name": "JSESSIONID",
                        "value": "candidate",
                        "domain": "example.invalid",
                        "path": "/",
                        "expires": -1,
                    }
                ]
            },
        )
        rejected_config = {
            "upstream_url": "https://example.invalid/eams/home",
            "playwright_storage_state": state_file.name,
        }
        write_json(session_file, rejected_config)
        try:
            validate_and_replace_storage_state(
                candidate_state,
                state_file,
                session_file,
                rejected_config,
                "A101",
                "2026-07-17",
                1,
            )
        except SessionRefreshError as exc:
            assert exc.code == "candidate_rejected"
        else:
            raise AssertionError("invalid candidate session should be rejected")
        assert load_json_file(state_file) == original_state

    print("EAMS session refresh self-test passed.")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Refresh an EAMS session through visible Microsoft Edge login.")
    parser.add_argument("--session-file", default=os.getenv("SCHEDULE_EAMS_SESSION_FILE", ""))
    parser.add_argument("--login-file", default=os.getenv("SCHEDULE_EAMS_LOGIN_FILE", ""))
    parser.add_argument("--profile-dir", default=os.getenv("SCHEDULE_EAMS_BROWSER_PROFILE", ""))
    parser.add_argument("--edge-executable", default=os.getenv("SCHEDULE_EDGE_EXECUTABLE", ""))
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=int(os.getenv("SCHEDULE_EAMS_LOGIN_TIMEOUT_SECONDS", str(DEFAULT_LOGIN_TIMEOUT_SECONDS))),
    )
    parser.add_argument(
        "--upstream-timeout-seconds",
        type=int,
        default=int(os.getenv("SCHEDULE_UPSTREAM_TIMEOUT_SECONDS", str(DEFAULT_UPSTREAM_TIMEOUT_SECONDS))),
    )
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    if args.self_test:
        run_self_test()
        return 0
    if not args.session_file or not args.login_file or not args.profile_dir:
        print(json.dumps({"ok": False, "status": "config_missing", "message": "Session refresh paths are missing."}))
        return 2
    try:
        result = refresh_session(args)
    except SessionRefreshError as exc:
        print(json.dumps({"ok": False, "status": exc.code, "message": str(exc)}, ensure_ascii=False))
        return 2
    except Exception:
        print(
            json.dumps(
                {"ok": False, "status": "internal_error", "message": "The EAMS session refresh failed unexpectedly."},
                ensure_ascii=False,
            )
        )
        return 2
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
