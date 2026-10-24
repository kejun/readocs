import asyncio
import json
import time

import pytest
from fastapi.testclient import TestClient
from readocs.app import create_app
from readocs.content import document, parse_feed
from readocs.errors import Problem
from readocs.network import PublicResolver, validate_url
from readocs.store import Store, canonical, digest


class FakeNetwork:
    def __init__(self):
        self.calls = 0
        self.bad = False
        self.revision = 1
        self.change_revision = False
        self.deny_doc = False

    async def json(self, method, url, **kwargs):
        if "oauth/v3/token" in url:
            self.calls += 1
            await asyncio.sleep(0)
            if self.bad:
                return 200, {"code": 1, "message": "do not expose this secret"}
            return 200, {
                "access_token": "test-token-" + str(self.calls),
                "expires_in": 7200,
            }
        if self.deny_doc:
            return 403, {"code": 1770032}
        if "/blocks" in url:
            if self.change_revision:
                self.revision += 1
            return 200, {
                "code": 0,
                "data": {
                    "items": [
                        {
                            "block_id": "docx123456",
                            "block_type": 1,
                            "children": ["second", "first"],
                        },
                        {
                            "block_id": "first",
                            "block_type": 2,
                            "text": {"elements": [{"text_run": {"content": "第一段"}}]},
                        },
                        {
                            "block_id": "second",
                            "block_type": 2,
                            "text": {"elements": [{"text_run": {"content": "第二段"}}]},
                        },
                    ]
                },
            }
        return 200, {
            "code": 0,
            "data": {
                "document": {
                    "title": "真实接口的测试替身",
                    "revision_id": self.revision,
                }
            },
        }

    async def request(self, method, url, **kwargs):
        return (
            200,
            {},
            b'<rss version="2.0"><channel><title>Feed</title><item><guid>a</guid><title>A</title><description>Hello</description></item></channel></rss>',
        )


@pytest.fixture
def env(tmp_path):
    network = FakeNetwork()
    app = create_app(tmp_path, network=network, background=False)
    with TestClient(app) as client:
        yield app, client, network


def login(app, client):
    assert (
        client.post("/api/login", json={"key": app.state.store.admin_key}).status_code
        == 200
    )
    client.headers["X-Readocs-Request"] = "1"


def paired(app, client):
    login(app, client)
    code = client.post("/api/pairings").json()["code"]
    r = client.post("/v1/pair", json={"code": code})
    assert r.status_code == 200
    result = r.json()
    return (
        code,
        result,
        {
            "Authorization": "Bearer " + result["device_token"],
            "X-Protocol-Version": "1",
        },
    )


def test_auth_pairing_revocation_precedes_conditional_request(env):
    app, client, _network = env
    assert client.get("/api/state").status_code == 401
    code, pair, headers = paired(app, client)
    assert client.post("/v1/pair", json={"code": code}).status_code == 401
    first = client.get("/v1/sync", headers=headers)
    assert first.status_code == 200
    headers["If-None-Match"] = first.headers["etag"]
    cached = client.get("/v1/sync", headers=headers)
    assert cached.status_code == 304 and cached.content == b""
    client.delete("/api/devices/" + pair["device_id"])
    assert client.get("/v1/sync", headers=headers).status_code == 401
    assert client.put(
        "/api/feishu", headers={"Authorization": headers["Authorization"]}, json={}
    ).status_code in (401, 422)


def test_conditional_sync_accepts_proxy_weak_etags(env):
    app, client, _network = env
    _code, pair, headers = paired(app, client)
    etag = client.get("/v1/sync", headers=headers).headers["etag"]
    for condition in (etag, "W/" + etag, '"other", W/' + etag, "*"):
        response = client.get(
            "/v1/sync", headers={**headers, "If-None-Match": condition}
        )
        assert response.status_code == 304 and response.content == b""
    assert (
        client.get(
            "/v1/sync", headers={**headers, "If-None-Match": 'W/"other,tag"'}
        ).status_code
        == 200
    )
    client.delete("/api/devices/" + pair["device_id"])
    assert (
        client.get(
            "/v1/sync", headers={**headers, "If-None-Match": "W/" + etag}
        ).status_code
        == 401
    )


