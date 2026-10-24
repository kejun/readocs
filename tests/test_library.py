import asyncio
import json
import time
from urllib.parse import parse_qs, urlsplit

import pytest
from fastapi.testclient import TestClient
from readocs.app import create_app
from readocs.content import document
from readocs.errors import Problem
from test_proxy import FakeNetwork, login, paired


class CollectionNetwork(FakeNetwork):
    def __init__(self):
        super().__init__()
        self.refreshes = 0
        self.discovered = []
        self.denied = set()
        self.broken_folder = False
        self.root_calls = []

    async def json(self, method, url, **kwargs):
        p = urlsplit(url)
        q = parse_qs(p.query)
        path = p.path.removeprefix("/open-apis/")
        if path == "authen/v2/oauth/token":
            payload = json.loads(kwargs["data"])
            self.refreshes += payload["grant_type"] == "refresh_token"
            await asyncio.sleep(0)
            return 200, {
                "access_token": f"user-{self.refreshes}",
                "refresh_token": f"refresh-{self.refreshes}",
                "expires_in": 7200,
                "refresh_token_expires_in": 86400,
                "scope": "drive:drive:readonly docx:document:readonly wiki:wiki:readonly offline_access",
            }
        data = None
        if path == "drive/v1/files":
            folder = q.get("folder_token", [""])[0]
            page = q.get("page_token", [""])[0]
            self.root_calls.append((folder, page))
            if folder == "folder" and self.broken_folder:
                return 403, {"code": 1770032}
            if not folder:
                if not page:
                    data = {"files": [], "has_more": True, "next_page_token": "second"}
                else:
                    data = {
                        "files": [
                            {"type": "folder", "token": "folder", "name": "Folder"}
                        ]
                        + self.discovered
                    }
            else:
                data = {
                    "files": [
                        {"type": "folder", "token": "folder", "name": "Cycle"},
                        {"type": "docx", "token": "shared", "name": "Duplicate"},
                    ]
                }
        elif path == "wiki/v2/spaces":
            data = {"items": [{"space_id": "123"}]}
        elif path == "wiki/v2/spaces/my_library":
            data = {"space": {"space_id": "456"}}
        elif path == "wiki/v2/spaces/123/nodes":
            if "parent_node_token" in q:
                data = {
                    "items": [
                        {"obj_type": "docx", "obj_token": "nested", "title": "Nested"}
                    ]
                }
            else:
                data = {
                    "items": [
                        {"obj_type": "docx", "obj_token": "shared", "title": "Shared"},
                        {
                            "obj_type": "sheet",
                            "obj_token": "sheet",
                            "node_token": "parent",
                            "title": "Sheet",
                            "has_child": True,
                        },
                    ]
                }
        elif path == "wiki/v2/spaces/456/nodes":
            data = {
                "items": [
                    {"obj_type": "docx", "obj_token": "personal", "title": "Personal"}
                ]
            }
        if data is not None:
            return 200, {"code": 0, "data": data}
        if path.startswith("docx/v1/documents/") and path.split("/")[3] in self.denied:
            return 403, {"code": 1770032}
        return await super().json(method, url, **kwargs)


def make(tmp_path):
    net = CollectionNetwork()
    app = create_app(tmp_path, network=net, background=False)
    return app, net


def configure(app, client):
    login(app, client)
    r = client.put("/api/feishu", json={"app_id": "cli_test", "secret": "private"})
    assert r.status_code == 200, r.text


async def settle(app, count=100):
    for _ in range(count):
        await app.state.worker.fetch(app.state.store.source("feishu"))


