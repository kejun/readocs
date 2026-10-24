import sqlite3

from fastapi.testclient import TestClient
from readocs.app import create_app
from readocs.content import document
from readocs.store import Store
from test_library import configure, make, settle
from test_proxy import login, paired


def test_article_order_shared_by_admin_and_device(tmp_path):
    app = create_app(tmp_path, background=False)
    with TestClient(app) as client:
        login(app, client)
        store = app.state.store
        with store.db:
            for sid, kind in (("feishu", "feishu"), ("feed", "rss")):
                store.db.execute(
                    "INSERT INTO sources(id,kind,title,config,interval) VALUES(?,?,?,?,?)",
                    (sid, kind, sid, store.encrypt({}), 300),
                )
            for did, title, created, updated in (
                ("a", "早报 2026-04-29", "2026-10-24T00:00:00Z", None),
                ("z", "早报 2026-10-24", "2026-04-29T00:00:00Z", None),
                ("b", "新文章", "2026-10-23T00:00:00Z", None),
                ("c", "早报 2026年10月22日", "2026-04-29T00:00:00Z", None),
                ("d", "无效日期 2026-02-30", "2026-10-21T00:00:00Z", None),
                ("e", "只有更新时间", None, "2026-10-20T00:00:00Z"),
                ("f", "没有日期", None, None),
                ("g", "同日早报 2026/10/24", None, None),
            ):
                # Include pending articles, which also need the right position.
                store.db.execute(
                    "INSERT INTO documents(source_id,id,title,url,created,updated,seen) VALUES('feishu',?,?, '',?,?,0)",
                    (did, title, created, updated),
                )
            for did, date in (
                ("a", "Wed, 21 Oct 2026 23:00:00 -0500"),
                ("z", "2026-10-22T05:00:00Z"),
                ("unknown", "invalid"),
            ):
                store.save_snapshot("feed", document(did, did, [], updated=date))
            # A later fetch of an old article must not move it to the top.
            store.save_snapshot(
                "feed",
                document("a", "a", [], updated="Wed, 21 Oct 2026 23:00:00 -0500"),
            )
        _, _, headers = paired(app, client)
        for sid, expected in (
            ("feishu", ["g", "z", "b", "c", "d", "e", "a", "f"]),
            ("feed", ["z", "a", "unknown"]),
        ):
            admin = client.get(f"/api/sources/{sid}/documents").json()["documents"]
            assert [item["document_id"] for item in admin] == expected
            for suffix in ("", "?compact=true"):
                device = client.get(
                    f"/v1/sources/{sid}/catalog{suffix}", headers=headers
                ).json()["items"]
                assert [item["document_id"] for item in device] == expected


async def test_discovery_dates_survive_body_fetch_and_refresh(tmp_path):
    app, net = make(tmp_path)
    original_json = net.json

    async def dated_json(method, url, **kwargs):
        status, obj = await original_json(method, url, **kwargs)
        if "/wiki/v2/spaces/123/nodes" in url:
            for item in obj.get("data", {}).get("items", []):
                item.update(obj_create_time="1792791000", obj_edit_time="1792792000")
        return status, obj

    net.json = dated_json
    net.discovered = [
        {
            "type": "docx",
            "token": "dated",
            "name": "Dated",
            "created_time": "1792791000",
            "modified_time": "1792792000",
        }
    ]
    with TestClient(app) as client:
        configure(app, client)
        await settle(app, 12)
        store = app.state.store
        for did in ("dated", "shared"):
            before = next(
                item for item in store.catalog("feishu") if item["document_id"] == did
            )
            assert before["snapshot_id"]
            assert before["content_created_at"] == "2026-10-23T21:30:00+00:00"
            assert before["content_updated_at"] == "2026-10-23T21:46:40+00:00"
            with store.db:
                for _ in range(2):
                    store.save_snapshot("feishu", document(did, "Changed body", []))
            after = next(
                item for item in store.catalog("feishu") if item["document_id"] == did
            )
            assert after["content_created_at"] == before["content_created_at"]
            assert after["content_updated_at"] == before["content_updated_at"]


def test_existing_database_adds_dates_without_losing_articles(tmp_path):
    with sqlite3.connect(tmp_path / "readocs.sqlite3") as db:
        db.executescript("""
        CREATE TABLE documents(
          source_id TEXT NOT NULL, id TEXT NOT NULL, title TEXT NOT NULL,
          url TEXT NOT NULL, snapshot_id TEXT, updated TEXT, seen REAL NOT NULL,
          summary INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(source_id,id));
        INSERT INTO documents(source_id,id,title,url,seen) VALUES('s','old','Old','',0);
        """)
    for _ in range(2):
        store = Store(tmp_path)
        assert store.catalog("s")[0]["document_id"] == "old"
        assert store.catalog("s")[0]["content_created_at"] is None
        store.db.close()
