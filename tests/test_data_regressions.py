import json
import sqlite3
from datetime import UTC, datetime, timedelta
from xml.sax.saxutils import escape

import pytest
from readocs.content import document, parse_feed
from readocs.feishu import Feishu
from readocs.library import Library
from readocs.store import Store, canonical, digest
from readocs.worker import Worker
from test_proxy import FakeNetwork


@pytest.fixture
def clock(monkeypatch):
    now = [datetime(2026, 10, 25, tzinfo=UTC).timestamp()]
    monkeypatch.setattr("readocs.store.time.time", lambda: now[0])
    return now


def feed(entries):
    items = []
    for entry in entries:
        fields = {"title": "Daily", "description": "Body", **entry}
        items.append(
            "<item>"
            + "".join(
                f"<{key}>{escape(str(value))}</{key}>" for key, value in fields.items()
            )
            + "</item>"
        )
    return (
        '<rss version="2.0"><channel><title>Feed</title>'
        + "".join(items)
        + "</channel></rss>"
    ).encode()


class FeedNetwork:
    def __init__(self, *responses):
        self.responses = list(responses)
        self.headers = []

    async def request(self, method, url, *, headers, max_bytes):
        assert max_bytes == 16 * 1024 * 1024
        self.headers.append(headers)
        return self.responses.pop(0)


def add_source(store, sid="rss", kind="rss"):
    with store.db:
        store.db.execute(
            "INSERT INTO sources(id,kind,title,config,interval) VALUES(?,?,?,?,?)",
            (sid, kind, sid, store.encrypt({"url": "https://example.com/feed"}), 300),
        )


def stored_body(store, snapshot):
    row = store.db.execute(
        "SELECT body FROM snapshots WHERE id=?", (snapshot,)
    ).fetchone()
    return json.loads(row[0]) if row else None


async def test_304_renews_current_members_after_restart_but_expires_history(
    tmp_path, clock
):
    store = Store(tmp_path)
    add_source(store)
    network = FeedNetwork(
        (200, {"ETag": "one"}, feed([{"guid": "old"}, {"guid": "current"}])),
        (200, {"ETag": "two"}, feed([{"guid": "current"}])),
    )
    worker = Worker(store, network, None)
    await worker.fetch(store.source("rss"))
    clock[0] += 86400
    await worker.fetch(store.source("rss"))
    assert len(store.catalog("rss")) == 2
    store.db.close()

    store = Store(tmp_path)
    network = FeedNetwork(*[(304, {"ETag": "two"}, b"")] * 8)
    worker = Worker(store, network, None)
    for _ in range(8):
        clock[0] += 86400
        await worker.fetch(store.source("rss"))
    items = store.catalog("rss")
    assert len(items) == 1
    assert items[0]["document_id"] == digest(b"rss\0current")[:32]
    assert items[0]["snapshot_id"]
    assert all(headers == {"If-None-Match": "two"} for headers in network.headers)
    store.db.close()


async def test_legacy_database_refetches_members_and_preserves_fallback_id(
    tmp_path, clock
):
    store = Store(tmp_path)
    add_source(store)
    legacy_id = digest(b"rss\0fallback:Daily")[:32]
    old_doc = document(
        legacy_id,
        "Daily",
        [
            {
                "block_id": "p-old",
                "type": "paragraph",
                "text": "Body",
                "depth": 0,
                "meta": {},
            }
        ],
    )
    with store.db:
        snapshot = store.save_snapshot("rss", old_doc)
        store.db.execute("UPDATE sources SET upstream_etag='old-validator'")
    original = stored_body(store, snapshot)
    store.db.close()
    # Recreate the database layout shipped before membership and lease tracking.
    with sqlite3.connect(tmp_path / "readocs.sqlite3") as db:
        db.executescript("""
        DROP TABLE feed_members;
        DROP TABLE feed_fallbacks;
        DROP TABLE snapshot_leases;
        ALTER TABLE sources DROP COLUMN feed_members_known;
        ALTER TABLE documents DROP COLUMN created;
        """)
    store = Store(tmp_path)
    assert store.catalog("rss")[0]["document_id"] == legacy_id
    assert stored_body(store, snapshot) == original
    network = FeedNetwork((200, {"ETag": "new-validator"}, feed([{}])))
    await Worker(store, network, None).fetch(store.source("rss"))
    assert network.headers == [{}]
    assert store.source("rss")["feed_members_known"] == 1
    assert [item["document_id"] for item in store.catalog("rss")] == [legacy_id]
    store.db.close()


