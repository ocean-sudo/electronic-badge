#!/usr/bin/env python3
"""USB 图片徽章控制台：Waveshare ESP32-S3-Touch-AMOLED-1.75C。

安装 requirements.txt 后，复制 .env.example 为 .env 并运行 python tools/serve.py。
默认仅监听 http://127.0.0.1:8765/；使用 --help 查看命令行覆盖选项。
配置优先级为命令行 > 进程环境 > 项目根目录 .env > 默认值。
完整安装、固定厂商依赖、固件构建、网络安全与已有验证边界见 README.md。
"""

import argparse
import ipaddress
import json
import os
import re
import socket
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import serial
from dotenv import load_dotenv
from serial.tools import list_ports

WIDTH = HEIGHT = 466
FRAME_BYTES = WIDTH * HEIGHT * 2
CHUNK_BYTES = 4096
MIN_SLOT = 0
MAX_SLOT = 2147483646
MAX_BLOB_BYTES = 64 * 1024 * 1024
JPEG_TAG = b"\xff\xfe\x00\x0fBDGJ1"
MENU_ACTIONS = frozenset({"playback", "display", "timeout", "animation", "main",
                          "slideshow", "shuffle", "interval_down", "interval_up",
                          "brightness_down", "brightness_up", "previous", "next", "sleep", "back",
                          "usb_sleep_down", "usb_sleep_up", "battery_sleep_down", "battery_sleep_up",
                          "transition_next", "motion_next", "rotation_faster", "rotation_slower"})
# Espressif ESP32-S3 native USB Serial/JTAG and USB CDC identifiers.
USB_IDS = {(0x303A, 0x1001), (0x303A, 0x0002)}
ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "web" / "index.html"


class BridgeError(Exception):
    def __init__(self, code, message, status=503):
        super().__init__(message)
        self.code = code
        self.status = status


