#!/usr/bin/env python3
"""Generate a OneNET device token without storing the device key."""

from __future__ import annotations

import argparse
import base64
import getpass
import hashlib
import hmac
import os
import time
from pathlib import Path
from urllib.parse import quote


def make_token(product_id: str, device_name: str, key: str, expires_at: int, method: str) -> str:
    version = "2018-10-31"
    res = f"products/{product_id}/devices/{device_name}"
    string_for_signature = f"{expires_at}\n{method}\n{res}\n{version}"

    digestmod = {
        "hmacmd5": hashlib.md5,
        "hmacsha1": hashlib.sha1,
        "hmacsha256": hashlib.sha256,
    }[method]

    key_bytes = base64.b64decode(key)
    sign = base64.b64encode(
        hmac.new(key_bytes, string_for_signature.encode("utf-8"), digestmod).digest()
    ).decode("utf-8")

    return (
        f"version={quote(version, safe='')}"
        f"&res={quote(res, safe='')}"
        f"&et={expires_at}"
        f"&method={quote(method, safe='')}"
        f"&sign={quote(sign, safe='')}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a OneNET device auth token for MQTT password / HTTP Authorization."
    )
    parser.add_argument("--product-id", required=True, help="OneNET product ID")
    parser.add_argument("--device-name", required=True, help="OneNET device name")
    parser.add_argument(
        "--ttl-days",
        type=int,
        default=30,
        help="Token lifetime in days from now. Default: 30",
    )
    parser.add_argument(
        "--method",
        choices=("hmacmd5", "hmacsha1", "hmacsha256"),
        default="hmacsha256",
        help="Signing method. Default: hmacsha256",
    )
    parser.add_argument(
        "--key-env",
        default="",
        help="Read the device key from this environment variable instead of prompting.",
    )
    parser.add_argument(
        "--output-env",
        default="",
        help="Write OneNET settings to this local env file instead of printing the token.",
    )
    parser.add_argument(
        "--mqtt-uri",
        default="",
        help="Optional MQTT URI to include when writing --output-env.",
    )
    parser.add_argument(
        "--api-host",
        default="studio-file.heclouds.com",
        help="Optional OneNET file API host to include when writing --output-env.",
    )
    args = parser.parse_args()

    key = os.environ.get(args.key_env, "").strip() if args.key_env else ""
    if not key:
        key = getpass.getpass("OneNET device key (input hidden): ").strip()
    if not key:
        raise SystemExit("Device key is required.")

    expires_at = int(time.time()) + args.ttl_days * 24 * 60 * 60
    token = make_token(args.product_id, args.device_name, key, expires_at, args.method)
    if args.output_env:
        output_path = Path(args.output_env)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(
            "\n".join(
                [
                    f"ONENET_PRODUCT_ID={args.product_id}",
                    f"ONENET_DEVICE_NAME={args.device_name}",
                    f"ONENET_AUTH_TOKEN={token}",
                    f"ONENET_MQTT_URI={args.mqtt_uri}",
                    f"ONENET_API_HOST={args.api_host}",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        print(f"OneNET settings saved to {output_path}")
        return 0

    print(token)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