def test_replaced_old_snapshot_retains_24_hours_and_reference_extends_lease(
    tmp_path, clock
):
    store = Store(tmp_path)
    add_source(store)
    with store.db:
        old = store.save_snapshot("rss", document("d", "Old", []))
    clock[0] += 8 * 86400
    # A snapshot's age does not shorten the lease when it stops being current.
    with store.db:
        store.save_snapshot("rss", document("d", "New", []))
    store.collect_snapshots()
    clock[0] += 23 * 3600
    store.collect_snapshots()
    assert stored_body(store, old)
    assert store.reference(old)
    store.db.close()

    store = Store(tmp_path)
    clock[0] += 2 * 3600
    store.collect_snapshots()
    assert stored_body(store, old)
    clock[0] += 22 * 3600 + 1
    store.collect_snapshots()
    assert stored_body(store, old) is None
    assert stored_body(store, store.catalog("rss")[0]["snapshot_id"])
    store.db.close()


def test_unleased_replaced_snapshot_expires_after_full_retirement_day(tmp_path, clock):
    store = Store(tmp_path)
    add_source(store)
    with store.db:
        old = store.save_snapshot("rss", document("d", "Old", []))
    clock[0] += 8 * 86400
    with store.db:
        store.save_snapshot("rss", document("d", "New", []))
    store.collect_snapshots()
    clock[0] += 86400 - 1
    store.collect_snapshots()
    assert stored_body(store, old)
    clock[0] += 2
    store.collect_snapshots()
    assert stored_body(store, old) is None
    store.db.close()


async def test_feed_quotas_use_article_dates_before_xml_order(tmp_path, clock):
    entries = [
        {
            "guid": str(i),
            "title": f"Article {i}",
            "pubDate": (
                datetime(2026, 1, 1, tzinfo=UTC) + timedelta(days=i)
            ).isoformat(),
        }
        for i in range(60)
    ]
    store = Store(tmp_path)
    add_source(store)
    network = FeedNetwork(
        (200, {}, feed(entries)), (200, {}, feed(list(reversed(entries))))
    )
    worker = Worker(store, network, None)
    await worker.fetch(store.source("rss"))
    items = store.catalog("rss")
    assert [item["title"] for item in items] == [
        f"Article {i}" for i in range(59, 9, -1)
    ]
    assert all(item["snapshot_id"] for item in items)
    await worker.fetch(store.source("rss"))
    assert store.catalog("rss") == items
    store.db.close()


def test_same_date_and_unknown_date_quotas_have_stable_id_order():
    entries = [{"guid": str(i), "pubDate": "2026-10-25T00:00:00Z"} for i in range(60)]
    expected = sorted(digest(f"rss\0{i}".encode())[:32] for i in range(60))[:50]
    for ordered in (entries, list(reversed(entries))):
        assert [
            doc["document_id"] for doc in parse_feed(feed(ordered), "rss")[1]
        ] == expected
    for entry in entries:
        entry.pop("pubDate")
    assert [
        doc["document_id"] for doc in parse_feed(feed(entries), "rss")[1]
    ] == expected


async def test_fallback_identity_survives_duplicates_edits_and_restart(tmp_path, clock):
    initial = [
        {"description": "A"},
        {"description": "B"},
        {"title": "Unique", "description": "Editable"},
    ]
    store = Store(tmp_path)
    add_source(store)
    await Worker(store, FeedNetwork((200, {}, feed(initial))), None).fetch(
        store.source("rss")
    )

    def by_text():
        return {
            stored_body(store, item["snapshot_id"])["blocks"][0]["text"]: item[
                "document_id"
            ]
            for item in store.catalog("rss")
        }

    before = by_text()
    assert set(before) == {"A", "B", "Editable"}
    assert len(set(before.values())) == 3
    store.db.close()
    store = Store(tmp_path)
    changed = [
        {"description": "New same-title article"},
        {"description": "B", "pubDate": "2026-10-24T00:00:00Z"},
        {"title": "Renamed", "description": "A"},
        {"title": "Unique", "description": "Edited body"},
    ]
    await Worker(store, FeedNetwork((200, {}, feed(changed))), None).fetch(
        store.source("rss")
    )
    after = by_text()
    assert after["A"] == before["A"]
    assert after["B"] == before["B"]
    assert after["Edited body"] == before["Editable"]
    assert after["New same-title article"] not in before.values()
    assert len(after) == 4
    store.db.close()


async def test_new_same_body_entry_does_not_steal_older_title_identity(tmp_path, clock):
    store = Store(tmp_path)
    add_source(store)
    network = FeedNetwork(
        (200, {}, feed([{"title": "Original"}])),
        (200, {}, feed([{"title": "New"}, {"title": "Original"}])),
    )
    worker = Worker(store, network, None)
    await worker.fetch(store.source("rss"))
    previous = store.catalog("rss")[0]["document_id"]
    await worker.fetch(store.source("rss"))
    items = store.catalog("rss")
    assert len(items) == 2
    assert (
        next(item for item in items if item["title"] == "Original")["document_id"]
        == previous
    )
    store.db.close()