def test_secret_write_only_and_failed_rotation_keeps_configuration(env):
    app, client, _network = env
    login(app, client)
    payload = {
        "app_id": "cli_test",
        "secret": "private-secret",
    }
    assert client.put("/api/feishu", json=payload).status_code == 200
    state = client.get("/api/state").text
    assert "private-secret" not in state and "test-token-" not in state
    _network.bad = True
    payload["secret"] = "bad-secret"
    assert client.put("/api/feishu", json=payload).status_code == 422
    assert app.state.store.get("feishu")["secret"] == "private-secret"
    assert (
        b"private-secret"
        not in (app.state.store.directory / "readocs.sqlite3").read_bytes()
    )
    assert (
        client.post("/api/feishu/test", json={"app_id": "different"}).status_code == 400
    )


async def test_token_refresh_coalesces_and_uses_actual_expiry(tmp_path):
    net = FakeNetwork()
    app = create_app(tmp_path, network=net, background=False)
    app.state.store.put("feishu", {"app_id": "id", "secret": "secret", "generation": 1})
    f = app.state.feishu
    tokens = await asyncio.gather(*(f.token() for _ in range(20)))
    assert len(set(tokens)) == 1 and net.calls == 1
    f.cached["refresh"] = time.monotonic() - 1
    await asyncio.gather(*(f.token() for _ in range(20)))
    assert net.calls == 2
    app.state.store.db.close()


async def test_revision_fence_and_tree_order(tmp_path):
    net = FakeNetwork()
    app = create_app(tmp_path, network=net, background=False)
    app.state.store.put("feishu", {"app_id": "id", "secret": "secret", "generation": 1})
    doc = await app.state.feishu.fetch("docx123456")
    assert [b["text"] for b in doc["blocks"]] == ["第二段", "第一段"]
    net.change_revision = True
    with pytest.raises(Problem, match="REVISION_CHANGED"):
        await app.state.feishu.fetch("docx123456")
    app.state.store.db.close()


def test_feed_identity_summary_namespaces_and_sanitization():
    def feed(date):
        return f"""<rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/"><channel><title>订阅</title><item><guid>x</guid><title>文章</title><pubDate>{date}</pubDate><content:encoded><![CDATA[<p>正文</p><script>secret</script><p>下一段</p>]]></content:encoded></item></channel></rss>""".encode()

    _, docs = parse_feed(feed("today"), "source1")
    _, changed = parse_feed(feed("tomorrow"), "source1")
    _, other = parse_feed(feed("today"), "source2")
    assert (
        docs[0]["document_id"] == changed[0]["document_id"] != other[0]["document_id"]
    )
    assert docs[0]["content_revision"] == changed[0]["content_revision"]
    assert not docs[0]["summary"] and "secret" not in canonical(docs).decode()
    atom = b"""<feed xmlns="http://www.w3.org/2005/Atom"><title>A</title><entry><id>a</id><title>B</title><content type="html">&lt;p&gt;hello&lt;/p&gt;</content></entry></feed>"""
    assert parse_feed(atom, "a")[1][0]["blocks"][0]["text"] == "hello"
    with pytest.raises(Problem):
        parse_feed(b'<!DOCTYPE a [<!ENTITY x SYSTEM "file:///etc/passwd">]><rss/>', "a")
    with pytest.raises(Problem):
        parse_feed(b"<rss><channel><item>", "a")


def test_snapshot_stable_on_check_and_partial_update_rollback(tmp_path):
    store = Store(tmp_path)
    doc = document(
        "doc",
        "标题",
        [
            {
                "block_id": "a",
                "type": "paragraph",
                "text": "正文",
                "depth": 0,
                "meta": {},
            }
        ],
    )
    with store.db:
        sid = store.save_snapshot("s", doc)
    changed = {**doc, "content_updated_at": "tomorrow"}
    with store.db:
        assert store.save_snapshot("s", changed) == sid
    try:
        with store.db:
            store.save_snapshot("s", document("other", "其他", []))
            raise RuntimeError("power failure before commit")
    except RuntimeError:
        pass
    assert len(store.catalog("s")) == 1
    assert store.reference(sid)["sha256"] == sid
    store.db.close()


@pytest.mark.parametrize(
    "url",
    [
        "http://127.0.0.1/a",
        "http://[::1]/",
        "https://169.254.169.254",
        "http://192.168.1.1",
        "file:///etc/passwd",
        "https://u:p@example.com",
        "http://localhost",
        "https://example.com:8000/a",
    ],
)
def test_ssrf_rejects_non_public_targets(url):
    with pytest.raises(Problem):
        validate_url(url)


async def test_dns_check_is_at_connection(monkeypatch):
    loop = asyncio.get_running_loop()

    async def answer(*a, **k):
        import socket

        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("127.0.0.1", 80))]

    monkeypatch.setattr(loop, "getaddrinfo", answer)
    with pytest.raises(Problem, match="URL_BLOCKED"):
        await PublicResolver().resolve("public-looking.example", 80)


