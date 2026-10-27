import time

import pytest
from fastapi.testclient import TestClient
from readocs.errors import Problem
from readocs.store import Store
from test_library import configure, make, settle
from test_proxy import paired


async def drain(app, limit=100):
    store = app.state.store
    for _ in range(limit):
        jobs = store.rows("SELECT * FROM jobs")
        if not jobs:
            return
        for job in jobs:
            await app.state.worker.execute(job)


async def test_manual_refresh_starts_every_source_including_failed_backoff(tmp_path):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        feed = client.post(
            "/api/feeds", json={"url": "https://example.com/rss", "interval": 600}
        ).json()["id"]
        store = app.state.store
        await drain(app)
        with store.db:
            store.db.execute(
                "UPDATE sources SET due=?,error='UPSTREAM_TIMEOUT',failures=1",
                (time.time() + 3600,),
            )
        _, _, headers = paired(app, client)
        assert client.post("/v1/refresh").status_code == 401
        response = client.post("/v1/refresh", headers=headers)
        assert response.status_code == 202
        rid = response.json()["refresh_id"]
        assert {j["source_id"] for j in store.rows("SELECT * FROM jobs")} == {
            "feishu",
            feed,
        }
        assert len(response.json()["job_ids"]) == 2
        assert client.post("/v1/refresh", headers=headers).status_code == 429
        assert store.get("device_refresh")["id"] == rid
        assert client.get("/v1/status").status_code == 401
        progress = client.get(f"/v1/status?refresh_id={rid}", headers=headers).json()
        assert progress["refresh"]["state"] == "running"
        assert progress["refresh"]["completed_sources"] == 0
        await drain(app)
        assert store.refresh_progress(rid)["state"] == "running"


async def test_full_collection_progress_survives_empty_queue_and_restart(tmp_path):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        net.discovered = [
            {"type": "docx", "token": f"doc{i:03}", "name": f"Doc {i}"}
            for i in range(20)
        ]
        store = app.state.store
        await settle(app, 5)
        result = store.refresh_all()
        rid = result["refresh_id"]
        await drain(app)
        progress = store.refresh_progress(rid)
        assert not store.rows("SELECT * FROM jobs")
        assert progress["state"] == "running"
        assert progress["processed_documents"] < progress["total_documents"]
        # Joining must not reset already fetched documents or directory scan.
        due = store.rows("SELECT id,due FROM feishu_items ORDER BY id")
        assert store.refresh_all() == result
        assert store.rows("SELECT id,due FROM feishu_items ORDER BY id") == due
        reopened = Store(tmp_path)
        assert reopened.refresh_progress(rid)["state"] == "running"
        reopened.db.close()
        await settle(app)
        completed = store.refresh_progress(rid)
        assert completed["state"] == "completed"
        assert completed["completed_sources"] == completed["total_sources"] == 1
        assert completed["processed_documents"] == completed["total_documents"] == 22
        store.enqueue("feishu", refresh=True)
        assert store.refresh_progress(rid) == completed
        replacement = store.refresh_all()
        assert replacement["refresh_id"] != rid
        assert store.refresh_progress(rid) == {"id": rid, "state": "superseded"}


async def test_manual_refresh_waits_for_transient_document_recovery(tmp_path):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        await settle(app)
        store = app.state.store
        rid = store.refresh_all()["refresh_id"]
        await drain(app)
        await settle(app)
        with store.db:
            store.db.execute(
                "UPDATE feishu_items SET error='UPSTREAM_TIMEOUT',failures=1,due=? WHERE id='shared'",
                (time.time() + 30,),
            )
            store.db.execute("UPDATE sources SET error='SOURCE_PARTIAL'")
        assert store.refresh_progress(rid)["state"] == "running"
        with store.db:
            store.db.execute("UPDATE feishu_items SET due=0 WHERE id='shared'")
        await settle(app, 1)
        assert store.refresh_progress(rid)["state"] == "completed"


@pytest.mark.parametrize("code", ["USER_AUTH_REQUIRED", "INTERNAL_ERROR"])
async def test_failed_attempt_is_explicit_and_does_not_poll_forever(tmp_path, code):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        store = app.state.store
        rid = store.refresh_all()["refresh_id"]

        async def fail(_):
            if code == "INTERNAL_ERROR":
                raise ValueError("private upstream payload")
            raise Problem(code, "授权失效", 401)

        app.state.worker.fetch = fail
        await drain(app)
        progress = store.refresh_progress(rid)
        assert progress["state"] == "partial"
        assert progress["failed_sources"] == 1
        assert progress["sources"][0]["error"] == code
        assert "private upstream payload" not in str(store.get("device_refresh"))


async def test_source_transport_retry_remains_pending_until_recovery(tmp_path):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        store = app.state.store
        rid = store.refresh_all()["refresh_id"]
        original = app.state.worker.fetch

        async def fail(_):
            raise Problem("UPSTREAM_TIMEOUT", "暂时不可用", 502)

        app.state.worker.fetch = fail
        await drain(app)
        assert store.refresh_progress(rid)["state"] == "running"
        app.state.worker.fetch = original
        await settle(app)
        assert store.refresh_progress(rid)["state"] == "completed"


async def test_stale_and_expired_requests_require_new_manual_check(tmp_path):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        store = app.state.store
        assert store.refresh_progress("old") == {"id": "old", "state": "superseded"}
        rid = store.refresh_all()["refresh_id"]
        record = store.get("device_refresh")
        store.put("device_refresh", {**record, "started": time.time() - 7201})
        assert store.refresh_progress(rid)["state"] == "expired"
        rid = store.refresh_all()["refresh_id"]
        with store.db:
            store.db.execute("UPDATE sources SET generation=generation+1")
        progress = store.refresh_progress(rid)
        assert progress["state"] == "partial"
        assert progress["sources"][0]["error"] == "SOURCE_CHANGED"


async def test_permanent_body_failure_settles_with_retained_content(tmp_path):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        await settle(app)
        store = app.state.store
        before = store.catalog("feishu")
        net.denied.add("shared")
        rid = store.refresh_all()["refresh_id"]
        await drain(app)
        await settle(app)
        assert store.refresh_progress(rid)["state"] == "partial"
        assert {
            d["document_id"]: d["snapshot_id"] for d in store.catalog("feishu")
        } == {d["document_id"]: d["snapshot_id"] for d in before}
