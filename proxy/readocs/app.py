import asyncio
import hmac
import re
import secrets
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Annotated
from urllib.parse import urlencode, urlsplit

from fastapi import Depends, FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse, RedirectResponse, Response
from fastapi.routing import APIRoute
from pydantic import BaseModel, ConfigDict, Field

from .content import canonical, digest
from .errors import Problem
from .feishu import Feishu
from .network import Network, validate_url
from .store import FEISHU_INTERVAL, Store
from .worker import Worker

STATIC = Path(__file__).parent / "static"
STATIC_TYPES = {
    "index.html": "text/html",
    "app.js": "application/javascript",
    "style.css": "text/css",
    "logo.svg": "image/svg+xml",
}


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


class Login(Input):
    key: str = Field(min_length=1, max_length=256)


class FeishuConfig(Input):
    app_id: str = Field(min_length=1, max_length=128)
    secret: str | None = Field(default=None, max_length=1024)


class FeedConfig(Input):
    url: str = Field(min_length=1, max_length=2048)
    interval: int = 1800


class Pair(Input):
    code: str = Field(min_length=6, max_length=128)
    name: str = Field(default="PaperDIY", min_length=1, max_length=64)


def create_app(directory=None, network=None, background=True, device_prefix=""):
    if device_prefix and not re.fullmatch(r"/[A-Za-z0-9_-]{1,64}", device_prefix):
        raise ValueError("device_prefix must be one URL path segment")
    # Load assets with the routes so an old process cannot serve a newly edited form.
    assets = {name: (STATIC / name).read_bytes() for name in STATIC_TYPES}
    store = Store(Path(directory or ".readocs"))
    network = network or Network()
    feishu = Feishu(store, network)
    worker = Worker(store, network, feishu)

    @asynccontextmanager
    async def lifespan(app):
        task = asyncio.create_task(worker.run()) if background else None
        yield
        worker.stopping = True
        if task:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        store.db.close()

    app = FastAPI(
        title="Readocs",
        version="0.2.0",
        lifespan=lifespan,
        docs_url=None,
        redoc_url=None,
    )
    app.state.store, app.state.worker, app.state.feishu = store, worker, feishu
    failed_logins = {}
    # Includes removals, even when no config exists, to fence absent -> absent races.
    feishu_config_revision = 0

    @app.exception_handler(Problem)
    async def problem_handler(request, e):
        return JSONResponse(
            {"schema_version": 1, "code": e.code, "message": e.message},
            status_code=e.status,
            headers={"Retry-After": str(e.retry_after)} if e.retry_after else {},
        )

    @app.exception_handler(RequestValidationError)
    async def invalid_input(request, e):
        return JSONResponse(
            {"code": "INVALID_INPUT", "message": "请检查填写内容与字段长度"},
            status_code=422,
        )

    @app.middleware("http")
    async def security(request: Request, call_next):
        length = request.headers.get("content-length", "0")
        if not length.isdigit() or int(length) > 16384:
            return JSONResponse(
                {"code": "CONTENT_TOO_LARGE", "message": "请求超过大小限制"},
                status_code=413,
            )
        if request.method not in ("GET", "HEAD", "OPTIONS"):
            body = bytearray()
            async for chunk in request.stream():
                body.extend(chunk)
                if len(body) > 16384:
                    return JSONResponse(
                        {"code": "CONTENT_TOO_LARGE", "message": "请求超过大小限制"},
                        status_code=413,
                    )
            request._body = bytes(body)
            origin = request.headers.get("origin")
            if origin and origin != str(request.base_url).rstrip("/"):
                return JSONResponse(
                    {"code": "ORIGIN_REJECTED", "message": "请从当前管理页操作"},
                    status_code=403,
                )
        # Plain HTTP is a localhost development convenience only. LAN deployment uses TLS.
        if (
            request.url.scheme != "https"
            and request.client
            and request.client.host not in ("127.0.0.1", "::1", "testclient")
        ):
            return JSONResponse(
                {"code": "HTTPS_REQUIRED", "message": "请通过 HTTPS 连接"},
                status_code=403,
            )
        response = await call_next(request)
        response.headers.update(
            {
                "Cache-Control": "no-store",
                "X-Content-Type-Options": "nosniff",
                "Referrer-Policy": "no-referrer",
                "X-Frame-Options": "DENY",
                "Content-Security-Policy": "default-src 'self'; style-src 'self'; script-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'",
            }
        )
        return response

    async def admin(request: Request):
        cookie = request.cookies.get("readocs_session", "")
        row = store.db.execute(
            "SELECT expires FROM sessions WHERE token_hash=?",
            (digest(cookie.encode()),),
        ).fetchone()
        if not cookie or not row or row[0] <= time.time():
            raise Problem("ADMIN_LOGIN_REQUIRED", "请先登录管理页", 401)
        if (
            request.method not in ("GET", "HEAD")
            and request.headers.get("X-Readocs-Request") != "1"
        ):
            raise Problem("ORIGIN_REJECTED", "请从管理页提交操作", 403)

    async def device(request: Request):
        if request.headers.get("X-Protocol-Version", "1") != "1":
            raise Problem("UPGRADE_REQUIRED", "请更新设备固件", 426)
        auth = request.headers.get("Authorization", "")
        token = auth[7:] if auth.startswith("Bearer ") else ""
        rows = store.rows(
            "SELECT * FROM devices WHERE token_hash=? AND revoked=0",
            (digest(token.encode()),),
        )
        if not token or not rows:
            raise Problem("DEVICE_TOKEN_INVALID", "请重新绑定设备", 401)
        with store.db:
            store.db.execute(
                "UPDATE devices SET last_seen=? WHERE id=?",
                (time.time(), rows[0]["id"]),
            )
        return rows[0]

    @app.get("/")
    async def index():
        return Response(assets["index.html"], media_type="text/html")

    @app.get("/static/{name}")
    async def static(name: str):
        if name == "index.html" or name not in STATIC_TYPES:
            raise Problem("NOT_FOUND", "页面不存在", 404)
        return Response(assets[name], media_type=STATIC_TYPES[name])

    @app.post("/api/login")
    async def login(payload: Login, request: Request):
        ip = request.client.host
        cutoff, attempts = failed_logins.get(ip, (0, 0))
        if cutoff > time.time() and attempts >= 5:
            raise Problem("RATE_LIMITED", "尝试次数过多，请稍后重试", 429, 60)
        if not hmac.compare_digest(payload.key.encode(), store.admin_key.encode()):
            failed_logins[ip] = (
                time.time() + 60,
                attempts + 1 if cutoff > time.time() else 1,
            )
            raise Problem("ADMIN_KEY_INVALID", "管理密钥不正确", 401)
        failed_logins.pop(ip, None)
        token = secrets.token_urlsafe(32)
        with store.db:
            store.db.execute("DELETE FROM sessions WHERE expires<?", (time.time(),))
            store.db.execute(
                "INSERT INTO sessions VALUES(?,?)",
                (digest(token.encode()), time.time() + 43200),
            )
        response = JSONResponse({"ok": True})
        response.set_cookie(
            "readocs_session",
            token,
            httponly=True,
            samesite="strict",
            secure=request.url.scheme == "https",
            max_age=43200,
        )
        return response

    @app.post("/api/logout", dependencies=[Depends(admin)])
    async def logout(request: Request):
        with store.db:
            store.db.execute(
                "DELETE FROM sessions WHERE token_hash=?",
                (digest(request.cookies.get("readocs_session", "").encode()),),
            )
        response = JSONResponse({"ok": True})
        response.delete_cookie("readocs_session")
        return response

    @app.get("/api/state", dependencies=[Depends(admin)])
    async def state():
        config = store.get("feishu", {})
        sources = []
        for s in store.rows("SELECT * FROM sources ORDER BY kind,id"):
            cfg = store.decrypt(s["config"])
            sources.append(
                {
                    k: s[k]
                    for k in (
                        "id",
                        "kind",
                        "title",
                        "interval",
                        "checked",
                        "fetched",
                        "error",
                    )
                }
                | {
                    "url": cfg.get("url", ""),
                    "count": len(store.catalog(s["id"])),
                    "progress": store.feishu_progress()
                    if s["kind"] == "feishu"
                    else None,
                }
            )
        return {
            "schema_version": 1,
            "sources": sources,
            "feishu": {
                "app_id": config.get("app_id", ""),
                "secret_set": bool(config.get("secret")),
                "expires": feishu.cached["expires"] if feishu.cached else None,
                "user_authorized": bool(store.get("feishu_user")),
                "user_expires": store.get("feishu_user", {}).get("expires"),
            },
            "devices": store.rows(
                "SELECT id,name,created,last_seen,revoked FROM devices ORDER BY created DESC"
            ),
            "jobs": store.rows("SELECT id,source_id,state FROM jobs"),
        }

    def credentials(payload):
        old = store.get("feishu", {})
        secret = payload.secret or (
            old.get("secret") if old.get("app_id") == payload.app_id else None
        )
        if not secret:
            raise Problem("APP_SECRET_REQUIRED", "请输入与该 App ID 对应的 App Secret")
        return secret

    @app.post("/api/feishu/test", dependencies=[Depends(admin)])
    async def test_feishu(payload: FeishuConfig):
        result = await feishu.exchange(payload.app_id.strip(), credentials(payload))
        return {"ok": True, "expires": result["expires"]}

    @app.put("/api/feishu", dependencies=[Depends(admin)])
    async def save_feishu(payload: FeishuConfig):
        nonlocal feishu_config_revision
        revision = feishu_config_revision
        old = store.get("feishu", {})
        secret = credentials(payload)
        result = await feishu.exchange(payload.app_id.strip(), secret)
        if revision != feishu_config_revision:
            raise Problem("CONFIG_CHANGED", "飞书配置已变更，请重新保存", 409)
        feishu_config_revision += 1
        # Fence removed/re-created configurations against old in-flight requests too.
        generation = time.time_ns()
        changed = (
            old.get("app_id") != payload.app_id.strip() or old.get("secret") != secret
        )
        store.put(
            "feishu",
            {
                "app_id": payload.app_id.strip(),
                "secret": secret,
                "generation": generation,
            },
        )
        feishu.cached = {**result, "generation": generation}
        sid = "feishu"
        with store.db:
            store.reset_feishu_library(clear=changed)
            store.db.execute("DELETE FROM oauth_states")
            if changed:
                store.db.execute("DELETE FROM settings WHERE key='feishu_user'")
            store.db.execute(
                "INSERT INTO sources(id,kind,title,config,interval,generation) VALUES(?,?,?,?,?,?) "
                "ON CONFLICT(id) DO UPDATE SET title=excluded.title,config=excluded.config,interval=excluded.interval,generation=excluded.generation,due=0,error=NULL",
                (
                    sid,
                    "feishu",
                    "飞书文档",
                    store.encrypt({"scope": "all"}),
                    FEISHU_INTERVAL,
                    generation,
                ),
            )
        if not changed and (user := store.get("feishu_user")):
            store.put("feishu_user", {**user, "generation": generation})
        return {"ok": True, "job_id": store.enqueue(sid, refresh=True)}

    @app.post("/api/feishu/authorize", dependencies=[Depends(admin)])
    async def authorize_feishu(request: Request):
        config = store.get("feishu")
        if not config:
            raise Problem("UPSTREAM_AUTH_REQUIRED", "请先保存飞书应用配置", 403)
        nonce = secrets.token_urlsafe(32)
        redirect = str(request.base_url).rstrip("/") + "/api/feishu/callback"
        with store.db:
            store.db.execute("DELETE FROM oauth_states WHERE expires<?", (time.time(),))
            store.db.execute(
                "INSERT INTO oauth_states VALUES(?,?,?,?,?)",
                (
                    digest(nonce.encode()),
                    digest(request.cookies["readocs_session"].encode()),
                    config["generation"],
                    redirect,
                    time.time() + 600,
                ),
            )
        url = "https://accounts.feishu.cn/open-apis/authen/v1/authorize?" + urlencode(
            {
                "client_id": config["app_id"],
                "response_type": "code",
                "redirect_uri": redirect,
                "state": nonce,
                "scope": "drive:drive:readonly docx:document:readonly wiki:wiki:readonly offline_access",
            }
        )
        response = JSONResponse({"url": url, "redirect_uri": redirect})
        # The regular admin cookie stays Strict; only this short-lived callback binding is Lax.
        response.set_cookie(
            "readocs_oauth",
            nonce,
            httponly=True,
            samesite="lax",
            secure=request.url.scheme == "https",
            max_age=600,
            path="/api/feishu/callback",
        )
        return response

    @app.get("/api/feishu/callback")
    async def feishu_callback(
        request: Request, state: str = "", code: str = "", error: str = ""
    ):
        hashed = digest(state.encode())
        row = store.db.execute(
            "SELECT * FROM oauth_states WHERE hash=?", (hashed,)
        ).fetchone()
        config = store.get("feishu", {})
        valid = (
            row
            and row["expires"] > time.time()
            and row["generation"] == config.get("generation")
            and hmac.compare_digest(state, request.cookies.get("readocs_oauth", ""))
            and store.db.execute(
                "SELECT 1 FROM sessions WHERE token_hash=? AND expires>?",
                (row["session_hash"], time.time()),
            ).fetchone()
        )
        if not valid:
            raise Problem(
                "OAUTH_STATE_INVALID", "授权会话已失效，请从管理页重新授权", 403
            )
        with store.db:
            store.db.execute("DELETE FROM oauth_states WHERE hash=?", (hashed,))
        outcome = "failed"
        if code and len(code) <= 4096 and not error:
            try:
                async with feishu.user_lock:
                    user = await feishu.user_exchange(
                        config,
                        grant_type="authorization_code",
                        code=code,
                        redirect_uri=row["redirect"],
                    )
                    if (
                        store.get("feishu", {}).get("generation")
                        != config["generation"]
                    ):
                        raise Problem(
                            "USER_AUTH_REQUIRED", "应用配置已变更，请重新授权", 403
                        )
                    # A new authorization may belong to a different account: never mix private caches.
                    with store.db:
                        store.reset_feishu_library(clear=True)
                        store.db.execute(
                            "UPDATE sources SET generation=generation+1,due=0,error=NULL WHERE id='feishu'"
                        )
                    store.put("feishu_user", user)
                    store.enqueue("feishu")
                    outcome = "ok"
            except Problem:
                pass
        response = RedirectResponse(
            "/?feishu_auth=" + outcome + "#feishu", status_code=303
        )
        response.delete_cookie("readocs_oauth", path="/api/feishu/callback")
        return response

    @app.delete("/api/feishu", dependencies=[Depends(admin)])
    async def remove_feishu():
        nonlocal feishu_config_revision
        feishu_config_revision += 1
        with store.db:
            store.db.execute(
                "DELETE FROM settings WHERE key IN ('feishu','feishu_user')"
            )
            store.reset_feishu_library(clear=True)
            store.db.execute("DELETE FROM oauth_states")
            store.db.execute("DELETE FROM sources WHERE kind='feishu'")
            store.db.execute("DELETE FROM documents WHERE source_id='feishu'")
            store.db.execute("DELETE FROM jobs WHERE source_id='feishu'")
        feishu.cached = None
        return {"ok": True}

    @app.post("/api/feeds", dependencies=[Depends(admin)])
    async def add_feed(payload: FeedConfig):
        validate_url(payload.url)
        if payload.interval not in (600, 900, 1800, 3600, 10800):
            raise Problem("INVALID_INTERVAL", "请选择可用的检查间隔")
        sources = store.rows("SELECT * FROM sources WHERE kind='rss'")
        if any(store.decrypt(s["config"])["url"] == payload.url for s in sources):
            raise Problem("SOURCE_EXISTS", "这个订阅源已经添加")
        if len(sources) >= 2:
            raise Problem("SOURCE_LIMIT", "首版最多添加两个 RSS 或 Atom 源")
        sid = secrets.token_hex(8)
        with store.db:
            store.db.execute(
                "INSERT INTO sources(id,kind,title,config,interval) VALUES(?,?,?,?,?)",
                (
                    sid,
                    "rss",
                    urlsplit(payload.url).hostname,
                    store.encrypt({"url": payload.url}),
                    payload.interval,
                ),
            )
        return {"id": sid, "job_id": store.enqueue(sid)}

    @app.delete("/api/sources/{sid}", dependencies=[Depends(admin)])
    async def remove_source(sid: str):
        s = store.source(sid)
        if not s or s["kind"] != "rss":
            raise Problem("NOT_FOUND", "订阅源不存在", 404)
        with store.db:
            store.db.execute("DELETE FROM sources WHERE id=?", (sid,))
            store.db.execute("DELETE FROM documents WHERE source_id=?", (sid,))
            store.db.execute("DELETE FROM jobs WHERE source_id=?", (sid,))
        return {"ok": True}

    @app.post("/api/sources/{sid}/refresh", dependencies=[Depends(admin)])
    async def refresh_source(sid: str):
        if not store.source(sid):
            raise Problem("NOT_FOUND", "内容源不存在", 404)
        return {"job_id": store.enqueue(sid, refresh=True)}

    @app.get("/api/sources/{sid}/documents", dependencies=[Depends(admin)])
    async def documents(sid: str, cursor: int = 0):
        items = store.catalog(sid)
        if cursor < 0 or cursor > len(items):
            raise Problem("INVALID_CURSOR", "目录位置无效")
        return {
            "documents": items[cursor : cursor + 50],
            "next_cursor": cursor + 50 if cursor + 50 < len(items) else None,
            "count": len(items),
        }

    @app.get("/api/preview/{snapshot_id}", dependencies=[Depends(admin)])
    async def preview(snapshot_id: str):
        row = store.db.execute(
            "SELECT body FROM snapshots WHERE id=?", (snapshot_id,)
        ).fetchone()
        if not row:
            raise Problem("NOT_FOUND", "正文尚未下载", 404)
        return Response(row[0], media_type="application/json")

    @app.post("/api/pairings", dependencies=[Depends(admin)])
    async def pairing():
        code = secrets.token_hex(6).upper()
        with store.db:
            store.db.execute("DELETE FROM pairings WHERE expires<?", (time.time(),))
            store.db.execute(
                "INSERT INTO pairings VALUES(?,?)",
                (digest(code.encode()), time.time() + 300),
            )
        return {"code": code, "expires_in": 300}

    @app.post("/v1/pair")
    async def pair(payload: Pair):
        code_hash = digest(payload.code.upper().encode())
        did, token = secrets.token_hex(12), secrets.token_urlsafe(32)
        with store.db:
            result = store.db.execute(
                "DELETE FROM pairings WHERE code_hash=? AND expires>?",
                (code_hash, time.time()),
            )
            if result.rowcount != 1:
                raise Problem("PAIRING_INVALID", "配对码无效或已过期，请重新生成", 401)
            store.db.execute(
                "INSERT INTO devices(id,name,token_hash,created) VALUES(?,?,?,?)",
                (did, payload.name, digest(token.encode()), time.time()),
            )
        return {"schema_version": 1, "device_id": did, "device_token": token}

    @app.delete("/api/devices/{did}", dependencies=[Depends(admin)])
    async def revoke(did: str):
        with store.db:
            store.db.execute("UPDATE devices SET revoked=1 WHERE id=?", (did,))
        return {"ok": True}

    @app.get("/v1/sync")
    async def sync(request: Request, d: Annotated[dict, Depends(device)]):
        body = canonical(store.manifest())
        if len(body) > 8192:
            raise Problem("CONTENT_TOO_LARGE", "同步清单超过协议限制", 500)
        etag = '"' + digest(body) + '"'
        # Intermediaries can weaken ETags when compressing the response.
        # If-None-Match uses weak comparison, including each tag in a list.
        condition = request.headers.get("If-None-Match", "")
        if condition.strip() == "*" or etag in re.findall(r'"[^"\r\n]*"', condition):
            return Response(status_code=304, headers={"ETag": etag})
        return Response(body, media_type="application/json", headers={"ETag": etag})

    @app.get("/v1/sources/{sid}/catalog")
    async def catalog(
        sid: str,
        d: Annotated[dict, Depends(device)],
        revision: str = "",
        cursor: int = 0,
        compact: bool = False,
    ):
        if not store.source(sid):
            raise Problem("SOURCE_ACCESS_DENIED", "该内容源已移除", 403)
        items = store.catalog(sid)
        current = store.catalog_revision(items)
        if revision and revision != current:
            raise Problem("SNAPSHOT_EXPIRED", "目录已更新，请重新检查", 409)
        if cursor < 0 or cursor > len(items):
            raise Problem("INVALID_CURSOR", "目录位置无效")
        page = items[cursor : cursor + (50 if compact else 10)]
        if compact:
            page = [
                {k: i[k] for k in ("document_id", "title", "snapshot_id")} for i in page
            ]
        return (
            {
                "schema_version": 1,
                "catalog_revision": current,
                "items": [
                    {
                        **i,
                        "snapshot": store.reference(i["snapshot_id"])
                        if i["snapshot_id"]
                        else None,
                    }
                    for i in ([] if compact else page)
                ],
                "next_cursor": cursor + len(page)
                if cursor + len(page) < len(items)
                else None,
            }
            if not compact
            else {
                "schema_version": 1,
                "catalog_revision": current,
                "items": page,
                "next_cursor": cursor + len(page)
                if cursor + len(page) < len(items)
                else None,
            }
        )

    @app.get("/v1/sources/{sid}/documents/{did}")
    async def document_reference(
        sid: str, did: str, d: Annotated[dict, Depends(device)]
    ):
        if not store.source(sid):
            raise Problem("SOURCE_ACCESS_DENIED", "该内容源已移除", 403)
        rows = store.rows(
            "SELECT id AS document_id,title,snapshot_id FROM documents WHERE source_id=? AND id=?",
            (sid, did),
        )
        if not rows:
            raise Problem("SOURCE_GONE", "文档已移除，请重新同步目录", 404)
        doc = rows[0]
        return {
            **doc,
            "snapshot": store.reference(doc["snapshot_id"])
            if doc["snapshot_id"]
            else None,
        }

    @app.get("/v1/snapshots/{snapshot_id}/chunks/{index}")
    async def chunk(snapshot_id: str, index: int, d: Annotated[dict, Depends(device)]):
        row = store.db.execute(
            "SELECT p.body FROM snapshots p JOIN sources s ON s.id=p.source_id WHERE p.id=?",
            (snapshot_id,),
        ).fetchone()
        if not row:
            raise Problem("SOURCE_ACCESS_DENIED", "快照不可访问，请重新同步", 403)
        body = row[0]
        if index < 0 or index * 32768 >= len(body):
            raise Problem("NOT_FOUND", "分块不存在", 404)
        return Response(
            body[index * 32768 : (index + 1) * 32768],
            media_type="application/octet-stream",
        )

    @app.post("/v1/refresh", status_code=202)
    async def refresh(d: Annotated[dict, Depends(device)]):
        if time.time() - d["refreshed"] < 30:
            raise Problem("RATE_LIMITED", "请稍后再检查", 429, 30)
        with store.db:
            store.db.execute(
                "UPDATE devices SET refreshed=? WHERE id=?", (time.time(), d["id"])
            )
        return {
            "schema_version": 1,
            "job_ids": [
                store.enqueue(s["id"], refresh=True)
                for s in store.rows(
                    "SELECT id FROM sources WHERE due<=? OR error IS NULL",
                    (time.time(),),
                )
            ],
        }

    @app.get("/v1/status")
    async def status(d: Annotated[dict, Depends(device)]):
        return {
            "schema_version": 1,
            "sources": store.rows(
                "SELECT id,checked,fetched,error FROM sources ORDER BY id"
            ),
            "pending": store.db.execute("SELECT count(*) FROM jobs").fetchone()[0],
        }

    if device_prefix:
        # A shared tunnel hostname only publishes this device API namespace.
        for route in list(app.routes):
            if isinstance(route, APIRoute) and route.path.startswith("/v1/"):
                app.add_api_route(
                    device_prefix + route.path,
                    route.endpoint,
                    methods=route.methods,
                    dependencies=route.dependencies,
                    status_code=route.status_code,
                    response_class=route.response_class,
                    include_in_schema=False,
                )
    return app