class Badge:
    """One persistent exclusive serial handle; whole transactions hold the lock."""

    def __init__(self, device=None):
        self.device = device
        self.port = None
        self.lock = threading.Lock()

    def _close(self):
        if self.port is not None:
            try:
                self.port.close()
            except (OSError, serial.SerialException):
                pass
            self.port = None

    def close(self):
        with self.lock:
            self._close()

    def _connect(self):
        if self.port is not None and self.port.is_open:
            return
        device = self.device
        if device is None:
            candidates = [p.device for p in list_ports.comports()
                          if (p.vid, p.pid) in USB_IDS]
            if not candidates:
                raise BridgeError("not_found", "未找到徽章。请连接支持数据传输的 USB 线，或用 --serial 指定串口。")
            if len(candidates) != 1:
                raise BridgeError("ambiguous", "发现多个 Espressif 串口，请用 --serial 明确指定：" + "、".join(candidates))
            device = candidates[0]
        port = serial.Serial(port=None, baudrate=115200, timeout=0.25,
                             write_timeout=10, exclusive=True)
        # Native ESP32-S3 CDC requires DTR to indicate an attached host.
        # Keep RTS deasserted: opening the console must not reset the device.
        port.dtr = True
        port.rts = False
        port.port = device
        try:
            port.open()
            port.reset_input_buffer()
            self.port = port
            # Synchronize only on open, allowing finite startup output before JSON.
            self._status(startup=True)
        except Exception:
            port.close()
            self.port = None
            raise

    def _line(self, deadline):
        line = bytearray()
        while time.monotonic() < deadline:
            byte = self.port.read(1)
            if not byte:
                continue
            if byte == b"\n":
                try:
                    return line.rstrip(b"\r").decode("ascii")
                except UnicodeDecodeError as exc:
                    raise BridgeError("protocol", "设备响应不是有效的 ASCII 协议。请确认已刷入图片徽章固件。", 502) from exc
            line.extend(byte)
            if len(line) > 4096:
                raise BridgeError("protocol", "设备响应过长，串口协议不同步。", 502)
        raise BridgeError("timeout", "设备响应超时。请检查 USB 连接后重试。", 504)

    def _write(self, payload):
        if self.port.write(payload) != len(payload):
            raise BridgeError("short_write", "USB 写入未完成，请重新连接后重试。", 502)

    def _command(self, command):
        self._write((command + "\n").encode("ascii"))

    def _expect(self, expected, deadline):
        response = self._line(deadline)
        if response == expected:
            return
        self._response_error(response)

    @staticmethod
    def _response_error(response):
        if response.startswith("ERR "):
            raise BridgeError("device", "设备拒绝操作：" + response[4:], 409)
        raise BridgeError("protocol", "设备响应不符合徽章协议：" + response[:160], 502)

    def _status(self, startup=False):
        self._command("STATUS")
        deadline = time.monotonic() + (8 if startup else 5)
        while True:
            response = self._line(deadline)
            try:
                status = json.loads(response)
            except (ValueError, TypeError):
                if startup and not response.startswith("ERR "):
                    continue
                self._response_error(response)
            if not isinstance(status, dict) or status.get("firmware") != "picture-badge-2":
                if startup:
                    continue
                raise BridgeError("firmware", "设备未运行兼容的 picture-badge-2 固件。", 502)
            if status.get("width") != WIDTH or status.get("height") != HEIGHT:
                raise BridgeError("firmware", "设备图片尺寸与 466 × 466 协议不一致。", 502)
            revision = status.get("catalog_revision")
            if type(revision) is not int or not 0 <= revision <= 0xFFFFFFFF:
                raise BridgeError("protocol", "设备返回了无效的图片目录版本。", 502)
            for name in ("image_count", "storage_total_bytes", "storage_used_bytes", "storage_free_bytes", "storage_reserve_bytes"):
                if type(status.get(name)) is not int or status[name] < 0:
                    raise BridgeError("protocol", "设备返回了无效的存储汇总。", 502)
            return status

    def _read_data(self, command, maximum, timeout=90):
        deadline = time.monotonic() + timeout
        self._command(command)
        response = self._line(min(deadline, time.monotonic() + 10))
        fields = response.split()
        if len(fields) != 3 or fields[0] != "DATA":
            self._response_error(response)
        try:
            size, expected_crc = int(fields[1]), int(fields[2])
        except ValueError as exc:
            raise BridgeError("protocol", "设备返回了无效的数据长度或校验码。", 502) from exc
        if not 0 <= size <= maximum or not 0 <= expected_crc <= 0xFFFFFFFF:
            raise BridgeError("protocol", "设备返回了超出范围的数据长度或校验码。", 502)
        data = bytearray()
        idle_deadline = time.monotonic() + 10
        while len(data) < size:
            if time.monotonic() >= min(deadline, idle_deadline):
                raise BridgeError("timeout", "设备数据读取超时。请检查 USB 连接后重试。", 504)
            chunk = self.port.read(min(CHUNK_BYTES, size - len(data)))
            if chunk:
                data.extend(chunk)
                idle_deadline = time.monotonic() + 10
        self._expect("", min(deadline, time.monotonic() + 10))
        self._expect("OK", min(deadline, time.monotonic() + 10))
        if zlib.crc32(data) != expected_crc:
            raise BridgeError("checksum", "设备数据传输校验失败，请重试。", 502)
        return bytes(data), expected_crc

    def _jpeg_limit(self):
        status = self._status()
        return max(0, status["storage_total_bytes"] - status["storage_reserve_bytes"])

    @staticmethod
    def _validate_jpeg(image, transfer_crc=None, maximum=0xFFFFFFFF):
        if len(image) < 21 or len(image) > maximum or image[:2] != b"\xff\xd8" or image[2:11] != JPEG_TAG or image[-2:] != b"\xff\xd9":
            raise BridgeError("jpeg", "图片不是徽章支持的带校验 JPEG 文件。", 400)
        if int.from_bytes(image[11:15], "little") != len(image):
            raise BridgeError("jpeg", "JPEG 内记录的文件长度不正确。", 400)
        expected = int.from_bytes(image[15:19], "little")
        normalized_crc = zlib.crc32(image[:15])
        normalized_crc = zlib.crc32(b"\0\0\0\0", normalized_crc)
        normalized_crc = zlib.crc32(memoryview(image)[19:], normalized_crc) & 0xFFFFFFFF
        if normalized_crc != expected:
            raise BridgeError("checksum", "JPEG 文件完整性校验失败。", 400)
        if transfer_crc is not None and zlib.crc32(image) != transfer_crc:
            raise BridgeError("checksum", "JPEG 传输校验失败。", 400)
        return expected

    def _read_jpeg(self, slot):
        maximum = self._jpeg_limit()
        data, transfer_crc = self._read_data(f"GET {slot}", maximum)
        self._validate_jpeg(data, transfer_crc, maximum)
        return data, transfer_crc

    def _read_frame(self, command):
        data, expected_crc = self._read_data(command, FRAME_BYTES)
        if len(data) != FRAME_BYTES:
            raise BridgeError("protocol", "设备返回了无效的 RGB565 画面长度。", 502)
        return data, expected_crc

    def _read_catalog(self):
        data, _ = self._read_data("LIST", MAX_BLOB_BYTES)
        try:
            catalog = json.loads(data)
        except (ValueError, UnicodeDecodeError) as exc:
            raise BridgeError("protocol", "设备图片目录不是有效 JSON。", 502) from exc
        revision = catalog.get("revision") if isinstance(catalog, dict) else None
        if type(revision) is not int or not 0 <= revision <= 0xFFFFFFFF or not isinstance(catalog.get("images"), list):
            raise BridgeError("protocol", "设备返回了无效的图片目录。", 502)
        images, ids = [], set()
        for image in catalog["images"]:
            if not isinstance(image, dict):
                raise BridgeError("protocol", "设备图片目录包含无效条目。", 502)
            slot, size, crc = image.get("slot"), image.get("bytes"), image.get("crc32")
            if type(slot) is not int or not MIN_SLOT <= slot <= MAX_SLOT or slot in ids or type(size) is not int or size < 21 or size > 0xFFFFFFFF or type(crc) is not int or not 0 <= crc <= 0xFFFFFFFF:
                raise BridgeError("protocol", "设备图片目录条目超出有效范围。", 502)
            ids.add(slot)
            images.append({"slot": slot, "bytes": size, "crc32": crc})
        return {"revision": catalog["revision"], "images": sorted(images, key=lambda image: image["slot"])}

    def _put_image(self, slot, image):
        self._validate_jpeg(image, maximum=self._jpeg_limit())
        transfer_crc = zlib.crc32(image)
        target = "AUTO" if slot is None else str(slot)
        deadline = time.monotonic() + 90
        self._command(f"PUT {target} {len(image)} {transfer_crc}")
        self._expect("READY", min(deadline, time.monotonic() + 10))
        for offset in range(0, len(image), CHUNK_BYTES):
            if time.monotonic() >= deadline:
                raise BridgeError("timeout", "图片上传超时；未完成的上传不会替换原图片。", 504)
            self._write(image[offset:offset + CHUNK_BYTES])
            self._expect("ACK", min(deadline, time.monotonic() + 10))
        response = self._line(deadline)
        fields = response.split()
        if len(fields) != 2 or fields[0] != "OK":
            self._response_error(response)
        try:
            committed_slot = int(fields[1])
        except ValueError as exc:
            raise BridgeError("protocol", "设备确认了无效的图片槽位。", 502) from exc
        if not MIN_SLOT <= committed_slot <= MAX_SLOT:
            raise BridgeError("protocol", "设备确认的图片槽位超出范围。", 502)
        if slot is not None and committed_slot != slot:
            raise BridgeError("protocol", "设备确认的图片槽位与请求不一致。", 502)
        return {"ok": True, "slot": committed_slot, "crc32": transfer_crc}

    def transact(self, operation, *args):
        with self.lock:
            try:
                self._connect()
                if operation == "status":
                    return self._status()
                if operation == "catalog":
                    return self._read_catalog()
                if operation == "jpeg_limit":
                    return self._jpeg_limit()
                if operation == "get":
                    return self._read_jpeg(args[0])
                if operation == "menu_frame":
                    return self._read_frame("MENUFRAME")
                if operation == "display_frame":
                    return self._read_frame("DISPLAYFRAME")
                if operation == "put":
                    return self._put_image(*args)
                self._command(operation)
                self._expect("OK", time.monotonic() + 15)
                if operation == "REBOOT":
                    self._close()
                return {"ok": True}
            except BridgeError:
                self._close()
                raise
            except (OSError, serial.SerialException) as exc:
                self._close()
                raise BridgeError("serial", "无法访问 USB 串口（可能已拔出、被占用或没有权限）：" + str(exc)) from exc


