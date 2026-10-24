"""Persistent breadth-first discovery and independently retried document fetches."""

import asyncio
import json
import re
import secrets
import time

from .dates import upstream_time
from .errors import Problem
from .store import digest


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", value):
        raise Problem("PARSE_ERROR", "飞书目录标识无效", 502)
    return value


class Library:
    def __init__(self, store, feishu):
        self.store, self.feishu = store, feishu

    def current(self, source):
        latest = self.store.source("feishu")
        return latest and latest["generation"] == source["generation"]

    def add(self, kind, target=""):
        self.store.db.execute(
            "INSERT OR IGNORE INTO feishu_frontier(id,kind,target) VALUES(?,?,?)",
            (digest((kind + ":" + target).encode()), kind, target),
        )

    async def page(self, task):
        kind, target = task["kind"], task["target"]
        params = {"page_size": 50}
        if task["cursor"]:
            params["page_token"] = task["cursor"]
        if kind == "drive":
            if target:
                params["folder_token"] = target
            path, key = "drive/v1/files", "files"
        elif kind == "spaces":
            path, key = "wiki/v2/spaces", "items"
        elif kind == "personal":
            data = await self.feishu.get("wiki/v2/spaces/my_library", api=True)
            space = data.get("space", {}).get("space_id")
            return [], [("nodes", identifier(space) + ":")], ""
        else:
            space, parent = target.split(":", 1)
            path, key = "wiki/v2/spaces/" + space + "/nodes", "items"
            if parent:
                params["parent_node_token"] = parent
        data = await self.feishu.get(path, params, api=True)
        items = data.get(key)
        if not isinstance(items, list):
            raise Problem("PARSE_ERROR", "飞书目录响应不完整", 502)
        docs, children = [], []
        for item in items:
            if not isinstance(item, dict):
                raise Problem("PARSE_ERROR", "飞书目录条目无效", 502)
            if kind == "spaces":
                children.append(("nodes", identifier(item.get("space_id")) + ":"))
                continue
            typ = item.get("type") if kind == "drive" else item.get("obj_type")
            token = item.get("token") if kind == "drive" else item.get("obj_token")
            if kind == "drive" and typ == "shortcut":
                shortcut = item.get("shortcut_info", {})
                typ, token = shortcut.get("target_type"), shortcut.get("target_token")
            if typ == "folder":
                children.append(("drive", identifier(token)))
            if kind == "nodes" and item.get("has_child"):
                children.append(
                    ("nodes", space + ":" + identifier(item.get("node_token")))
                )
            if typ == "docx":
                title = item.get("name") if kind == "drive" else item.get("title")
                if not isinstance(title, str):
                    raise Problem("PARSE_ERROR", "飞书文档标题无效", 502)
                url = item.get("url", "")
                docs.append(
                    (
                        identifier(token),
                        title[:256],
                        url if isinstance(url, str) else "",
                        upstream_time(
                            item.get("created_time")
                            if kind == "drive"
                            else item.get("obj_create_time")
                        ),
                        upstream_time(
                            item.get("modified_time")
                            if kind == "drive"
                            else item.get("obj_edit_time")
                        ),
                    )
                )
        cursor = ""
        if data.get("has_more"):
            cursor = data.get("next_page_token") or data.get("page_token")
            visited = json.loads(task["visited"])
            if (
                not isinstance(cursor, str)
                or not cursor
                or len(cursor) > 2048
                or cursor in visited
            ):
                raise Problem("PARSE_ERROR", "飞书目录分页标识无效", 502)
        return docs, children, cursor

    async def fetch(self, source):
        store = self.store
        scan = store.get("feishu_scan")
        now = time.time()
        if not scan or (scan["finished"] and scan["next"] <= now):
            scan = {
                "run": secrets.token_hex(12),
                "finished": False,
                "next": now + source["interval"],
            }
            with store.db:
                store.db.execute("DELETE FROM feishu_frontier")
                self.add("drive")
                self.add("spaces")
                if store.get("feishu_user"):
                    self.add("personal")
            store.put("feishu_scan", scan)
        # Commit each page before yielding; restart never loses the discovery queue.
        for _ in range(1):
            tasks = store.rows(
                "SELECT * FROM feishu_frontier WHERE done=0 ORDER BY rowid LIMIT 1"
            )
            if not tasks:
                break
            task = tasks[0]
            try:
                async with asyncio.timeout(20):
                    docs, children, cursor = await self.page(task)
            except (Problem, TimeoutError) as error:
                if not self.current(source):
                    return
                code = error.code if isinstance(error, Problem) else "UPSTREAM_TIMEOUT"
                if code in (
                    "USER_AUTH_REQUIRED",
                    "UPSTREAM_AUTH_REQUIRED",
                    "APP_CREDENTIALS_INVALID",
                    "RATE_LIMITED",
                ):
                    raise
                with store.db:
                    store.db.execute(
                        "UPDATE feishu_frontier SET done=1,error=? WHERE id=?",
                        (code, task["id"]),
                    )
                continue
            if not self.current(source):
                return
            with store.db:
                for did, title, url, created, modified in docs:
                    store.db.execute(
                        "INSERT INTO feishu_items(id,title,url,seen_run) VALUES(?,?,?,?) "
                        "ON CONFLICT(id) DO UPDATE SET title=excluded.title,url=excluded.url,seen_run=excluded.seen_run",
                        (did, title, url, scan["run"]),
                    )
                    store.db.execute(
                        "INSERT INTO documents(source_id,id,title,url,seen,created,updated) VALUES('feishu',?,?,?,?,?,?) "
                        "ON CONFLICT(source_id,id) DO UPDATE SET title=excluded.title,url=excluded.url,"
                        "created=COALESCE(excluded.created,documents.created),updated=COALESCE(excluded.updated,documents.updated)",
                        (did, title, url, time.time(), created, modified),
                    )
                for kind, target in children:
                    self.add(kind, target)
                visited = json.loads(task["visited"])
                if cursor:
                    visited.append(cursor)
                store.db.execute(
                    "UPDATE feishu_frontier SET cursor=?,visited=?,done=? WHERE id=?",
                    (cursor, json.dumps(visited), int(not cursor), task["id"]),
                )
        if not self.current(source):
            return
        pending = bool(
            store.db.execute(
                "SELECT 1 FROM feishu_frontier WHERE done=0 LIMIT 1"
            ).fetchone()
        )
        partial = bool(
            store.db.execute(
                "SELECT 1 FROM feishu_frontier WHERE error IS NOT NULL LIMIT 1"
            ).fetchone()
        )
        if not pending and not scan["finished"]:
            # An incomplete or forbidden subtree cannot prove a document was removed.
            if not partial:
                with store.db:
                    store.db.execute(
                        "DELETE FROM documents WHERE source_id='feishu' AND id NOT IN (SELECT id FROM feishu_items WHERE seen_run=?)",
                        (scan["run"],),
                    )
                    store.db.execute(
                        "DELETE FROM feishu_items WHERE seen_run<>?", (scan["run"],)
                    )
            scan.update(finished=True, next=time.time() + source["interval"])
            store.put("feishu_scan", scan)
            if not partial:
                with store.db:
                    store.db.execute(
                        "UPDATE sources SET fetched=? WHERE id='feishu'", (time.time(),)
                    )
        for item in store.rows(
            "SELECT * FROM feishu_items WHERE due<=? ORDER BY due,id LIMIT 1",
            (time.time(),),
        ):
            try:
                async with asyncio.timeout(25):
                    doc = await self.feishu.fetch(item["id"], item["url"])
            except (Problem, TimeoutError) as error:
                if not self.current(source):
                    return
                code = error.code if isinstance(error, Problem) else "UPSTREAM_TIMEOUT"
                if code in (
                    "USER_AUTH_REQUIRED",
                    "UPSTREAM_AUTH_REQUIRED",
                    "APP_CREDENTIALS_INVALID",
                    "RATE_LIMITED",
                ):
                    raise
                failures = item["failures"] + 1
                with store.db:
                    store.db.execute(
                        "UPDATE feishu_items SET error=?,failures=?,due=? WHERE id=?",
                        (
                            code,
                            failures,
                            time.time()
                            + min(
                                21600, source["interval"] * 2 ** min(failures - 1, 6)
                            ),
                            item["id"],
                        ),
                    )
                continue
            if not self.current(source):
                return
            with store.db:
                store.save_snapshot("feishu", doc)
                store.db.execute(
                    "UPDATE feishu_items SET due=?,error=NULL,failures=0 WHERE id=?",
                    (time.time() + source["interval"], item["id"]),
                )
                store.db.execute(
                    "UPDATE sources SET fetched=? WHERE id='feishu'", (time.time(),)
                )
        next_item = store.db.execute("SELECT MIN(due) FROM feishu_items").fetchone()[0]
        due = (
            time.time() + 1
            if pending
            else min(scan["next"], next_item if next_item is not None else scan["next"])
        )
        failed = store.feishu_progress()["failed"]
        with store.db:
            store.db.execute(
                "UPDATE sources SET title='飞书文档',checked=?,due=?,error=?,failures=0 WHERE id='feishu' AND generation=?",
                (
                    time.time(),
                    max(time.time() + 1, due),
                    "SOURCE_PARTIAL" if partial or failed else None,
                    source["generation"],
                ),
            )
