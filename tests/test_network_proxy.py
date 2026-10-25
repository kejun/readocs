import asyncio
import gzip
import ipaddress
import socket
import ssl
from datetime import UTC, datetime, timedelta
from types import SimpleNamespace

import aiohttp
import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID
from readocs.errors import Problem
from readocs.network import LIMIT, Network, proxy_target
from yarl import URL


@pytest.fixture
async def trusted_proxy(tmp_path, monkeypatch):
    state = SimpleNamespace(
        requests=[],
        origins=[],
        sni=[],
        responses={},
        dns={
            "feed.example": ["93.184.216.34"],
            "other.example": ["1.1.1.1"],
            "proxy.example": ["127.0.0.1"],
            "private.example": ["127.0.0.1"],
            "mixed.example": ["93.184.216.34", "127.0.0.1"],
            "bad.example": ["93.184.216.34"],
        },
    )
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "feed.example")])
    now = datetime.now(UTC)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=1))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), True)
        .add_extension(
            x509.SubjectAlternativeName(
                [x509.DNSName("feed.example"), x509.DNSName("other.example")]
            ),
            False,
        )
        .sign(key, hashes.SHA256())
    )
    cert_path, key_path = tmp_path / "cert.pem", tmp_path / "key.pem"
    cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    server_ssl = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server_ssl.load_cert_chain(cert_path, key_path)
    server_ssl.set_servername_callback(lambda _sock, name, _ctx: state.sni.append(name))
    client_ssl = ssl.create_default_context(cafile=str(cert_path))
    connector = aiohttp.TCPConnector
    monkeypatch.setattr(
        "readocs.network.aiohttp.TCPConnector",
        lambda **kwargs: connector(ssl=client_ssl, **kwargs),
    )

    loop = asyncio.get_running_loop()
    getaddrinfo = loop.getaddrinfo

    async def resolve(host, port, *args, **kwargs):
        if host not in state.dns:
            return await getaddrinfo(host, port, *args, **kwargs)
        return [
            (
                socket.AF_INET6 if ":" in address else socket.AF_INET,
                socket.SOCK_STREAM,
                socket.IPPROTO_TCP,
                "",
                (address, port, 0, 0) if ":" in address else (address, port),
            )
            for address in state.dns[host]
        ]

    monkeypatch.setattr(loop, "getaddrinfo", resolve)

    async def origin(reader, writer):
        try:
            raw = await reader.readuntil(b"\r\n\r\n")
            lines = raw.decode("latin1").split("\r\n")
            method, path, _version = lines[0].split(" ")
            headers = {
                name.lower(): value.strip()
                for line in lines[1:]
                if ":" in line
                for name, value in [line.split(":", 1)]
            }
            state.origins.append({"method": method, "path": path, "headers": headers})
            status, extra, body = state.responses.get(path, (200, {}, b"public feed"))
            response = (
                f"HTTP/1.1 {status} Fixture\r\nConnection: close\r\n"
                f"Content-Length: {len(body)}\r\n"
                + "".join(f"{name}: {value}\r\n" for name, value in extra.items())
                + "\r\n"
            ).encode()
            writer.write(response + (b"" if method == "HEAD" else body))
            await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            writer.close()

    tls_origin = await asyncio.start_server(origin, "127.0.0.1", 0, ssl=server_ssl)
    plain_origin = await asyncio.start_server(origin, "127.0.0.1", 0)
    tls_port = tls_origin.sockets[0].getsockname()[1]
    plain_port = plain_origin.sockets[0].getsockname()[1]

    async def relay(reader, writer):
        try:
            while data := await reader.read(65536):
                writer.write(data)
                await writer.drain()
        except (ConnectionError, OSError):
            pass
        finally:
            writer.close()

    async def proxy(reader, writer):
        upstream = None
        try:
            raw = await reader.readuntil(b"\r\n\r\n")
            first, rest = raw.split(b"\r\n", 1)
            method, target, _version = first.decode().split(" ")
            state.requests.append({"method": method, "target": target, "headers": rest})
            remote, upstream = await asyncio.open_connection(
                "127.0.0.1", tls_port if method == "CONNECT" else plain_port
            )
            if method == "CONNECT":
                writer.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
                await writer.drain()
            else:
                assert URL(target).host is not None
                assert ipaddress.ip_address(URL(target).host).is_global
                # A real forward proxy removes its own authentication header.
                forwarded = b"\r\n".join(
                    line
                    for line in rest.split(b"\r\n")
                    if not line.lower().startswith(b"proxy-authorization:")
                )
                upstream.write(
                    f"{method} {URL(target).raw_path_qs} HTTP/1.1\r\n".encode()
                    + forwarded
                )
                await upstream.drain()
            await asyncio.gather(relay(reader, upstream), relay(remote, writer))
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            writer.close()
            if upstream:
                upstream.close()

    proxy_server = await asyncio.start_server(proxy, "127.0.0.1", 0)
    port = proxy_server.sockets[0].getsockname()[1]
    for scheme in ("http", "https"):
        value = f"http://test-user:test-password@proxy.example:{port}"
        monkeypatch.setenv(scheme + "_proxy", value)
        monkeypatch.setenv(scheme.upper() + "_PROXY", value)
    monkeypatch.setenv("no_proxy", "")
    monkeypatch.setenv("NO_PROXY", "")
    try:
        yield state
    finally:
        for server in (proxy_server, tls_origin, plain_origin):
            server.close()
            await server.wait_closed()