def test_csrf_and_limits(env):
    app, client, _network = env
    login(app, client)
    assert (
        client.post(
            "/api/pairings", headers={"Origin": "https://evil.example"}
        ).status_code
        == 403
    )
    assert (
        client.post("/api/feeds", json={"url": "https://example.com/a"}).status_code
        == 200
    )
    assert (
        client.post("/api/feeds", json={"url": "https://example.com/a"}).status_code
        == 400
    )
    assert (
        client.post("/api/feeds", json={"url": "https://example.com/b"}).status_code
        == 200
    )
    assert (
        client.post("/api/feeds", json={"url": "https://example.com/c"}).status_code
        == 400
    )


async def test_worker_preserves_old_snapshot_on_failure(tmp_path):
    net = FakeNetwork()
    app = create_app(tmp_path, network=net, background=False)
    store = app.state.store
    with store.db:
        store.db.execute(
            "INSERT INTO sources(id,kind,title,config,interval) VALUES(?,?,?,?,?)",
            (
                "s",
                "rss",
                "Test",
                store.encrypt({"url": "https://example.com/rss"}),
                1800,
            ),
        )
    await app.state.worker.fetch(store.source("s"))
    old = store.manifest()

    async def invalid(*args, **kw):
        return 200, {}, b"broken xml"

    net.request = invalid
    jid = store.enqueue("s")
    await app.state.worker.execute(
        store.rows("SELECT * FROM jobs WHERE id=?", (jid,))[0]
    )
    assert store.manifest() == old
    assert store.source("s")["error"] == "PARSE_ERROR"
    store.db.close()


def test_full_protocol_chunk_integrity_and_removed_source_access(env):
    app, client, _net = env
    _code, _pair, headers = paired(app, client)
    store = app.state.store
    with store.db:
        store.db.execute(
            "INSERT INTO sources(id,kind,title,config,interval) VALUES(?,?,?,?,?)",
            (
                "s",
                "rss",
                "测试",
                store.encrypt({"url": "https://example.com/feed"}),
                1800,
            ),
        )
        store.save_snapshot(
            "s",
            document(
                "d",
                "跨块正文",
                [
                    {
                        "block_id": "p",
                        "type": "paragraph",
                        "text": "阅读" * 10000,
                        "depth": 0,
                        "meta": {},
                    }
                ],
            ),
        )
    manifest = client.get("/v1/sync", headers=headers).json()
    catalog = client.get("/v1/sources/s/catalog", headers=headers).json()
    ref = catalog["items"][0]["snapshot"]
    body = b""
    assert len(ref["chunks"]) >= 2
    for chunk in ref["chunks"]:
        part = client.get(
            f"/v1/snapshots/{ref['snapshot_id']}/chunks/{chunk['index']}",
            headers=headers,
        )
        assert len(part.content) <= 32768
        assert digest(part.content) == chunk["sha256"]
        body += part.content
    assert digest(body) == ref["sha256"] and len(body) == ref["bytes"]
    assert json.loads(body)["document_id"] == "d"
    rev = manifest["sources"][0]["catalog_revision"]
    assert (
        client.get("/v1/sources/s/catalog?revision=wrong", headers=headers).status_code
        == 409
    )
    assert (
        client.get("/v1/sources/s/catalog?revision=" + rev, headers=headers).status_code
        == 200
    )
    client.delete("/api/sources/s")
    assert (
        client.get(
            f"/v1/snapshots/{ref['snapshot_id']}/chunks/0", headers=headers
        ).status_code
        == 403
    )


