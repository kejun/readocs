"""Exercise real reads, persistent discovery and jobs with transient failures."""

import logging
import sqlite3
import time
from urllib.parse import parse_qs, urlsplit

import pytest
from fastapi.testclient import TestClient
from readocs.content import document
from readocs.errors import Problem, retry_delay
from readocs.store import Store
from test_library import configure, make, settle
from test_proxy import FakeNetwork


async def test_retry_failed_page_once_without_restarting_document(
    tmp_path, monkeypatch, caplog
):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        calls, delays = [], []

        class Flaky(FakeNetwork):
            async def json(self, method, url, **kwargs):
                if method != "GET":
                    return await super().json(method, url, **kwargs)
                page = parse_qs(urlsplit(url).query).get("page_token", [""])[0]
                kind = "blocks" if "/blocks" in url else "document"
                calls.append((kind, page))
                if kind == "document":
                    return await super().json(method, url, **kwargs)
                if not page:
                    return 200, {
                        "data": {
                            "items": [
                                {
                                    "block_id": "root",
                                    "block_type": 1,
                                    "children": ["text"],
                                }
                            ],
                            "has_more": True,
                            "page_token": "private-cursor",
                        }
                    }
                if calls.count((kind, page)) == 1:
                    raise Problem("UPSTREAM_TIMEOUT", "private upstream response", 502)
                return 200, {
                    "data": {
                        "items": [
                            {
                                "block_id": "text",
                                "block_type": 2,
                                "text": {
                                    "elements": [{"text_run": {"content": "完整正文"}}]
                                },
                            }
                        ]
                    }
                }

        async def pause(delay):
            delays.append(delay)

        app.state.feishu.network = Flaky()
        app.state.feishu.cached = {
            "token": "private-token",
            "refresh": time.monotonic() + 3600,
            "generation": app.state.store.get("feishu")["generation"],
        }
        monkeypatch.setattr("readocs.feishu.asyncio.sleep", pause)
        result = await app.state.feishu.fetch("root")
        assert [b["text"] for b in result["blocks"]] == ["完整正文"]
        assert calls == [
            ("document", ""),
            ("blocks", ""),
            ("blocks", "private-cursor"),
            ("blocks", "private-cursor"),
            ("document", ""),
        ]
        assert delays == [1]
        assert "stage=blocks" in caplog.text
        assert "private" not in caplog.text


@pytest.mark.parametrize("failure", ["timeout", "permission", "rate", "oauth"])
async def test_retries_bounded_and_never_replay_oauth_or_access_denials(
    tmp_path, monkeypatch, failure
):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        calls = []

        class Broken(FakeNetwork):
            async def json(self, method, url, **kwargs):
                calls.append(method)
                if failure in ("timeout", "oauth"):
                    raise Problem("UPSTREAM_TIMEOUT", "private", 502)
                if failure == "permission":
                    return 403, {"code": 1770032}
                return 400, {"code": 99991400}

        async def pause(delay):
            assert delay == 1

        app.state.feishu.network = Broken()
        if failure == "oauth":
            app.state.feishu.cached = None
        monkeypatch.setattr("readocs.feishu.asyncio.sleep", pause)
        with pytest.raises(Problem) as raised:
            await app.state.feishu.get("doc")
        assert (
            raised.value.code
            == {
                "timeout": "UPSTREAM_TIMEOUT",
                "permission": "SOURCE_ACCESS_DENIED",
                "rate": "RATE_LIMITED",
                "oauth": "UPSTREAM_TIMEOUT",
            }[failure]
        )
        assert calls == (
            ["GET", "GET"]
            if failure == "timeout"
            else ["POST"]
            if failure == "oauth"
            else ["GET"]
        )