async def test_recursive_collection_over_50_deduplicates_and_isolates_failures(
    tmp_path,
):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        net.discovered = [
            {"type": "docx", "token": f"doc{i:03}", "name": f"Document {i}"}
            for i in range(65)
        ]
        net.denied.add("doc001")
        await settle(app, 75)
        store = app.state.store
        progress = store.feishu_progress()
        assert progress == {
            "discovered": 67,
            "synced": 66,
            "failed": 1,
            "scanning": False,
        }
        assert len(store.catalog("feishu")) == 67
        assert store.source("feishu")["error"] == "SOURCE_PARTIAL"
        assert net.root_calls.count(("folder", "")) == 1  # cyclic folder reference
        assert ("", "second") in net.root_calls  # empty first page is not the end
        assert store.manifest()["sources"][0]["collection"] is True
        assert "snapshot" not in store.manifest()["sources"][0]
        first = client.get("/api/sources/feishu/documents").json()
        second = client.get("/api/sources/feishu/documents?cursor=50").json()
        assert len(first["documents"]) == 50 and len(second["documents"]) == 17
        _, _, headers = paired(app, client)
        cursor, ids = 0, []
        while cursor is not None:
            page = client.get(
                f"/v1/sources/feishu/catalog?cursor={cursor}", headers=headers
            ).json()
            ids += [d["document_id"] for d in page["items"]]
            cursor = page["next_cursor"]
        assert len(ids) == len(set(ids)) == 67
        ref = client.get("/v1/sources/feishu/documents/doc064", headers=headers)
        assert ref.status_code == 200 and ref.json()["snapshot"]
        assert client.get("/v1/sources/feishu/documents/doc064").status_code == 401


async def test_unfetched_bodies_continue_after_directory_scan(tmp_path):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        net.discovered = [
            {"type": "docx", "token": f"doc{i:03}", "name": f"Document {i}"}
            for i in range(20)
        ]
        store = app.state.store
        for _ in range(10):
            await app.state.worker.fetch(store.source("feishu"))
            if store.get("feishu_scan")["finished"]:
                break
        assert store.get("feishu_scan")["finished"]
        assert (
            store.db.execute(
                "SELECT COUNT(*) FROM feishu_items WHERE due=0"
            ).fetchone()[0]
            > 0
        )
        # The live worker obeys this deadline; a zero item deadline is due now.
        assert store.source("feishu")["due"] <= time.time() + 2
        synced = store.feishu_progress()["synced"]
        await app.state.worker.fetch(store.source("feishu"))
        assert store.feishu_progress()["synced"] == synced + 1


async def test_partial_discovery_retains_old_and_successful_scan_removes_missing(
    tmp_path,
):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        net.discovered = [{"type": "docx", "token": "old", "name": "Old"}]
        await settle(app, 10)
        store = app.state.store
        old = next(d for d in store.catalog("feishu") if d["document_id"] == "old")
        net.discovered = []
        net.broken_folder = True
        scan = store.get("feishu_scan")
        scan["next"] = 0
        store.put("feishu_scan", scan)
        await settle(app, 10)
        assert old in store.catalog("feishu")
        net.broken_folder = False
        scan = store.get("feishu_scan")
        scan["next"] = 0
        store.put("feishu_scan", scan)
        await settle(app, 10)
        assert not any(d["document_id"] == "old" for d in store.catalog("feishu"))


def test_oauth_single_use_state_cookie_session_and_generation(tmp_path):
    app, _net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        auth = client.post("/api/feishu/authorize").json()
        q = parse_qs(urlsplit(auth["url"]).query)
        assert "offline_access" in q["scope"][0]
        assert auth["redirect_uri"] == "http://testserver/api/feishu/callback"
        state = q["state"][0]
        assert client.get("/api/feishu/callback?state=bad&code=x").status_code == 403
        # A copied link without the browser-bound callback cookie cannot change identity.
        other = TestClient(app)
        assert (
            other.get(f"/api/feishu/callback?state={state}&code=x").status_code == 403
        )
        other.close()