def test_body_updates_do_not_interrupt_catalog_pagination(env):
    app, client, _net = env
    _code, _pair, headers = paired(app, client)
    store = app.state.store
    with store.db:
        store.db.execute(
            "INSERT INTO sources(id,kind,title,config,interval) VALUES(?,?,?,?,?)",
            ("feishu", "feishu", "Documents", store.encrypt({}), 300),
        )
        for i in range(65):
            store.db.execute(
                "INSERT INTO documents(source_id,id,title,url,seen) VALUES('feishu',?,?,'',0)",
                (f"doc{i:03}", f"Title {i}"),
            )
    response = client.get("/v1/sync", headers=headers)
    original = response.json()["sources"][0]
    revision = original["catalog_revision"]
    first = client.get(
        f"/v1/sources/feishu/catalog?compact=true&revision={revision}", headers=headers
    ).json()
    assert first["next_cursor"] == 50
    with store.db:
        store.save_snapshot("feishu", document("doc064", "Title 64", []))
    updated = client.get(
        "/v1/sync", headers={**headers, "If-None-Match": response.headers["etag"]}
    )
    assert updated.status_code == 200
    source = updated.json()["sources"][0]
    assert source["catalog_revision"] == revision
    assert source["body_revision"] != original["body_revision"]
    second = client.get(
        f"/v1/sources/feishu/catalog?compact=true&cursor=50&revision={revision}",
        headers=headers,
    )
    assert second.status_code == 200
    assert second.json()["items"][-1]["snapshot_id"]
    assert (
        len({item["document_id"] for item in first["items"] + second.json()["items"]})
        == 65
    )
    with store.db:
        store.db.execute("UPDATE documents SET title='Changed title' WHERE id='doc064'")
    assert (
        client.get(
            f"/v1/sources/feishu/catalog?revision={revision}", headers=headers
        ).status_code
        == 409
    )


def test_validation_does_not_echo_secrets(env):
    app, client, _network = env
    login(app, client)
    response = client.post(
        "/api/feishu/test",
        json={"app_id": "id", "secret": "secret-input", "unexpected": "field"},
    )
    assert response.status_code == 422
    assert "secret-input" not in response.text


def test_atom_xhtml_keeps_paragraph_boundaries_and_declared_chinese_encoding():
    xml = b"""<feed xmlns="http://www.w3.org/2005/Atom"><title>A</title><entry><id>x</id><title>T</title><content type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml"><p>First</p><p>Second</p><script>bad</script></div></content></entry></feed>"""
    docs = parse_feed(xml, "s")[1]
    text = "\n".join(b["text"] for b in docs[0]["blocks"])
    assert (
        "First" in text
        and "Second" in text
        and "FirstSecond" not in text
        and "bad" not in text
    )
    legacy = '<?xml version="1.0" encoding="GB18030"?><rss version="2.0"><channel><title>中文</title><item><guid>1</guid><title>文章</title><description>正常正文</description></item></channel></rss>'.encode(
        "gb18030"
    )
    title, docs = parse_feed(legacy, "legacy")
    assert title == "中文" and docs[0]["blocks"][0]["text"] == "正常正文"


def test_assets_and_routes_are_loaded_together(tmp_path, monkeypatch):
    import readocs.app as module

    static = tmp_path / "static"
    static.mkdir()
    for name in ("index.html", "app.js", "style.css", "logo.svg"):
        (static / name).write_text("before")
    monkeypatch.setattr(module, "STATIC", static)
    app = module.create_app(tmp_path / "data", network=FakeNetwork(), background=False)
    for name in ("index.html", "app.js", "style.css", "logo.svg"):
        (static / name).write_text("after")
    with TestClient(app) as client:
        assert client.get("/").text == "before"
        assert client.get("/static/app.js").text == "before"
        assert client.get("/static/style.css").text == "before"
        logo = client.get("/static/logo.svg")
        assert logo.text == "before"
        assert logo.headers["content-type"].startswith("image/svg+xml")
    updated = module.create_app(
        tmp_path / "data", network=FakeNetwork(), background=False
    )
    with TestClient(updated) as client:
        assert client.get("/").text == "after"


def test_tunnel_prefix_publishes_device_protocol_only(tmp_path):
    app = create_app(
        tmp_path, network=FakeNetwork(), background=False, device_prefix="/readocs"
    )
    with TestClient(app) as client:
        login(app, client)
        pairing = client.post("/api/pairings").json()
        pair = client.post("/readocs/v1/pair", json={"code": pairing["code"]})
        assert pair.status_code == 200
        headers = {"Authorization": "Bearer " + pair.json()["device_token"]}
        assert client.get("/readocs/v1/sync").status_code == 401
        manifest = client.get("/readocs/v1/sync", headers=headers)
        assert manifest.status_code == 200
        assert (
            client.get(
                "/readocs/v1/sync",
                headers={**headers, "If-None-Match": manifest.headers["etag"]},
            ).status_code
            == 304
        )
        assert client.get("/readocs/v1/status", headers=headers).status_code == 200
        assert client.post("/readocs/v1/refresh", headers=headers).status_code == 202
        assert client.get("/readocs/api/state").status_code == 404
        assert client.get("/readocs/").status_code == 404
        client.delete("/api/devices/" + pair.json()["device_id"])
        assert (
            client.get(
                "/readocs/v1/sync",
                headers={**headers, "If-None-Match": manifest.headers["etag"]},
            ).status_code
            == 401
        )
