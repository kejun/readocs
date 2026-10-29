import time

import pytest
from fastapi.testclient import TestClient
from readocs.content import document
from test_library import configure, make, settle


@pytest.mark.parametrize("manual", [False, True])
async def test_bodies_check_recent_edits_first_without_changing_reading_order(
    tmp_path, monkeypatch, manual
):
    app, net = make(tmp_path)
    with TestClient(app) as client:
        configure(app, client)
        await settle(app, 12)
        store = app.state.store
        now = time.time()
        rows = [
            # The oldest deadline / smallest ID must not beat a recent edit.
            ("aaa-old", "2026-01-01", None, "2026-01-01", 0, None),
            ("zzz-edit", "旧文章 2026-01-02", "2026-01-02", "2026-10-29", 1, None),
            ("bbb-new", "新文章", "2026-10-28", "2026-10-28", 0, None),
            ("ccc-created", "创建时间", "2026-10-27", None, 0, None),
            ("ddd-invalid", "无效更新时间", "2026-10-26", "invalid", 0, None),
            ("eee-title", "早报 2026-10-25", None, None, 0, None),
            ("fff-unknown", "无日期", None, None, 0, None),
            # Eligible failures still recover first; future retries stay asleep.
            ("retry", "失败旧文", None, "2026-01-01", 0, "UPSTREAM_TIMEOUT"),
            (
                "future",
                "退避中的新文",
                None,
                "2026-10-30",
                now + 900,
                "UPSTREAM_TIMEOUT",
            ),
        ]
        with store.db:
            for did, title, created, updated, due, error in rows:
                store.db.execute(
                    "INSERT INTO feishu_items(id,title,url,seen_run,due,error) VALUES(?,?,'','run',?,?)",
                    (did, title, due, error),
                )
                store.db.execute(
                    "INSERT INTO documents(source_id,id,title,url,created,updated,seen) "
                    "VALUES('feishu',?,?,'',?,?,0)",
                    (did, title, created, updated),
                )
        if manual:
            net.discovered = [
                {
                    "type": "docx",
                    "token": did,
                    "name": title,
                    "created_time": created,
                    "modified_time": updated,
                }
                for did, title, created, updated, _, _ in rows
            ]
            store.refresh_all()
            # This retry was scheduled after the manual request (e.g. a 429).
            with store.db:
                store.db.execute(
                    "UPDATE feishu_items SET due=? WHERE id='future'", (now + 900,)
                )
        before = [d["document_id"] for d in store.catalog("feishu")]
        calls = []

        async def fetch(did, url, *, updated):
            calls.append(did)
            title = next(row[1] for row in rows if row[0] == did)
            return document(did, title, [], updated=updated)

        monkeypatch.setattr(app.state.feishu, "fetch", fetch)
        for _ in range(8):
            await app.state.worker.fetch(store.source("feishu"))
        assert calls == [
            "retry",
            "zzz-edit",
            "bbb-new",
            "ccc-created",
            "ddd-invalid",
            "eee-title",
            "aaa-old",
            "fff-unknown",
        ]
        assert [d["document_id"] for d in store.catalog("feishu")] == before
        assert (
            store.rows("SELECT due FROM feishu_items WHERE id='future'")[0]["due"]
            == now + 900
        )
