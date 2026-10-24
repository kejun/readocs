import hashlib
import json
import os
import secrets
import sqlite3
import time
from pathlib import Path

from cryptography.fernet import Fernet

from .dates import article_order


def canonical(value) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode()


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def private_file(path: Path, value: bytes) -> bytes:
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        return path.read_bytes()
    with os.fdopen(fd, "wb") as f:
        f.write(value)
        f.flush()
        os.fsync(f.fileno())
    return value


class Store:
    def __init__(self, directory: Path):
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.cipher = Fernet(
            private_file(directory / "vault.key", Fernet.generate_key())
        )
        self.admin_key = private_file(
            directory / "admin.key", secrets.token_urlsafe(32).encode()
        ).decode()
        self.db = sqlite3.connect(
            directory / "readocs.sqlite3", check_same_thread=False
        )
        os.chmod(directory / "readocs.sqlite3", 0o600)
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
        PRAGMA journal_mode=WAL;
        PRAGMA foreign_keys=ON;
        CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS sources(
          id TEXT PRIMARY KEY, kind TEXT NOT NULL, title TEXT NOT NULL, config TEXT NOT NULL,
          interval INTEGER NOT NULL, generation INTEGER NOT NULL DEFAULT 1,
          checked REAL, fetched REAL, due REAL NOT NULL DEFAULT 0, error TEXT,
          failures INTEGER NOT NULL DEFAULT 0, upstream_etag TEXT, upstream_modified TEXT);
        CREATE TABLE IF NOT EXISTS snapshots(
          id TEXT PRIMARY KEY, source_id TEXT NOT NULL, document_id TEXT NOT NULL,
          body BLOB NOT NULL, created REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS documents(
          source_id TEXT NOT NULL, id TEXT NOT NULL, title TEXT NOT NULL,
          url TEXT NOT NULL, snapshot_id TEXT, updated TEXT, seen REAL NOT NULL,
          summary INTEGER NOT NULL DEFAULT 0, created TEXT, PRIMARY KEY(source_id,id));
        CREATE TABLE IF NOT EXISTS devices(
          id TEXT PRIMARY KEY, name TEXT NOT NULL, token_hash TEXT NOT NULL UNIQUE,
          created REAL NOT NULL, last_seen REAL, revoked INTEGER NOT NULL DEFAULT 0,
          refreshed REAL NOT NULL DEFAULT 0);
        CREATE TABLE IF NOT EXISTS pairings(code_hash TEXT PRIMARY KEY, expires REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS sessions(token_hash TEXT PRIMARY KEY, expires REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS jobs(
          id TEXT PRIMARY KEY, source_id TEXT NOT NULL UNIQUE, state TEXT NOT NULL,
          created REAL NOT NULL, due REAL NOT NULL, attempts INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE IF NOT EXISTS feishu_items(
          id TEXT PRIMARY KEY, title TEXT NOT NULL, url TEXT NOT NULL,
          seen_run TEXT NOT NULL, due REAL NOT NULL DEFAULT 0,
          error TEXT, failures INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE IF NOT EXISTS feishu_frontier(
          id TEXT PRIMARY KEY, kind TEXT NOT NULL, target TEXT NOT NULL,
          cursor TEXT NOT NULL DEFAULT '', visited TEXT NOT NULL DEFAULT '[]',
          done INTEGER NOT NULL DEFAULT 0, error TEXT);
        CREATE TABLE IF NOT EXISTS oauth_states(
          hash TEXT PRIMARY KEY, session_hash TEXT NOT NULL, generation INTEGER NOT NULL,
          redirect TEXT NOT NULL, expires REAL NOT NULL);
        UPDATE jobs SET state='queued' WHERE state='running';
        """)
        if "created" not in {
            row["name"] for row in self.db.execute("PRAGMA table_info(documents)")
        }:
            self.db.execute("ALTER TABLE documents ADD COLUMN created TEXT")
        self.db.commit()

    def encrypt(self, value):
        return self.cipher.encrypt(canonical(value)).decode()

    def decrypt(self, value):
        return json.loads(self.cipher.decrypt(value.encode()))

    def get(self, key, default=None):
        row = self.db.execute(
            "SELECT value FROM settings WHERE key=?", (key,)
        ).fetchone()
        return self.decrypt(row[0]) if row else default

    def put(self, key, value):
        with self.db:
            self.db.execute(
                "INSERT OR REPLACE INTO settings VALUES(?,?)",
                (key, self.encrypt(value)),
            )

    def rows(self, sql, args=()):
        return [dict(r) for r in self.db.execute(sql, args)]

    def source(self, sid):
        rows = self.rows("SELECT * FROM sources WHERE id=?", (sid,))
        return rows[0] if rows else None

    def save_snapshot(self, sid: str, doc: dict):
        body = canonical(doc)
        snapshot_id = digest(body)
        existing = self.db.execute(
            "SELECT p.id,p.body FROM documents d JOIN snapshots p ON p.id=d.snapshot_id "
            "WHERE d.source_id=? AND d.id=?",
            (sid, doc["document_id"]),
        ).fetchone()
        if (
            existing
            and json.loads(existing["body"])["content_revision"]
            == doc["content_revision"]
        ):
            self.db.execute(
                "UPDATE documents SET seen=?,updated=COALESCE(?,updated) WHERE source_id=? AND id=?",
                (time.time(), doc.get("content_updated_at"), sid, doc["document_id"]),
            )
            return existing["id"]
        self.db.execute(
            "INSERT OR IGNORE INTO snapshots VALUES(?,?,?,?,?)",
            (snapshot_id, sid, doc["document_id"], body, time.time()),
        )
        self.db.execute(
            "INSERT INTO documents(source_id,id,title,url,snapshot_id,updated,seen,summary) VALUES(?,?,?,?,?,?,?,?) "
            "ON CONFLICT(source_id,id) DO UPDATE SET title=excluded.title,url=excluded.url,"
            "snapshot_id=excluded.snapshot_id,updated=COALESCE(excluded.updated,documents.updated),"
            "seen=excluded.seen,summary=excluded.summary",
            (
                sid,
                doc["document_id"],
                doc["title"],
                doc.get("url", ""),
                snapshot_id,
                doc.get("content_updated_at"),
                time.time(),
                int(doc.get("summary", False)),
            ),
        )
        return snapshot_id

    def catalog(self, sid):
        source = self.source(sid)
        collection = source and source["kind"] == "feishu"
        items = self.rows(
            "SELECT d.id AS document_id,d.title,d.url,d.snapshot_id,d.updated AS content_updated_at,"
            "d.created AS content_created_at,d.summary,i.error AS sync_error "
            "FROM documents d LEFT JOIN feishu_items i ON d.source_id='feishu' AND i.id=d.id WHERE d.source_id=?",
            (sid,),
        )
        items.sort(key=lambda item: article_order(item, feishu=collection))
        return items if collection else items[:50]

    def reference(self, snapshot_id):
        row = self.db.execute(
            "SELECT body FROM snapshots WHERE id=?", (snapshot_id,)
        ).fetchone()
        if not row:
            return None
        body = row[0]
        return {
            "snapshot_id": snapshot_id,
            "bytes": len(body),
            "sha256": digest(body),
            "chunks": [
                {
                    "index": i // 32768,
                    "bytes": len(body[i : i + 32768]),
                    "sha256": digest(body[i : i + 32768]),
                }
                for i in range(0, len(body), 32768)
            ],
        }

    @staticmethod
    def catalog_revision(items):
        # A body arriving or failing must not invalidate directory pagination.
        return digest(
            canonical(
                [
                    {
                        key: value
                        for key, value in item.items()
                        if key not in ("snapshot_id", "sync_error")
                    }
                    for item in items
                ]
            )
        )

    def manifest(self):
        sources = []
        for s in self.rows("SELECT id,kind,title FROM sources ORDER BY kind,id"):
            catalog = self.catalog(s["id"])
            entry = {
                **s,
                "catalog_revision": self.catalog_revision(catalog),
                "body_revision": digest(
                    canonical(
                        [(item["document_id"], item["snapshot_id"]) for item in catalog]
                    )
                ),
                "count": len(catalog),
            }
            entry["collection"] = True
            sources.append(entry)
        return {
            "schema_version": 1,
            "config_revision": digest(canonical(sources)),
            "sources": sources,
        }

    def reset_feishu_library(self, clear=False):
        self.db.execute("DELETE FROM feishu_frontier")
        self.db.execute("DELETE FROM settings WHERE key='feishu_scan'")
        if clear:
            self.db.execute("DELETE FROM feishu_items")
            self.db.execute("DELETE FROM documents WHERE source_id='feishu'")
            self.db.execute("DELETE FROM snapshots WHERE source_id='feishu'")

    def feishu_progress(self):
        return {
            "discovered": self.db.execute(
                "SELECT COUNT(*) FROM feishu_items"
            ).fetchone()[0],
            "synced": self.db.execute(
                "SELECT COUNT(*) FROM documents WHERE source_id='feishu' AND snapshot_id IS NOT NULL"
            ).fetchone()[0],
            "failed": self.db.execute(
                "SELECT COUNT(*) FROM feishu_items WHERE error IS NOT NULL"
            ).fetchone()[0],
            "scanning": bool(
                self.db.execute(
                    "SELECT 1 FROM feishu_frontier WHERE done=0 LIMIT 1"
                ).fetchone()
            ),
        }

    def enqueue(self, sid, refresh=False):
        if refresh and sid == "feishu":
            scan = self.get("feishu_scan")
            if scan and scan["finished"]:
                self.put("feishu_scan", {**scan, "next": 0})
            with self.db:
                self.db.execute("UPDATE feishu_items SET due=0")
        row = self.db.execute(
            "SELECT id FROM jobs WHERE source_id=?", (sid,)
        ).fetchone()
        if row:
            return row[0]
        jid = secrets.token_hex(12)
        with self.db:
            self.db.execute(
                "INSERT INTO jobs VALUES(?,?, 'queued',?,?,0)",
                (jid, sid, time.time(), time.time()),
            )
        return jid

    def prune(self, sid):
        # Directory and body quotas are independent. Disappearance from a feed is not deletion.
        rows = self.rows(
            "SELECT id,seen FROM documents WHERE source_id=? ORDER BY seen DESC,id",
            (sid,),
        )
        now = time.time()
        with self.db:
            for i, row in enumerate(rows):
                if i >= 50 or now - row["seen"] > 7 * 86400:
                    self.db.execute(
                        "DELETE FROM documents WHERE source_id=? AND id=?",
                        (sid, row["id"]),
                    )
                elif i >= 20:
                    self.db.execute(
                        "UPDATE documents SET snapshot_id=NULL WHERE source_id=? AND id=?",
                        (sid, row["id"]),
                    )
            # Immutable snapshots have a minimum 24-hour download lease even after replacement.
            self.db.execute(
                "DELETE FROM snapshots WHERE created<? AND id NOT IN "
                "(SELECT snapshot_id FROM documents WHERE snapshot_id IS NOT NULL)",
                (now - 86400,),
            )
