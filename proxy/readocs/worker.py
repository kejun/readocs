import asyncio
import random
import time

from .content import parse_feed
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
        headers, status = {}, 200
        if source["kind"] == "feishu":
            await self.library.fetch(source)
            return
        else:
            request_headers = {}
            if source["upstream_etag"]:
                request_headers["If-None-Match"] = source["upstream_etag"]
            if source["upstream_modified"]:
                request_headers["If-Modified-Since"] = source["upstream_modified"]
            status, headers, body = await self.network.request(
                "GET", config["url"], headers=request_headers
            )
            if status == 304:
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
                title, docs = parse_feed(body, sid)
        # A config edit or credential rotation invalidates an in-flight result.
        latest = self.store.source(sid)
        if not latest or latest["generation"] != source["generation"]:
            return
        with self.store.db:
            for doc in reversed(docs):
                self.store.save_snapshot(sid, doc)
            self.store.db.execute(
                "UPDATE sources SET title=?,checked=?,fetched=?,due=?,error=NULL,failures=0,"
                "upstream_etag=?,upstream_modified=? WHERE id=?",
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