@pytest.mark.parametrize(
    "error,delay",
    [
        (Problem("UPSTREAM_TIMEOUT", "", 502), 30),
        (TimeoutError(), 30),
        (Problem("REVISION_CHANGED", "", 409), 5),
    ],
)
async def test_failed_body_keeps_snapshot_retries_soon_and_recovers_first(
    tmp_path, monkeypatch, caplog, error, delay
):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        await settle(app, 12)
        store = app.state.store
        with store.db:
            for did in ("aaa-healthy", "zzz-failed"):
                store.db.execute(
                    "INSERT INTO feishu_items(id,title,url,seen_run,due) VALUES(?,?,?,'run',0)",
                    (did, did, ""),
                )
                store.save_snapshot(
                    "feishu",
                    document(
                        did,
                        did,
                        [
                            {
                                "block_id": "old",
                                "text": "旧正文",
                                "type": "paragraph",
                                "depth": 0,
                                "meta": {},
                            }
                        ],
                    ),
                )
            store.db.execute(
                "UPDATE feishu_items SET error='UPSTREAM_TIMEOUT' WHERE id='zzz-failed'"
            )
        old = next(
            d["snapshot_id"]
            for d in store.catalog("feishu")
            if d["document_id"] == "zzz-failed"
        )
        calls = []

        async def broken(did, url, **kwargs):
            calls.append(did)
            raise error

        monkeypatch.setattr(app.state.feishu, "fetch", broken)
        before = time.time()
        await app.state.worker.fetch(store.source("feishu"))
        row = store.rows("SELECT * FROM feishu_items WHERE id='zzz-failed'")[0]
        assert calls == ["zzz-failed"]
        assert before + delay <= row["due"] <= time.time() + delay
        assert row["failures"] == 1
        assert (
            next(
                d["snapshot_id"]
                for d in store.catalog("feishu")
                if d["document_id"] == "zzz-failed"
            )
            == old
        )
        assert store.source("feishu")["error"] == "SOURCE_PARTIAL"

        # Manual refresh queues failed documents ahead of hundreds of healthy ones.
        assert client.post("/api/sources/feishu/refresh").status_code == 200

        async def recovered(did, url, **kwargs):
            calls.append(did)
            return document(did, did, [])

        monkeypatch.setattr(app.state.feishu, "fetch", recovered)
        caplog.set_level(logging.INFO)
        await app.state.worker.fetch(store.source("feishu"))
        assert calls == ["zzz-failed", "zzz-failed"]
        row = store.rows("SELECT * FROM feishu_items WHERE id='zzz-failed'")[0]
        assert row["error"] is None and row["failures"] == 0
        assert row["due"] >= before + 10800
        assert "feishu_document_recovered document=zzz-failed" in caplog.text


async def test_directory_timeout_retains_cursor_and_other_work_continues(
    tmp_path, monkeypatch
):
    app, _net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        await settle(app, 12)
        store = app.state.store
        original = store.catalog("feishu")
        scan = store.get("feishu_scan")
        store.put("feishu_scan", {**scan, "next": 0})
        page = app.state.worker.library.page
        failures = 0

        async def flaky(task):
            nonlocal failures
            if task["kind"] == "drive" and task["cursor"] == "second" and not failures:
                failures += 1
                raise Problem("UPSTREAM_TIMEOUT", "private", 502)
            return await page(task)

        monkeypatch.setattr(app.state.worker.library, "page", flaky)
        for _ in range(12):
            await app.state.worker.fetch(store.source("feishu"))
        row = store.rows("SELECT * FROM feishu_frontier WHERE error IS NOT NULL")[0]
        assert row["cursor"] == "second" and row["done"] == 0
        assert row["failures"] == 1 and row["due"] > time.time()
        assert store.get("feishu_scan")["finished"] is False
        assert store.source("feishu")["due"] == row["due"]
        assert {d["document_id"]: d["snapshot_id"] for d in original} == {
            d["document_id"]: d["snapshot_id"] for d in store.catalog("feishu")
        }
        assert all(
            t["done"]
            for t in store.rows(
                "SELECT * FROM feishu_frontier WHERE id<>?", (row["id"],)
            )
        )
        # A fresh process resumes the exact page, then finishes cleanly.
        reopened = Store(tmp_path)
        assert reopened.rows(
            "SELECT cursor,failures FROM feishu_frontier WHERE id=?", (row["id"],)
        )[0] == {"cursor": "second", "failures": 1}
        reopened.db.close()
        with store.db:
            store.db.execute(
                "UPDATE feishu_frontier SET due=0 WHERE id=?", (row["id"],)
            )
        await settle(app, 12)
        assert store.get("feishu_scan")["finished"]
        assert store.source("feishu")["error"] is None
        assert store.rows(
            "SELECT error,failures FROM feishu_frontier WHERE id=?", (row["id"],)
        )[0] == {"error": None, "failures": 0}

        # Persistent failure cannot hold future healthy-folder scans hostage.
        store.put("feishu_scan", {**store.get("feishu_scan"), "next": 0})

        async def unavailable(task):
            if task["kind"] == "drive":
                raise Problem("UPSTREAM_TIMEOUT", "private", 502)
            return await page(task)

        monkeypatch.setattr(app.state.worker.library, "page", unavailable)
        for _ in range(20):
            with store.db:
                store.db.execute("UPDATE feishu_frontier SET due=0 WHERE done=0")
            await app.state.worker.fetch(store.source("feishu"))
        assert store.get("feishu_scan")["finished"]
        assert store.source("feishu")["error"] == "SOURCE_PARTIAL"
        assert {d["document_id"] for d in original}.issubset(
            {d["document_id"] for d in store.catalog("feishu")}
        )
        assert store.rows(
            "SELECT done,failures,error FROM feishu_frontier WHERE kind='drive'"
        )[0] == {"done": 1, "failures": 6, "error": "UPSTREAM_TIMEOUT"}