async def test_https_proxy_pins_connect_ip_and_preserves_verified_host(trusted_proxy):
    status, _headers, body = await Network().request("GET", "https://feed.example/feed")
    assert status == 200 and body == b"public feed"
    request = trusted_proxy.requests[0]
    assert request["method"] == "CONNECT"
    assert request["target"] == "93.184.216.34:443"
    assert b"Proxy-Authorization: Basic " in request["headers"]
    assert trusted_proxy.sni == ["feed.example"]
    assert trusted_proxy.origins[0]["headers"]["host"] == "feed.example"
    assert "proxy-authorization" not in trusted_proxy.origins[0]["headers"]


async def test_http_proxy_uses_public_absolute_target_and_original_host(trusted_proxy):
    status, _headers, body = await Network().request(
        "GET", "http://feed.example/feed?q=1"
    )
    assert status == 200 and body == b"public feed"
    request = trusted_proxy.requests[0]
    assert request["method"] == "GET"
    assert request["target"] == "http://93.184.216.34/feed?q=1"
    assert trusted_proxy.origins[0]["headers"]["host"] == "feed.example"


async def test_proxy_redirect_rechecks_dns_and_replaces_host_and_sni(trusted_proxy):
    trusted_proxy.responses["/start"] = (
        302,
        {"Location": "https://other.example/final"},
        b"",
    )
    status, _headers, body = await Network().request(
        "GET", "https://feed.example/start"
    )
    assert status == 200 and body == b"public feed"
    assert [request["target"] for request in trusted_proxy.requests] == [
        "93.184.216.34:443",
        "1.1.1.1:443",
    ]
    assert trusted_proxy.sni == ["feed.example", "other.example"]
    assert [request["headers"]["host"] for request in trusted_proxy.origins] == [
        "feed.example",
        "other.example",
    ]


@pytest.mark.parametrize("host", ["private.example", "mixed.example", "proxy.example"])
async def test_proxy_cannot_bypass_private_target_dns(trusted_proxy, host):
    with pytest.raises(Problem, match="URL_BLOCKED"):
        await Network().request("GET", f"https://{host}/feed")
    assert trusted_proxy.requests == []


async def test_redirect_to_private_dns_is_blocked_before_next_connect(trusted_proxy):
    trusted_proxy.responses["/start"] = (
        302,
        {"Location": "https://private.example/final"},
        b"",
    )
    with pytest.raises(Problem, match="URL_BLOCKED"):
        await Network().request("GET", "https://feed.example/start")
    assert len(trusted_proxy.requests) == 1


async def test_proxy_keeps_certificate_hostname_verification(trusted_proxy):
    with pytest.raises(Problem, match="UPSTREAM_TIMEOUT"):
        await Network().request("GET", "https://bad.example/feed")
    assert trusted_proxy.sni == ["bad.example"]
    assert trusted_proxy.origins == []


async def test_proxy_can_pin_public_ipv6_target(trusted_proxy):
    trusted_proxy.dns["feed.example"] = ["2606:4700:4700::1111"]
    status, _headers, _body = await Network().request(
        "GET", "https://feed.example/feed"
    )
    assert status == 200
    assert trusted_proxy.requests[0]["target"] == "[2606:4700:4700::1111]:443"
    assert trusted_proxy.sni == ["feed.example"]


async def test_no_proxy_uses_the_direct_transport(trusted_proxy, monkeypatch):
    monkeypatch.setenv("no_proxy", "feed.example")
    monkeypatch.setenv("NO_PROXY", "feed.example")
    target, headers, options = await proxy_target(
        "https://feed.example/feed", {"User-Agent": "test"}
    )
    assert target == URL("https://feed.example/feed")
    assert headers == {"User-Agent": "test"}
    assert options == {}
    assert trusted_proxy.requests == []


async def test_no_proxy_still_checks_dns_at_the_direct_connection(
    trusted_proxy, monkeypatch
):
    monkeypatch.setenv("no_proxy", "private.example")
    monkeypatch.setenv("NO_PROXY", "private.example")
    _target, _headers, options = await proxy_target("https://private.example/feed", {})
    assert options == {}
    with pytest.raises(Problem, match="URL_BLOCKED"):
        await Network().request("GET", "https://private.example/feed")
    assert trusted_proxy.requests == []


async def test_unsupported_proxy_scheme_has_an_explicit_error(
    trusted_proxy, monkeypatch
):
    monkeypatch.setenv("https_proxy", "socks5://127.0.0.1:1080")
    with pytest.raises(Problem, match="UPSTREAM_PROXY_UNSUPPORTED"):
        await Network().request("GET", "https://feed.example/feed")
    assert trusted_proxy.requests == []


async def test_response_limit_can_increase_without_changing_json_default(trusted_proxy):
    body = b"x" * (LIMIT + 1)
    trusted_proxy.responses["/large"] = (200, {}, body)
    with pytest.raises(Problem, match="CONTENT_TOO_LARGE") as error:
        await Network().request("GET", "https://feed.example/large")
    assert "2 MiB" in error.value.message
    status, _headers, received = await Network().request(
        "GET", "https://feed.example/large", max_bytes=16 * 1024 * 1024
    )
    assert status == 200 and received == body
    with pytest.raises(Problem, match="CONTENT_TOO_LARGE"):
        await Network().json("GET", "https://feed.example/large")


@pytest.mark.parametrize("body", [b"x" * 256, bytes(range(128))])
async def test_compressed_transfer_and_expansion_each_obey_limit(trusted_proxy, body):
    compressed = gzip.compress(body)
    assert len(compressed) > 128 or len(body) > 128
    trusted_proxy.responses["/compressed"] = (
        200,
        {"Content-Encoding": "gzip"},
        compressed,
    )
    with pytest.raises(Problem, match="CONTENT_TOO_LARGE") as error:
        await Network().request("GET", "https://feed.example/compressed", max_bytes=128)
    assert "128 字节" in error.value.message