class Server(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, address, badge, allowed_hosts):
        super().__init__(address, Handler)
        self.badge = badge
        self.allowed_hosts = frozenset(allowed_hosts)


class Handler(BaseHTTPRequestHandler):
    server_version = "PictureBadge/2"

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def _reply(self, status, body, content_type="application/json; charset=utf-8", extra=None):
        if isinstance(body, dict):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'")
        self.send_header("Connection", "close")
        if extra:
            for key, value in extra.items():
                self.send_header(key, str(value))
        self.end_headers()
        self.close_connection = True
        self.wfile.write(body)

    def _security(self, mutation):
        hosts = self.headers.get_all("Host", [])
        if len(hosts) != 1:
            raise BridgeError("host", "请求必须包含唯一的 Host。", 403)
        try:
            host = urlsplit("http://" + hosts[0])
            valid = (host.hostname in self.server.allowed_hosts
                     and (host.port or 80) == self.server.server_port
                     and not host.username and not host.password
                     and not host.path and not host.query and not host.fragment)
        except ValueError:
            valid = False
        if not valid:
            raise BridgeError("host", "请求地址不在服务的 Host 白名单中。", 403)
        if mutation:
            origins = self.headers.get_all("Origin", [])
            if len(origins) > 1 or (origins and origins[0] != "http://" + hosts[0]):
                raise BridgeError("origin", "拒绝跨站操作，请直接打开徽章控制台地址。", 403)
            if self.headers.get("Sec-Fetch-Site") == "cross-site":
                raise BridgeError("origin", "拒绝跨站操作，请直接打开徽章控制台地址。", 403)

    def _body(self, maximum):
        lengths = self.headers.get_all("Content-Length", [])
        if self.headers.get("Transfer-Encoding") is not None or len(lengths) != 1:
            raise BridgeError("length", "请求必须包含唯一的 Content-Length，且不支持分块编码。", 411)
        try:
            length = int(lengths[0])
        except ValueError as exc:
            raise BridgeError("length", "无效的请求长度。", 400) from exc
        if length < 0 or length > maximum:
            raise BridgeError("length", "请求内容过大或长度无效。", 413)
        body = self.rfile.read(length)
        if len(body) != length:
            raise BridgeError("body", "请求内容不完整。", 400)
        return body

    @staticmethod
    def _integer(value, lower, upper, label):
        if type(value) is not int or not lower <= value <= upper:
            raise BridgeError("value", f"{label}必须是 {lower}～{upper} 的整数。", 400)
        return value

    def _slot(self, query, allow_auto=False):
        params = parse_qs(query, keep_blank_values=True)
        if set(params) != {"slot"} or len(params["slot"]) != 1:
            raise BridgeError("slot", "请指定唯一的图片槽位 slot。", 400)
        value = params["slot"][0]
        if allow_auto and value == "auto":
            return None
        if not value or not value.isascii() or not value.isdecimal() or len(value) > 10:
            raise BridgeError("slot", "图片槽位必须是 0～2147483646 的整数。", 400)
        return self._integer(int(value), MIN_SLOT, MAX_SLOT, "图片槽位")

    def _dispatch(self, mutation):
        self._security(mutation)
        url = urlsplit(self.path)
        if url.scheme or url.netloc or url.fragment:
            raise BridgeError("path", "无效的请求地址。", 400)
        path = url.path
        if not mutation:
            if path == "/" and not url.query:
                self._reply(200, PAGE.read_bytes(), "text/html; charset=utf-8")
            elif path == "/api/status" and not url.query:
                self._reply(200, self.server.badge.transact("status"))
            elif path == "/api/catalog" and not url.query:
                catalog = self.server.badge.transact("catalog")
                self._reply(200, {"catalog_revision": catalog["revision"], "images": catalog["images"]})
            elif path == "/api/image":
                image, crc = self.server.badge.transact("get", self._slot(url.query))
                self._reply(200, image, "image/jpeg", {"X-CRC32": crc})
            elif path == "/api/menu-image" and not url.query:
                image, crc = self.server.badge.transact("menu_frame")
                self._reply(200, image, "application/octet-stream", {"X-CRC32": crc})
            elif path == "/api/display-image" and not url.query:
                image, crc = self.server.badge.transact("display_frame")
                self._reply(200, image, "application/octet-stream", {"X-CRC32": crc})
            else:
                raise BridgeError("not_found", "没有这个页面或接口。", 404)
            return
        if path == "/api/image":
            slot = self._slot(url.query, allow_auto=True)
            if self.headers.get_content_type() != "image/jpeg":
                raise BridgeError("type", "图片必须使用 image/jpeg 二进制上传。", 415)
            image = self._body(self.server.badge.transact("jpeg_limit"))
            result = self.server.badge.transact("put", slot, image)
        else:
            if url.query or path not in {"/api/show", "/api/delete", "/api/next", "/api/previous", "/api/shuffle", "/api/brightness", "/api/sleep", "/api/reboot", "/api/slideshow", "/api/autosleep", "/api/animation", "/api/menu", "/api/menu-action"}:
                raise BridgeError("not_found", "没有这个操作接口。", 404)
            if self.headers.get_content_type() != "application/json":
                raise BridgeError("type", "操作参数必须为 JSON。", 415)
            try:
                data = json.loads(self._body(1024))
            except (ValueError, UnicodeDecodeError) as exc:
                raise BridgeError("json", "操作参数不是有效的 JSON。", 400) from exc
            if not isinstance(data, dict):
                raise BridgeError("json", "操作参数必须是 JSON 对象。", 400)
            if path in {"/api/show", "/api/delete"}:
                slot = self._integer(data.get("slot"), MIN_SLOT, MAX_SLOT, "图片槽位")
                command = ("SHOW" if path == "/api/show" else "DELETE") + f" {slot}"
            elif path == "/api/brightness":
                value = self._integer(data.get("value"), 1, 180, "亮度")
                command = f"BRIGHT {value}"
            elif path == "/api/sleep":
                if type(data.get("sleeping")) is not bool:
                    raise BridgeError("value", "sleeping 必须为 true 或 false。", 400)
                command = f"SLEEP {int(data['sleeping'])}"
            elif path == "/api/slideshow":
                if type(data.get("enabled")) is not bool:
                    raise BridgeError("value", "enabled 必须为 true 或 false。", 400)
                interval = self._integer(data.get("interval"), 2, 3600, "自动切图间隔（秒）")
                command = f"SLIDESHOW {int(data['enabled'])} {interval}"
            elif path == "/api/shuffle":
                if set(data) != {"enabled"} or type(data.get("enabled")) is not bool:
                    raise BridgeError("value", "随机设置必须且只能包含 enabled（true 或 false）。", 400)
                command = f"SHUFFLE {int(data['enabled'])}"
            elif path == "/api/previous":
                if data:
                    raise BridgeError("value", "上一张操作参数必须为空 JSON 对象。", 400)
                command = "PREVIOUS"
            elif path == "/api/autosleep":
                usb_seconds = self._integer(data.get("usb_seconds"), 0, 86400, "插电自动熄屏时间（秒）")
                battery_seconds = self._integer(data.get("battery_seconds"), 0, 86400, "电池自动熄屏时间（秒）")
                if (0 < usb_seconds < 5) or (0 < battery_seconds < 5):
                    raise BridgeError("value", "自动熄屏时间必须是 0（关闭）或 5～86400 的整数秒。", 400)
                command = f"AUTOSLEEP {usb_seconds} {battery_seconds}"
            elif path == "/api/animation":
                if set(data) != {"transition", "motion", "rotation_seconds"}:
                    raise BridgeError("value", "动画参数必须且只能包含 transition、motion、rotation_seconds。", 400)
                transition, motion = data["transition"], data["motion"]
                transitions = {"direct": 0, "fade": 1, "slide": 2}
                motions = {"off": 0, "shift": 1, "rotate": 2, "gravity": 3}
                if type(transition) is not str or transition not in transitions:
                    raise BridgeError("value", "transition 必须是 direct、fade 或 slide。", 400)
                if type(motion) is not str or motion not in motions:
                    raise BridgeError("value", "motion 必须是 off、shift、rotate 或 gravity。", 400)
                rotation_seconds = self._integer(data["rotation_seconds"], 8, 120, "完整旋转周期（秒）")
                command = f"ANIMATION {transitions[transition]} {motions[motion]} {rotation_seconds}"
            elif path == "/api/menu":
                if type(data.get("open")) is not bool:
                    raise BridgeError("value", "open 必须为 true 或 false。", 400)
                command = f"MENU {int(data['open'])}"
            elif path == "/api/menu-action":
                action = data.get("action")
                if type(action) is not str or action not in MENU_ACTIONS:
                    raise BridgeError("value", "无效的设备菜单操作。", 400)
                command = f"MENUACT {action}"
            else:
                command = "NEXT" if path == "/api/next" else "REBOOT"
            result = self.server.badge.transact(command)
        self._reply(200, result)

    def _handle(self, mutation):
        try:
            self._dispatch(mutation)
        except BridgeError as exc:
            self._reply(exc.status, {"ok": False, "error": exc.code, "message": str(exc)})
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        except (TimeoutError, socket.timeout):
            self.close_connection = True
        except Exception as exc:
            print(f"Request failed: {exc}", file=sys.stderr)
            self._reply(500, {"ok": False, "error": "internal", "message": "本地服务发生错误，请查看终端日志。"})

    def do_GET(self):
        self._handle(False)

    def do_POST(self):
        self._handle(True)


