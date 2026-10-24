import argparse
import asyncio
import datetime
import ipaddress
import os
import socket
from pathlib import Path

import uvicorn
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

from .app import create_app
from .store import private_file


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def certificate(directory, hosts):
    key_path, cert_path = directory / "tls.key", directory / "tls.crt"
    # The development CA remains stable for device trust; explicit deletion rotates it.
    if key_path.exists() and cert_path.exists():
        return key_path, cert_path
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name(
        [x509.NameAttribute(NameOID.COMMON_NAME, "Readocs local development")]
    )
    names = [x509.DNSName("localhost")]
    for host in set(hosts + ["127.0.0.1", "::1"]):
        try:
            names.append(x509.IPAddress(ipaddress.ip_address(host)))
        except ValueError:
            names.append(x509.DNSName(host))
    now = datetime.datetime.now(datetime.UTC)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=365))
        .add_extension(x509.SubjectAlternativeName(names), False)
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
        .sign(key, hashes.SHA256())
    )
    private_file(
        key_path,
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        ),
    )
    private_file(cert_path, cert.public_bytes(serialization.Encoding.PEM))
    return key_path, cert_path


def main():
    parser = argparse.ArgumentParser(description="Readocs 本地内容代理")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--data", type=Path, default=Path(".readocs"))
    parser.add_argument("--https", action="store_true")
    parser.add_argument(
        "--device-port",
        type=int,
        help="Serve device HTTPS on the LAN while keeping management HTTP on localhost",
    )
    parser.add_argument(
        "--device-prefix",
        default="",
        help="Device API path on a shared tunnel hostname, e.g. /readocs",
    )
    parser.add_argument(
        "--name", action="append", default=[], help="Additional TLS host or IP"
    )
    parser.add_argument(
        "--show-key",
        action="store_true",
        help="Print the local admin key to this terminal",
    )
    args = parser.parse_args()
    os.umask(0o077)
    app = create_app(args.data, device_prefix=args.device_prefix)
    if args.show_key:
        print(app.state.store.admin_key)
        return
    if args.host not in ("127.0.0.1", "::1", "localhost") and not args.https:
        parser.error("局域网服务必须使用 --https")
    options = {}
    if args.https:
        key, cert = certificate(args.data, args.name + [local_ip()])
        options = {"ssl_keyfile": str(key), "ssl_certfile": str(cert)}
    print(
        f"Readocs: {'https' if args.https else 'http'}://{local_ip() if args.host == '0.0.0.0' else args.host}:{args.port}"
    )
    print(
        f"管理密钥保存在 {args.data.resolve() / 'admin.key'}；运行 readocs --show-key 查看。"
    )
    if args.device_port:
        if (
            args.https
            or args.host not in ("127.0.0.1", "::1", "localhost")
            or args.device_port == args.port
        ):
            parser.error("--device-port 需与本机 HTTP 管理端口配合使用，且两个端口不同")
        key, cert = certificate(args.data, args.name + [local_ip()])
        print(f"设备代理: https://{local_ip()}:{args.device_port}")

        async def serve_both():
            management = uvicorn.Server(
                uvicorn.Config(app, host=args.host, port=args.port, access_log=False)
            )
            devices = uvicorn.Server(
                uvicorn.Config(
                    app,
                    host="0.0.0.0",
                    port=args.device_port,
                    access_log=False,
                    lifespan="off",
                    ssl_keyfile=str(key),
                    ssl_certfile=str(cert),
                )
            )
            tasks = [
                asyncio.create_task(management.serve()),
                asyncio.create_task(devices.serve()),
            ]
            try:
                await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            finally:
                management.should_exit = devices.should_exit = True
                await asyncio.gather(*tasks)

        asyncio.run(serve_both())
    else:
        uvicorn.run(app, host=args.host, port=args.port, access_log=False, **options)


if __name__ == "__main__":
    main()
