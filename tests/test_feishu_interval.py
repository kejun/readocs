import pytest
from fastapi.testclient import TestClient
from readocs.store import Store
from test_library import configure, make, settle


@pytest.mark.parametrize("scan_next", [0, 10400])
@pytest.mark.parametrize("failures", [0, 1])
def test_existing_feishu_schedule_moves_once_without_delaying_pending_work(
    tmp_path, scan_next, failures
):
    store = Store(tmp_path)
    store.put("feishu_scan", {"run": "existing", "finished": True, "next": scan_next})
    store.put("feishu_user", {"access_token": "test-token"})
    with store.db:
        store.db.executemany(
            "INSERT INTO sources(id,kind,title,config,interval,generation,due,failures) "
            "VALUES(?,?,?,?,?,?,?,?)",
            [
                (
                    "feishu",
                    "feishu",
                    "Docs",
                    store.encrypt({}),
                    300,
                    7,
                    10300,
                    failures,
                ),
                ("feed", "rss", "RSS", store.encrypt({}), 1800, 8, 11800, 0),
            ],
        )
        store.db.executemany(
            "INSERT INTO feishu_items(id,title,url,seen_run,due,failures) VALUES(?,?,?,?,?,?)",
            [
                ("cached", "Cached", "", "existing", 10300, 0),
                ("pending", "Pending", "", "existing", 0, 0),
                ("retry", "Retry", "", "existing", 10600, 1),
            ],
        )
    store.db.close()

    # Reopening exercises the production upgrade and its idempotence.
    for _ in range(2):
        store = Store(tmp_path)
        source = store.source("feishu")
        assert source["interval"] == 10800
        assert source["generation"] == 7
        assert source["due"] == (10300 if failures else 0)
        assert store.get("feishu_scan")["next"] == (20900 if scan_next else 0)
        assert store.get("feishu_user") == {"access_token": "test-token"}
        due = {
            row["id"]: row["due"]
            for row in store.rows("SELECT id,due FROM feishu_items")
        }
        assert due == {"cached": 20800, "pending": 0, "retry": 10600}
        assert store.source("feed")["interval"] == 1800
        assert store.source("feed")["due"] == 11800
        store.db.close()


async def test_feishu_new_save_resave_and_fetch_use_three_hours(tmp_path, monkeypatch):
    monkeypatch.setattr("readocs.library.time.time", lambda: 2_000_000_000)
    app, network = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        store = app.state.store
        assert store.source("feishu")["interval"] == 10800
        with store.db:
            store.db.execute("UPDATE sources SET interval=300 WHERE id='feishu'")
        configure(app, client)
        assert store.source("feishu")["interval"] == 10800

        await settle(app)
        assert store.get("feishu_scan")["finished"]
        assert store.get("feishu_scan")["next"] == 2_000_010_800
        assert store.source("feishu")["due"] == 2_000_010_800
        assert all(
            item["due"] == 2_000_010_800
            for item in store.rows("SELECT due FROM feishu_items")
        )
        roots = len(network.root_calls)
        await app.state.worker.fetch(store.source("feishu"))
        assert len(network.root_calls) == roots

        response = client.post("/api/sources/feishu/refresh")
        assert response.status_code == 200
        assert store.get("feishu_scan")["next"] == 0
        assert all(
            item["due"] == 0 for item in store.rows("SELECT due FROM feishu_items")
        )