def main():
    # CLI replaces each environment option; process environment wins over .env.
    load_dotenv(ROOT / ".env", override=False)
    parser = argparse.ArgumentParser(description="USB 图片徽章控制台（默认仅本机，可指定多个局域网或 Tailscale 监听地址）")
    parser.add_argument("--port", type=int, default=os.environ.get("BADGE_PORT", "8765"),
                        help="HTTP 端口；默认读取 BADGE_PORT，否则 8765")
    parser.add_argument("--host", action="append", metavar="IP",
                        help="监听的 IPv4 地址，可重复；覆盖 BADGE_HOSTS，未配置时仅 127.0.0.1")
    parser.add_argument("--allow-host", action="append", metavar="NAME",
                        help="额外允许的主机名，不含协议或端口；可重复，覆盖 BADGE_ALLOW_HOSTS")
    parser.add_argument("--serial", default=os.environ.get("BADGE_SERIAL") or None, metavar="PATH",
                        help="明确指定 USB 串口；覆盖 BADGE_SERIAL，未配置时自动检测")
    args = parser.parse_args()
    if args.host is None:
        args.host = [host.strip() for host in os.environ.get("BADGE_HOSTS", "").split(",") if host.strip()]
    if args.allow_host is None:
        args.allow_host = [name.strip() for name in os.environ.get("BADGE_ALLOW_HOSTS", "").split(",") if name.strip()]
    if not 1 <= args.port <= 65535:
        parser.error("--port 必须为 1～65535")
    try:
        listen_ips = list(dict.fromkeys(ipaddress.IPv4Address(host) for host in (args.host or ["127.0.0.1"])))
    except ipaddress.AddressValueError:
        parser.error("--host 必须是 IPv4 地址")
    allowed_hosts = {"127.0.0.1", "localhost", *(str(ip) for ip in listen_ips)}
    for name in args.allow_host:
        name = name.lower()
        if len(name) > 253 or not all(re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label) for label in name.split(".")):
            parser.error("--allow-host 必须是不含协议或端口的有效主机名")
        allowed_hosts.add(name)
    if not PAGE.is_file():
        parser.error(f"缺少网页文件：{PAGE}")
    badge = Badge(args.serial)
    servers = []
    workers = []
    try:
        # Bind every explicit interface before serving; all listeners share the
        # same serial transaction lock instead of opening the badge twice.
        for listen_ip in listen_ips:
            servers.append(Server((str(listen_ip), args.port), badge, allowed_hosts))
        for listen_ip in listen_ips:
            print(f"图片徽章控制台：http://{listen_ip}:{args.port}/", flush=True)
        print(f"允许的 Host（端口 {args.port}）：" + "、".join(sorted(allowed_hosts)), flush=True)
        if any(not ip.is_loopback for ip in listen_ips):
            print("注意：没有登录认证。只用于可信局域网或受控 Tailscale 网络，不要映射此端口到公网。", flush=True)
        print("按 Ctrl+C 关闭。串口保持独占；拔插后页面会自动重新连接。", flush=True)
        for server in servers[1:]:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            workers.append((server, worker))
        servers[0].serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        for server, worker in workers:
            server.shutdown()
            worker.join()
        for server in servers:
            server.server_close()
        badge.close()


if __name__ == "__main__":
    main()