async def test_rate_limit_defers_source_and_honors_server_delay(tmp_path, monkeypatch):
    app, _ = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)

        async def limited(source):
            raise Problem("RATE_LIMITED", "private", 429, 120)

        monkeypatch.setattr(app.state.worker, "fetch", limited)
        job = app.state.store.rows("SELECT * FROM jobs")[0]
        before = time.time()
        await app.state.worker.execute(job)
        source = app.state.store.source("feishu")
        assert source["error"] == "RATE_LIMITED"
        assert before + 120 <= source["due"] <= time.time() + 130
        assert not app.state.store.rows("SELECT * FROM jobs")


def test_backoff_capped_and_permissions_keep_slow_schedule():
    assert [retry_delay(TimeoutError(), n, 10800) for n in range(1, 9)] == [
        30,
        60,
        120,
        240,
        480,
        900,
        900,
        900,
    ]
    assert retry_delay(Problem("RATE_LIMITED", "", 429, 1800), 1, 10800) == 1800
    assert retry_delay(Problem("SOURCE_ACCESS_DENIED", "", 403), 1, 10800) == 10800


def test_existing_frontier_migration_is_idempotent(tmp_path):
    store = Store(tmp_path)
    store.db.close()
    with sqlite3.connect(tmp_path / "readocs.sqlite3") as db:
        db.execute("DROP TABLE feishu_frontier")
        db.execute(
            "CREATE TABLE feishu_frontier(id TEXT PRIMARY KEY,kind TEXT,target TEXT,cursor TEXT,visited TEXT,done INTEGER,error TEXT)"
        )
        db.execute(
            "INSERT INTO feishu_frontier VALUES('saved','drive','folder','next','[]',0,NULL)"
        )
    for _ in range(2):
        store = Store(tmp_path)
        assert store.rows("SELECT cursor,done,due,failures FROM feishu_frontier")[
            0
        ] == {"cursor": "next", "done": 0, "due": 0, "failures": 0}
        store.db.close()


async def test_slow_pagination_and_body_fit_worker_budget(tmp_path, monkeypatch):
    # Scale seconds to centiseconds so the former 20/25/60s nested deadlines
    # fail without making the suite wait for real network timeout durations.
    import asyncio

    app, _net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        store = app.state.store
        with store.db:
            store.db.execute(
                "INSERT INTO feishu_items(id,title,url,seen_run) VALUES('slow','Slow','','run')"
            )
        original_timeout = asyncio.timeout
        monkeypatch.setattr(
            asyncio, "timeout", lambda delay: original_timeout(delay / 100)
        )

        async def slow_page(task):
            await asyncio.sleep(0.25)
            return [], [], ""

        async def slow_body(did, url, **kwargs):
            await asyncio.sleep(0.70)
            return document(did, "Slow", [])

        monkeypatch.setattr(app.state.worker.library, "page", slow_page)
        monkeypatch.setattr(app.state.feishu, "fetch", slow_body)
        await app.state.worker.execute(store.rows("SELECT * FROM jobs")[0])
        row = next(d for d in store.catalog("feishu") if d["document_id"] == "slow")
        assert row["snapshot_id"] and row["sync_error"] is None
        assert store.source("feishu")["error"] is None
