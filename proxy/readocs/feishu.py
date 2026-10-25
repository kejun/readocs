import asyncio
import json
import time
from urllib.parse import urlencode

from .content import document, feishu_blocks
from .errors import Problem

AUTH = "https://accounts.feishu.cn/oauth/v3/token"
DOCS = "https://open.feishu.cn/open-apis/docx/v1/documents/"
API = "https://open.feishu.cn/open-apis/"
USER_AUTH = API + "authen/v2/oauth/token"
TOKEN_ERRORS = {99991661, 99991663, 99991664, 99991665, 99991668, 99991677}


class Feishu:
    def __init__(self, store, network):
        self.store, self.network = store, network
        self.lock = asyncio.Lock()
        self.user_lock = asyncio.Lock()
        self.cached = None

    async def user_exchange(self, config, **grant):
        status, obj = await self.network.json(
            "POST",
            USER_AUTH,
            headers={"Content-Type": "application/json"},
            data=json.dumps(
                {
                    "client_id": config["app_id"],
                    "client_secret": config["secret"],
                    **grant,
                }
            ),
        )
        if status != 200 or obj.get("code", 0) or obj.get("error"):
            raise Problem("USER_AUTH_REQUIRED", "飞书账号授权已失效，请重新授权", 403)
        token, refresh = obj.get("access_token"), obj.get("refresh_token")
        expiry, refresh_expiry = (
            obj.get("expires_in"),
            obj.get("refresh_token_expires_in"),
        )
        if (
            not isinstance(token, str)
            or not 1 <= len(token) <= 16384
            or not isinstance(refresh, str)
            or not 1 <= len(refresh) <= 16384
            or not isinstance(expiry, (int, float))
            or isinstance(expiry, bool)
            or not 1 < expiry <= 86400
            or not isinstance(refresh_expiry, (int, float))
            or isinstance(refresh_expiry, bool)
            or not 1 < refresh_expiry <= 366 * 86400
        ):
            raise Problem(
                "USER_AUTH_REQUIRED",
                "授权未返回离线凭据，请开通 offline_access 后重新授权",
                403,
            )
        required = {
            "drive:drive:readonly",
            "docx:document:readonly",
            "wiki:wiki:readonly",
            "offline_access",
        }
        if "scope" in obj and (
            not isinstance(obj["scope"], str)
            or not required.issubset(set(obj["scope"].split()))
        ):
            raise Problem(
                "SOURCE_ACCESS_DENIED",
                "请开通并授权云盘、云文档、知识库只读权限和 offline_access",
                403,
            )
        return {
            "token": token,
            "refresh_token": refresh,
            "expires": time.time() + expiry,
            "refresh_expires": time.time() + refresh_expiry,
            "generation": config["generation"],
        }

    async def access_token(self, failed=None):
        # Never fall back to an app identity when the user's grant expires.
        async with self.user_lock:
            user = self.store.get("feishu_user")
            if user:
                config = self.store.get("feishu", {})
                if (
                    user["generation"] != config.get("generation")
                    or user["refresh_expires"] <= time.time()
                ):
                    raise Problem("USER_AUTH_REQUIRED", "请重新授权飞书账号", 403)
                if user["expires"] <= time.time() + 120 or user["token"] == failed:
                    result = await self.user_exchange(
                        config,
                        grant_type="refresh_token",
                        refresh_token=user["refresh_token"],
                    )
                    if (
                        self.store.get("feishu", {}).get("generation")
                        != config["generation"]
                    ):
                        raise Problem(
                            "USER_AUTH_REQUIRED", "应用配置已变更，请重新授权", 403
                        )
                    self.store.put("feishu_user", result)
                    user = result
                return user["token"]
        if failed:
            async with self.lock:
                if self.cached and self.cached["token"] == failed:
                    self.cached = None
        return await self.token()

    async def exchange(self, app_id, secret):
        status, obj = await self.network.json(
            "POST",
            AUTH,
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            data=urlencode(
                {
                    "grant_type": "client_credentials",
                    "client_id": app_id,
                    "client_secret": secret,
                }
            ),
        )
        if status != 200 or obj.get("code", 0) != 0 or obj.get("error"):
            raise Problem(
                "APP_CREDENTIALS_INVALID",
                "应用凭据验证失败，请核对 App ID、App Secret 和应用状态",
                422,
            )
        token, expiry = obj.get("access_token"), obj.get("expires_in")
        if (
            not isinstance(token, str)
            or not token
            or len(token) > 16384
            or not isinstance(expiry, (int, float))
            or isinstance(expiry, bool)
            or not 1 < expiry <= 86400
        ):
            raise Problem("PARSE_ERROR", "飞书令牌响应不完整", 502)
        return {
            "token": token,
            "expires": time.time() + expiry,
            "refresh": time.monotonic() + max(1, expiry - min(120, expiry / 5)),
        }

    async def token(self, force=False):
        async with self.lock:
            config = self.store.get("feishu")
            if not config:
                raise Problem("UPSTREAM_AUTH_REQUIRED", "请先配置飞书应用", 403)
            generation = config["generation"]
            if (
                force
                or not self.cached
                or self.cached["generation"] != generation
                or self.cached["refresh"] <= time.monotonic()
            ):
                self.cached = {
                    **await self.exchange(config["app_id"], config["secret"]),
                    "generation": generation,
                }
            return self.cached["token"]

    async def get(self, suffix, params=None, *, api=False):
        url = (
            (API if api else DOCS)
            + suffix
            + ("?" + urlencode(params) if params else "")
        )
        failed = None
        for attempt in range(2):
            token = await self.access_token(failed)
            status, obj = await self.network.json(
                "GET", url, headers={"Authorization": "Bearer " + token}
            )
            code = obj.get("code", 0)
            if (status == 401 or code in TOKEN_ERRORS) and attempt == 0:
                # Only discard the token which actually failed; concurrent requests share its replacement.
                failed = token
                continue
            if status in (403, 400) or code in (1770032, 1770033, 99991672, 99991679):
                raise Problem(
                    "SOURCE_ACCESS_DENIED",
                    "请检查云盘、云文档和知识库只读权限及账号授权范围",
                    403,
                )
            if status == 404:
                raise Problem("SOURCE_GONE", "文档不存在或当前应用无法访问", 404)
            if status != 200 or code:
                raise Problem(
                    "UPSTREAM_AUTH_REQUIRED"
                    if code in TOKEN_ERRORS
                    else "UPSTREAM_ERROR",
                    "飞书请求失败，请检查应用状态与文档权限",
                    502,
                )
            if not isinstance(obj.get("data"), dict):
                raise Problem("PARSE_ERROR", "飞书正文响应不完整", 502)
            return obj["data"]
        raise Problem("UPSTREAM_AUTH_REQUIRED", "飞书授权已失效，请检查应用配置", 403)

    async def fetch(self, did, url="", *, updated=None):
        before = (await self.get(did)).get("document", {})
        if "revision_id" not in before:
            raise Problem("PARSE_ERROR", "文档缺少修订版本", 502)
        raw, page, tokens = [], None, set()
        # Latest-only read needs no historical editing permission. A revision fence detects changes.
        for _ in range(20):
            params = {"page_size": 500, "document_revision_id": -1}
            if page:
                params["page_token"] = page
            data = await self.get(did + "/blocks", params)
            raw.extend(data.get("items", []))
            if len(raw) > 5000:
                raise Problem("CONTENT_TOO_LARGE", "文档结构超过首版处理限制", 422)
            if not data.get("has_more"):
                break
            page = data.get("page_token")
            if not page or page in tokens:
                raise Problem("PARSE_ERROR", "文档分页标识无效", 502)
            tokens.add(page)
        else:
            raise Problem("CONTENT_TOO_LARGE", "文档分页超过首版处理限制", 422)
        after = (await self.get(did)).get("document", {})
        if before["revision_id"] != after.get("revision_id"):
            raise Problem("REVISION_CHANGED", "文档在读取时发生变化，将稍后重试", 409)
        return document(
            did,
            before.get("title", "未命名文档")[:256],
            feishu_blocks(raw),
            url=url,
            updated=updated,
        )
