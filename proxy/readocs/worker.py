import asyncio
import random
import time

from .content import FEED_MAX_BYTES, FeedIdentities, parse_feed
from .errors import Problem
from .library import Library


class Worker:
    def __init__(self, store, network, feishu):
        self.store, self.network, self.feishu = store, network, feishu
        self.stopping = False
        self.library = Library(store, feishu)

    async def fetch(self, source):
        sid = source["id"]
        config = self.store.decrypt(source["config"])
        headers, status, identities = {}, 200, None
        if source["kind"] == "feishu":
            await self.library.fetch(source)
            return
        else:
            request_headers = {}
            # Older releases kept 50 directory entries but only 20 bodies.
            # A 304 cannot rebuild those missing snapshots. Refetch current
            # members once; missing historical entries must not disable
            # conditional checks forever after rolling out of the feed.
            missing_current_bodies = (
                self.store.db.execute(
                    "SELECT 1 FROM documents d JOIN feed_members m "
                    "ON m.source_id=d.source_id AND m.document_id=d.id "
                    "WHERE d.source_id=? AND d.snapshot_id IS NULL LIMIT 1",
                    (sid,),
                ).fetchone()
                is not None
            )
            if (
                source["feed_members_known"]
                and source["upstream_etag"]
                and not missing_current_bodies
            ):
                request_headers["If-None-Match"] = source["upstream_etag"]
            if (
                source["feed_members_known"]
                and source["upstream_modified"]
                and not missing_current_bodies
            ):
                request_headers["If-Modified-Since"] = source["upstream_modified"]
            status, headers, body = await self.network.request(
                "GET", config["url"], headers=request_headers, max_bytes=FEED_MAX_BYTES
            )
            if status == 304:
                if not source["feed_members_known"]:
                    raise Problem("PARSE_ERROR", "订阅源缺少可复用的完整检查结果", 502)
                title, docs = source["title"], []
            elif status == 429:
                try:
                    delay = int(headers.get("Retry-After", "60"))
                except ValueError:
                    delay = 60
                raise Problem(
                    "RATE_LIMITED",
                    "订阅源暂时限流，请稍后重试",
                    429,
                    max(60, min(86400, delay)),
                )
            elif status in (404, 410):
                raise Problem("SOURCE_GONE", "订阅源不可用，旧内容已保留", 404)
            elif status != 200:
                raise Problem("UPSTREAM_TIMEOUT", "订阅源暂时不可用", 502)
            else:
                identities = FeedIdentities(
                    self.store.rows(
                        "SELECT document_id,title,fingerprint,updated FROM feed_fallbacks WHERE source_id=?",
                        (sid,),
                    )
                )
                title, docs = parse_feed(body, sid, identities=identities)
        # A config edit or credential rotation invalidates an in-flight result.
        latest = self.store.source(sid)
        if not latest or latest["generation"] != source["generation"]:
            return
        with self.store.db:
            if status == 304:
                self.store.db.execute(
                    "UPDATE documents SET seen=? WHERE source_id=? AND id IN "
                    "(SELECT document_id FROM feed_members WHERE source_id=?)",
                    (time.time(), sid, sid),
                )
                self.store.db.execute(
                    "UPDATE feed_fallbacks SET seen=? WHERE source_id=? AND document_id IN "
                    "(SELECT document_id FROM feed_members WHERE source_id=?)",
                    (time.time(), sid, sid),
                )
            else:
                self.store.db.execute(
                    "DELETE FROM feed_members WHERE source_id=?", (sid,)
                )
                self.store.db.executemany(
                    "INSERT INTO feed_members VALUES(?,?)",
                    [(sid, did) for did in identities.members],
                )
                self.store.db.executemany(
                    "INSERT INTO feed_fallbacks VALUES(?,?,?,?,?,?) "
                    "ON CONFLICT(source_id,document_id) DO UPDATE SET title=excluded.title,"
                    "fingerprint=excluded.fingerprint,updated=excluded.updated,seen=excluded.seen",
                    [
                        (
                            sid,
                            row["document_id"],
                            row["title"],
                            row["fingerprint"],
                            row["updated"],
                            time.time(),
                        )
                        for row in identities.records
                    ],
                )
            for doc in docs:
                self.store.save_snapshot(sid, doc)
            self.store.db.execute(
                "UPDATE sources SET title=?,checked=?,fetched=?,due=?,error=NULL,failures=0,"
                "upstream_etag=?,upstream_modified=?,feed_members_known=1 WHERE id=?",
                (
                    title,
                    time.time(),
                    time.time(),
                    time.time() + source["interval"],
                    headers.get("Etag")
                    or headers.get("ETag")
                    or source["upstream_etag"],
                    headers.get("Last-Modified") or source["upstream_modified"],
                    sid,
                ),
            )
        if source["kind"] == "rss":
            self.store.prune(sid)

    async def execute(self, job):
        source = self.store.source(job["source_id"])
        if not source:
            with self.store.db:
                self.store.db.execute("DELETE FROM jobs WHERE id=?", (job["id"],))
            return
        with self.store.db:
            self.store.db.execute(
                "UPDATE jobs SET state='running',attempts=attempts+1 WHERE id=?",
                (job["id"],),
            )
        try:
            async with asyncio.timeout(60):
                await self.fetch(source)
        except (Problem, TimeoutError) as e:
            code = e.code if isinstance(e, Problem) else "UPSTREAM_TIMEOUT"
            failures = source["failures"] + 1
            retry = min(
                21600, source["interval"] * 2 ** min(failures - 1, 6)
            ) + random.uniform(0, 10)
            retry = max(retry, e.retry_after if isinstance(e, Problem) else 0)
            with self.store.db:
                self.store.db.execute(
                    "UPDATE sources SET error=?,checked=?,due=?,failures=? WHERE id=? AND generation=?",
                    (
                        code,
                        time.time(),
                        time.time() + retry,
                        failures,
                        source["id"],
                        source["generation"],
                    ),
                )
        except Exception:  # noqa: BLE001 — isolate malformed upstream content without logging secrets
            # Never persist upstream payloads or exception strings which can contain secrets.
            with self.store.db:
                self.store.db.execute(
                    "UPDATE sources SET error='INTERNAL_ERROR',due=? WHERE id=? AND generation=?",
                    (
                        time.time() + source["interval"],
                        source["id"],
                        source["generation"],
                    ),
                )
        finally:
            with self.store.db:
                self.store.db.execute("DELETE FROM jobs WHERE id=?", (job["id"],))

    async def run(self):
        while not self.stopping:
            now = time.time()
            for source in self.store.rows(
                "SELECT id FROM sources WHERE due<=?", (now,)
            ):
                self.store.enqueue(source["id"])
            jobs = self.store.rows(
                "SELECT * FROM jobs WHERE state='queued' AND due<=? ORDER BY created LIMIT 2",
                (now,),
            )
            if jobs:
                await asyncio.gather(*(self.execute(j) for j in jobs))
            else:
                await asyncio.sleep(1)
