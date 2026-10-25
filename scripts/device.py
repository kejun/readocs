#!/usr/bin/env python3
"""Configure a Readocs device over USB without placing Wi-Fi passwords in arguments."""

import argparse
import getpass
import json
import sys
import time
from pathlib import Path

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command", choices=["status", "setup", "wifi", "bind", "check", "forget"]
    )
    parser.add_argument("--port", default="/dev/cu.usbmodem101")
    parser.add_argument("--ssid")
    parser.add_argument(
        "--replace",
        help="Explicit saved SSID to replace when all five slots are occupied",
    )
    parser.add_argument(
        "--server", help="HTTPS proxy URL, reachable on the device network"
    )
    parser.add_argument(
        "--ca", type=Path, help="Local proxy public TLS certificate (.readocs/tls.crt)"
    )
    args = parser.parse_args()
    payload = {"cmd": args.command}
    if args.command in ("wifi", "forget"):
        payload["ssid"] = args.ssid or input("Wi-Fi 名称：")
        if args.command == "wifi":
            payload["password"] = getpass.getpass("Wi-Fi 密码（开放网络直接回车）：")
        if args.replace:
            payload["replace"] = args.replace
    if args.command == "bind":
        if not args.server or not args.server.startswith("https://"):
            parser.error("绑定需要 --server https://代理地址:端口")
        payload.update(server=args.server, code=input("管理页上的配对码：").strip())
        if args.ca:
            payload["ca"] = args.ca.read_text()
    try:
        # Exclusive access prevents accidental sharing with a logger or monitor.
        with serial.Serial(args.port, 115200, timeout=0.5, exclusive=True) as port:
            port.reset_input_buffer()
            port.write((json.dumps(payload, ensure_ascii=False) + "\n").encode())
            port.flush()
            payload.clear()
            # A command can queue behind a 60-second article download, then
            # use its own 20-second network budget. Allow USB scheduling too.
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                line = port.readline().decode("utf-8", errors="replace").strip()
                if line.startswith("ERROR "):
                    print(line)
                    sys.exit(1)
                kind, _, body = line.partition(" ")
                if kind not in ("STATUS", "RESULT"):
                    continue
                try:
                    result = json.loads(body)
                except ValueError:
                    continue
                if not isinstance(result, dict):
                    continue
                # Automatic sync also emits RESULT. Only the response to our
                # command may complete this operation.
                if kind == "STATUS":
                    if args.command != "status":
                        continue
                elif (
                    args.command == "status"
                    or result.get("command") != args.command
                    or not isinstance(result.get("ok"), bool)
                ):
                    continue
                print(line)
                if kind == "RESULT" and not result["ok"]:
                    sys.exit(1)
                return
            sys.exit("设备未在 90 秒内返回结果，请查看屏幕或检查串口占用。")
    except serial.SerialException:
        sys.exit("USB 连接失败或串口被占用，请关闭其他串口监视器后重试。")


if __name__ == "__main__":
    main()