async def test_304_preserves_degraded_ids_beyond_the_catalog_quota(tmp_path, clock):
    store = Store(tmp_path)
    add_source(store)
    entries = [{"title": f"Title {i}", "description": str(i)} for i in range(60)]
    network = FeedNetwork(
        (200, {"ETag": "stable"}, feed(entries)),
        (304, {}, b""),
        (200, {}, feed(list(reversed(entries)))),
    )
    worker = Worker(store, network, None)
    await worker.fetch(store.source("rss"))
    before = store.catalog("rss")
    clock[0] += 8 * 86400
    await worker.fetch(store.source("rss"))
    assert store.db.execute("SELECT COUNT(*) FROM feed_fallbacks").fetchone()[0] == 60
    await worker.fetch(store.source("rss"))
    assert store.catalog("rss") == before
    store.db.close()


@pytest.mark.parametrize(
    ("next_date", "same_issue"),
    [
        ("2026-10-25T00:00:00Z", False),
        ("2026-10-24T00:00:00Z", True),
        ("2026-10-24T08:00:00+08:00", True),
    ],
)
async def test_rolling_same_title_feed_preserves_issues_and_matches_edits(
    tmp_path, clock, next_date, same_issue
):
    store = Store(tmp_path)
    add_source(store)
    network = FeedNetwork(
        (
            200,
            {},
            feed(
                [
                    {
                        "title": "Daily briefing",
                        "pubDate": "2026-10-24T00:00:00Z",
                        "description": "First issue",
                    }
                ]
            ),
        ),
        (
            200,
            {},
            feed(
                [
                    {
                        "title": "Daily briefing",
                        "pubDate": next_date,
                        "description": "Second issue",
                    }
                ]
            ),
        ),
    )
    worker = Worker(store, network, None)
    await worker.fetch(store.source("rss"))
    first_id = store.catalog("rss")[0]["document_id"]
    clock[0] += 86400
    await worker.fetch(store.source("rss"))
    items = store.catalog("rss")
    if same_issue:
        assert len(items) == 1
        assert items[0]["document_id"] == first_id
        assert (
            stored_body(store, items[0]["snapshot_id"])["blocks"][0]["text"]
            == "Second issue"
        )
    else:
        assert len(items) == 2
        assert items[0]["document_id"] != first_id
        assert items[1]["document_id"] == first_id
        assert [
            stored_body(store, item["snapshot_id"])["blocks"][0]["text"]
            for item in items
        ] == ["Second issue", "First issue"]
    store.db.close()


async def test_feishu_directory_timestamp_repairs_immutable_legacy_snapshot(
    tmp_path, clock
):
    store = Store(tmp_path)
    add_source(store, "feishu", "feishu")
    store.put("feishu", {"app_id": "test", "secret": "test", "generation": 1})
    store.put("feishu_scan", {"run": "test", "finished": True, "next": clock[0] + 300})
    feishu = Feishu(store, FakeNetwork())
    old_doc = await feishu.fetch("docx123456")
    with store.db:
        old = store.save_snapshot("feishu", old_doc)
        store.db.execute(
            "UPDATE documents SET updated='2026-10-24T00:00:00+00:00' WHERE source_id='feishu'"
        )
        store.db.execute(
            "INSERT INTO feishu_items(id,title,url,seen_run) VALUES('docx123456','Title','','test')"
        )
    await Library(store, feishu).fetch(store.source("feishu"))
    new = store.catalog("feishu")[0]["snapshot_id"]
    assert new != old
    assert stored_body(store, new)["content_updated_at"] == "2026-10-24T00:00:00+00:00"
    assert stored_body(store, old)["content_updated_at"] is None
    assert (
        stored_body(store, old)["content_revision"]
        == stored_body(store, new)["content_revision"]
    )
    assert digest(canonical(stored_body(store, old))) == old
    with store.db:
        store.db.execute("UPDATE feishu_items SET due=0")
    await Library(store, feishu).fetch(store.source("feishu"))
    assert store.catalog("feishu")[0]["snapshot_id"] == new
    store.db.close()


async def test_legacy_rss_body_quota_refetches_then_restores_conditional_checks(
    tmp_path, clock
):
    store = Store(tmp_path)
    add_source(store)
    body = feed([{"guid": str(i), "description": str(i)} for i in range(50)])
    network = FeedNetwork(
        (200, {"ETag": "stable"}, body), (200, {"ETag": "stable"}, body), (304, {}, b"")
    )
    worker = Worker(store, network, None)
    await worker.fetch(store.source("rss"))
    with store.db:
        store.db.execute(
            "UPDATE documents SET snapshot_id=NULL WHERE id IN (SELECT id FROM documents LIMIT 30)"
        )
    await worker.fetch(store.source("rss"))
    assert len(store.catalog("rss")) == 50
    assert all(item["snapshot_id"] for item in store.catalog("rss"))
    assert network.headers[:2] == [{}, {}]
    await worker.fetch(store.source("rss"))
    assert network.headers[-1] == {"If-None-Match": "stable"}
    assert all(item["snapshot_id"] for item in store.catalog("rss"))
    store.db.close()