def test_oauth_success_refresh_rotation_and_app_change(tmp_path):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        store = app.state.store
        with store.db:
            store.save_snapshot("feishu", document("old-account", "Private", []))
        auth = client.post("/api/feishu/authorize").json()
        state = parse_qs(urlsplit(auth["url"]).query)["state"][0]
        callback = f"/api/feishu/callback?state={state}&code=test-code"
        response = client.get(callback, follow_redirects=False)
        assert (
            response.status_code == 303
            and "feishu_auth=ok" in response.headers["location"]
        )
        assert not store.catalog("feishu")
        assert client.get(callback).status_code == 403
        user = store.get("feishu_user")
        user["expires"] = 0
        store.put("feishu_user", user)

        async def refresh():
            return await asyncio.gather(
                *(app.state.feishu.access_token() for _ in range(20))
            )

        assert len(set(asyncio.run(refresh()))) == 1
        assert (
            net.refreshes == 1
            and store.get("feishu_user")["refresh_token"] == "refresh-1"
        )
        state_text = client.get("/api/state").text
        assert "refresh-1" not in state_text and "user-1" not in state_text
        configure(app, client)  # same credentials preserve the user's grant
        assert store.get("feishu_user")
        assert (
            client.put(
                "/api/feishu", json={"app_id": "different", "secret": "new"}
            ).status_code
            == 200
        )
        assert store.get("feishu_user") is None


async def test_personal_library_is_enumerated_for_user_and_expired_grant_never_falls_back(
    tmp_path,
):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        config = app.state.store.get("feishu")
        app.state.store.put(
            "feishu_user",
            {
                "token": "user",
                "refresh_token": "refresh",
                "expires": time.time() + 3600,
                "refresh_expires": time.time() + 3600,
                "generation": config["generation"],
            },
        )
        await settle(app, 12)
        assert any(
            d["document_id"] == "personal" for d in app.state.store.catalog("feishu")
        )
        user = app.state.store.get("feishu_user")
        user["refresh_expires"] = 0
        app.state.store.put("feishu_user", user)
        with pytest.raises(Problem, match="USER_AUTH_REQUIRED"):
            await app.state.feishu.access_token()
        assert net.calls == 1


async def test_scan_resumes_after_restart_and_manual_refresh_rescans(tmp_path):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        await app.state.worker.fetch(app.state.store.source("feishu"))
        pending = app.state.store.rows("SELECT * FROM feishu_frontier WHERE done=0")
        assert any(t["cursor"] == "second" for t in pending)
    resumed = create_app(tmp_path, network=net, background=False)
    with TestClient(resumed) as client:
        await settle(resumed, 12)
        assert net.root_calls.count(("", "")) == 1
        assert len(resumed.state.store.catalog("feishu")) == 2
        login(resumed, client)
        net.discovered = [{"type": "docx", "token": "new", "name": "New document"}]
        assert client.post("/api/sources/feishu/refresh").status_code == 200
        await settle(resumed, 12)
        assert len(resumed.state.store.catalog("feishu")) == 3
        _, _, headers = paired(resumed, client)
        compact = client.get(
            "/v1/sources/feishu/catalog?compact=true", headers=headers
        ).json()
        assert len(compact["items"]) == 3
        assert all(
            set(i) == {"document_id", "title", "snapshot_id"} for i in compact["items"]
        )


def test_expired_or_rotated_oauth_state_cannot_replace_grant(tmp_path):
    app, _net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        state = parse_qs(
            urlsplit(client.post("/api/feishu/authorize").json()["url"]).query
        )["state"][0]
        with app.state.store.db:
            app.state.store.db.execute("UPDATE oauth_states SET expires=0")
        assert (
            client.get(f"/api/feishu/callback?state={state}&code=x").status_code == 403
        )
        state = parse_qs(
            urlsplit(client.post("/api/feishu/authorize").json()["url"]).query
        )["state"][0]
        configure(app, client)
        assert (
            client.get(f"/api/feishu/callback?state={state}&code=x").status_code == 403
        )
        assert app.state.store.get("feishu_user") is None
