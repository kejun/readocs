"""Bounded public HTTP transport. DNS is checked at the connector, including redirects."""

import asyncio
import ipaddress
import json
import socket
import zlib
from urllib.parse import urljoin, urlsplit

import aiohttp

from .errors import Problem

LIMIT = 2 * 1024 * 1024


def validate_url(url):
    try:
        p = urlsplit(url)
        if (
            p.scheme not in ("http", "https")
            or not p.hostname
            or p.username
            or p.password
            or p.fragment
            or p.port not in (None, 80, 443)
            or len(url) > 2048
        ):
            raise ValueError()
        try:
            ip = ipaddress.ip_address(p.hostname)
        except ValueError:
            if (
                p.hostname.lower() in ("localhost", "localhost.localdomain")
                or ":" in p.hostname
            ):
                raise Problem("URL_BLOCKED", "请使用公开网络的 HTTP 或 HTTPS 地址")
        else:
            if not ip.is_global:
                raise Problem("URL_BLOCKED", "请使用公开网络的 HTTP 或 HTTPS 地址")
        return url
    except (ValueError, TypeError):
        raise Problem(
            "URL_BLOCKED", "请输入有效的公开网络 HTTP 或 HTTPS 地址"
        ) from None


class PublicResolver(aiohttp.abc.AbstractResolver):
    async def resolve(self, host, port=0, family=socket.AF_INET):
        records = await asyncio.get_running_loop().getaddrinfo(
            host, port, type=socket.SOCK_STREAM, family=family
        )
        if not records or any(
            not ipaddress.ip_address(r[4][0]).is_global for r in records
        ):
            raise Problem("URL_BLOCKED", "源地址解析到了不可访问的网络")
        return [
            {
                "hostname": host,
                "host": r[4][0],
                "port": port,
                "family": r[0],
                "proto": r[2],
                "flags": socket.AI_NUMERICHOST,
            }
            for r in records
        ]

    async def close(self):
        pass


class Network:
    async def request(self, method, url, *, headers=None, data=None):
        headers = {
            "User-Agent": "Readocs/0.1",
            "Accept-Encoding": "gzip, deflate",
            **(headers or {}),
        }
        try:
            async with asyncio.timeout(15):
                connector = aiohttp.TCPConnector(
                    resolver=PublicResolver(), use_dns_cache=False, force_close=True
                )
                async with aiohttp.ClientSession(
                    connector=connector,
                    auto_decompress=False,
                    trust_env=False,
                    max_field_size=32768,
                    timeout=aiohttp.ClientTimeout(total=15, connect=5),
                ) as session:
                    for _ in range(6):
                        validate_url(url)
                        async with session.request(
                            method,
                            url,
                            headers=headers,
                            data=data,
                            allow_redirects=False,
                        ) as r:
                            if r.status in (301, 302, 303, 307, 308):
                                target = urljoin(url, r.headers.get("Location", ""))
                                # Auth endpoints must never forward application secrets on a redirect.
                                if (
                                    method != "GET"
                                    or urlsplit(target).netloc != urlsplit(url).netloc
                                ) and ("Authorization" in headers or method != "GET"):
                                    raise Problem(
                                        "UPSTREAM_REDIRECT",
                                        "授权接口发生了意外跳转",
                                        502,
                                    )
                                url = target
                                continue
                            encoding = r.headers.get(
                                "Content-Encoding", "identity"
                            ).lower()
                            if encoding not in ("identity", "gzip", "deflate"):
                                raise Problem(
                                    "PARSE_ERROR", "源使用了不支持的压缩格式", 502
                                )
                            inflater = (
                                zlib.decompressobj(31 if encoding == "gzip" else 15)
                                if encoding != "identity"
                                else None
                            )
                            chunks, total, expanded = [], 0, 0
                            async for chunk in r.content.iter_chunked(16384):
                                total += len(chunk)
                                chunk = (
                                    inflater.decompress(chunk, LIMIT - expanded + 1)
                                    if inflater
                                    else chunk
                                )
                                expanded += len(chunk)
                                if (
                                    total > LIMIT
                                    or expanded > LIMIT
                                    or (inflater and inflater.unconsumed_tail)
                                ):
                                    raise Problem(
                                        "CONTENT_TOO_LARGE",
                                        "源内容超过 2 MiB 限制",
                                        422,
                                    )
                                chunks.append(chunk)
                            if inflater and not inflater.eof:
                                raise Problem("PARSE_ERROR", "源压缩数据不完整", 502)
                            return r.status, dict(r.headers), b"".join(chunks)
            raise Problem("UPSTREAM_REDIRECT", "源跳转次数过多", 502)
        except (TimeoutError, aiohttp.ClientConnectionError, socket.gaierror):
            raise Problem("UPSTREAM_TIMEOUT", "连接失败，请稍后重试", 502) from None
        except (aiohttp.ClientError, zlib.error, ValueError):
            raise Problem("PARSE_ERROR", "源返回的数据无效", 502) from None

    async def json(self, method, url, **kwargs):
        status, headers, body = await self.request(method, url, **kwargs)
        if status == 429:
            try:
                delay = min(86400, max(30, int(headers.get("Retry-After", 60))))
            except ValueError:
                delay = 60
            raise Problem("RATE_LIMITED", "上游请求过于频繁，请稍后重试", 429, delay)
        if status >= 500:
            raise Problem("UPSTREAM_TIMEOUT", "上游服务暂时不可用", 502)
        try:
            result = json.loads(body)
        except (ValueError, UnicodeError):
            raise Problem("PARSE_ERROR", "上游没有返回有效数据", 502) from None
        if not isinstance(result, dict):
            raise Problem("PARSE_ERROR", "上游返回的数据结构无效", 502)
        return status, result
