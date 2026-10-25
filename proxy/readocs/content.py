import hashlib
import re
import secrets
from collections import Counter, defaultdict, deque
from html.parser import HTMLParser
from urllib.parse import urlsplit, urlunsplit

from defusedxml import ElementTree as ET

from .dates import article_order, timestamp
from .errors import Problem
from .store import canonical, digest

ATOM = "{http://www.w3.org/2005/Atom}"
CONTENT = "{http://purl.org/rss/1.0/modules/content/}"
XHTML = "{http://www.w3.org/1999/xhtml}"
FEED_MAX_BYTES = 16 * 1024 * 1024
FEED_ARTICLE_MAX_BYTES = 64 * 1024
FEED_TRUNCATION_NOTE = "正文较长，以下为部分内容；完整内容请查看原文。"


class TextHTML(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts, self.hidden = [], 0

    def handle_starttag(self, tag, attrs):
        tag = tag.rsplit(":", 1)[-1]
        if tag in ("script", "style", "iframe", "object"):
            self.hidden += 1
        if not self.hidden:
            if tag in (
                "p",
                "div",
                "br",
                "li",
                "h1",
                "h2",
                "h3",
                "blockquote",
                "pre",
                "tr",
            ):
                self.parts.append("\n")
            if tag == "li":
                self.parts.append("• ")
            if tag == "img":
                self.parts.append("[图片：" + dict(attrs).get("alt", "图片") + "]")

    def handle_endtag(self, tag):
        tag = tag.rsplit(":", 1)[-1]
        if tag in ("script", "style", "iframe", "object"):
            self.hidden = max(0, self.hidden - 1)
        if tag in ("p", "div", "li", "pre", "blockquote", "tr") and not self.hidden:
            self.parts.append("\n")

    def handle_data(self, data):
        if not self.hidden:
            self.parts.append(data)


def clean_html(value):
    parser = TextHTML()
    parser.feed(value)
    return re.sub(r"\n[ \t]*\n+", "\n\n", "".join(parser.parts)).strip()


def blocks_from_text(text):
    blocks = []
    counts = {}
    for paragraph in re.split(r"\n\s*\n", text):
        if not paragraph.strip():
            continue
        key = digest(paragraph.strip().encode())[:24]
        counts[key] = counts.get(key, 0) + 1
        blocks.append(
            {
                "block_id": f"p-{key}-{counts[key]}",
                "type": "paragraph",
                "text": paragraph.strip(),
                "depth": 0,
                "meta": {},
            }
        )
    return blocks


def document(
    doc_id, title, blocks, *, url="", updated=None, summary=False, truncated=False
):
    selected, size = [], 0
    for block in blocks:
        if len(block.get("text", "").encode()) > 65536:
            raise Problem("CONTENT_TOO_LARGE", "单段正文超过 64 KiB", 422)
        block = {**block, "hash": digest(canonical(block))}
        size += len(canonical(block))
        if len(selected) >= 500 or size > 290 * 1024:
            truncated = True
            break
        selected.append(block)
    visible = {
        "title": title,
        "blocks": selected,
        "truncated": truncated,
        "summary": summary,
        "url": url,
    }
    return {
        "schema_version": 1,
        "document_id": doc_id,
        "content_revision": digest(canonical(visible)),
        "content_updated_at": updated,
        **visible,
    }


def normalize_url(url):
    p = urlsplit(url)
    host = (p.hostname or "").lower()
    if ":" in host:
        host = f"[{host}]"
    if p.port and not (
        p.scheme.lower() == "http"
        and p.port == 80
        or p.scheme.lower() == "https"
        and p.port == 443
    ):
        host += f":{p.port}"
    return urlunsplit((p.scheme.lower(), host, p.path, p.query, ""))


class FeedIdentities:
    """Match degraded entries without treating a shared title as a unique ID."""

    def __init__(self, previous=()):
        self.previous = list(previous)
        self.records = []
        self.members = []

    def resolve(self, entries):
        remaining = {row["document_id"]: row for row in self.previous}
        assigned = {}
        buckets = defaultdict(deque)
        titles = defaultdict(deque)
        title_counts = Counter(row["title"] for row in self.previous)
        incoming_counts = Counter(entry["title"] for entry in entries)
        for row in sorted(self.previous, key=lambda row: row["document_id"]):
            did, fingerprint, title, updated = (
                row["document_id"],
                row["fingerprint"],
                row["title"],
                row["updated"],
            )
            for key in (
                (fingerprint, title, updated),
                (fingerprint, title),
                (fingerprint, None, updated),
                (fingerprint,),
            ):
                buckets[key].append(did)
            titles[title].append(did)

        def available(bucket):
            while bucket and bucket[0] not in remaining:
                bucket.popleft()
            return bucket[0] if bucket else None

        def assign(index, did):
            assigned[index] = did
            title_counts[remaining.pop(did)["title"]] -= 1
            incoming_counts[entries[index]["title"]] -= 1

        # Match unchanged bodies first across the whole feed, so a newly inserted
        # same-title entry cannot steal an older article's ID before it is visited.
        for specificity in range(4):
            for index, entry in enumerate(entries):
                if index in assigned:
                    continue
                fingerprint, title, updated = (
                    entry["fingerprint"],
                    entry["title"],
                    entry["updated"],
                )
                key = (
                    (fingerprint, title, updated),
                    (fingerprint, title),
                    (fingerprint, None, updated),
                    (fingerprint,),
                )[specificity]
                did = available(buckets[key])
                if did:
                    assign(index, did)
        # A unique surviving title can identify an edited body, unless two known
        # publication times show that this is a later issue of a recurring title.
        for index, entry in enumerate(entries):
            if index in assigned:
                continue
            title = entry["title"]
            if title_counts[title] == incoming_counts[title] == 1:
                did = available(titles[title])
                previous_date = timestamp(remaining[did]["updated"])
                current_date = timestamp(entry["updated"])
                if (
                    previous_date is not None
                    and current_date is not None
                    and previous_date != current_date
                ):
                    continue
                assign(index, did)
        self.records = [
            {**entry, "document_id": assigned.get(i) or secrets.token_hex(16)}
            for i, entry in enumerate(entries)
        ]
        return [row["document_id"] for row in self.records]


def parse_feed(body: bytes, sid: str, *, identities=None):
    if len(body) > FEED_MAX_BYTES:
        raise Problem("CONTENT_TOO_LARGE", "订阅源超过 16 MiB 限制", 422)
    if re.search(rb"<!\s*(DOCTYPE|ENTITY)", body, re.IGNORECASE):
        raise Problem("PARSE_ERROR", "不支持含 DTD 或实体声明的源", 422)
    declared = re.search(
        rb"<\?xml[^>]*encoding=[\"'](gb18030|gbk|gb2312)[\"']",
        body[:200],
        re.IGNORECASE,
    )
    if declared:
        try:
            decoded = body.decode("gb18030", errors="strict")
            decoded = re.sub(
                r"(encoding=[\"'])(?:gb18030|gbk|gb2312)([\"'])",
                r"\1utf-8\2",
                decoded,
                count=1,
                flags=re.IGNORECASE,
            )
            body = decoded.encode("utf-8")
        except UnicodeError:
            raise Problem("PARSE_ERROR", "源声明的中文编码与正文不匹配", 422) from None
        if len(body) > FEED_MAX_BYTES:
            raise Problem("CONTENT_TOO_LARGE", "解码后的订阅源超过 16 MiB 限制", 422)
    try:
        root = ET.fromstring(body)
        count = 0
        stack = [(root, 0)]
        while stack:
            elem, depth = stack.pop()
            count += 1
            if depth > 32 or count > 30000:
                raise Problem("CONTENT_TOO_LARGE", "源结构超过处理限制", 422)
            stack.extend((c, depth + 1) for c in elem)
    except (ET.ParseError, ValueError, LookupError):
        raise Problem("PARSE_ERROR", "无法解析源内容，请检查编码与格式", 422) from None
    if root.tag == ATOM + "feed":
        title = root.findtext(ATOM + "title", "未命名订阅")
        entries = root.findall(ATOM + "entry")
        atom = True
    elif root.tag == "rss" and root.get("version") == "2.0":
        title = root.findtext("channel/title", "未命名订阅")
        entries = root.findall("channel/item")
        atom = False
    else:
        raise Problem("SOURCE_UNSUPPORTED", "首版支持 RSS 2.0 与 Atom 1.0", 422)
    docs, seen, parsed, degraded = [], set(), [], []
    for e in entries:
        if atom:
            title_e = e.findtext(ATOM + "title", "未命名文章")
            link = next(
                (
                    c.get("href", "")
                    for c in e.findall(ATOM + "link")
                    if c.get("rel", "alternate") == "alternate"
                ),
                "",
            )
            identity = e.findtext(ATOM + "id")
            updated = e.findtext(ATOM + "updated") or e.findtext(ATOM + "published")
            content = e.find(ATOM + "content")
            summary = content is None
            if content is None:
                content = e.find(ATOM + "summary")
            text = ""
            if content is not None:
                typ = content.get("type", "text")
                if typ == "xhtml":
                    div = content.find(XHTML + "div")
                    text = (
                        clean_html(ET.tostring(div, encoding="unicode"))
                        if div is not None
                        else ""
                    )
                elif typ in ("html", "text/html"):
                    text = clean_html(content.text or "")
                else:
                    text = content.text or ""
        else:
            title_e, link, identity = (
                e.findtext("title", "未命名文章"),
                e.findtext("link", ""),
                e.findtext("guid"),
            )
            updated = e.findtext("pubDate")
            text = e.findtext(CONTENT + "encoded")
            summary = text is None
            text = clean_html(
                text if text is not None else e.findtext("description", "")
            )
        blocks = blocks_from_text(text or "[此条目没有正文]")
        if not identity and link:
            identity = normalize_url(link)
        if identity:
            did = hashlib.sha256((sid + "\0" + identity).encode()).hexdigest()[:32]
            if did in seen:
                continue
            seen.add(did)
        else:
            did = None
            degraded.append(
                {
                    "title": title_e[:256],
                    "fingerprint": digest(canonical([b["text"] for b in blocks])),
                    "updated": updated,
                }
            )
        if urlsplit(link).scheme not in ("http", "https"):
            link = ""
        blocks, truncated = feed_article_blocks(blocks)
        parsed.append((did, title_e[:256], blocks, link, updated, summary, truncated))
    identities = identities or FeedIdentities()
    fallback_ids = iter(identities.resolve(degraded))
    for did, title_e, blocks, link, updated, summary, truncated in parsed:
        docs.append(
            document(
                did or next(fallback_ids),
                title_e,
                blocks,
                url=link,
                updated=updated,
                summary=summary,
                truncated=truncated,
            )
        )
    identities.members = [doc["document_id"] for doc in docs]
    docs.sort(key=article_order)
    return title[:256], docs[:50]


def feed_article_blocks(blocks):
    """Keep a bounded readable prefix without rejecting other feed entries."""
    lengths = [len(block["text"].encode("utf-8")) for block in blocks]
    if (
        sum(lengths) + max(0, len(blocks) - 1) * 2 <= FEED_ARTICLE_MAX_BYTES
        and len(blocks) <= 500
    ):
        return blocks, False
    # Older readers render blocks but do not display the truncated metadata.
    # Reserve space for a visible notice inside both the byte and block budgets.
    notice = {
        "block_id": "readocs-note-truncated",
        "type": "paragraph",
        "text": FEED_TRUNCATION_NOTE,
        "depth": 0,
        "meta": {},
    }
    budget = FEED_ARTICLE_MAX_BYTES - len(FEED_TRUNCATION_NOTE.encode("utf-8")) - 2
    selected, size = [], 0
    for block, length in zip(blocks, lengths):
        if len(selected) >= 499:
            break
        # Include paragraph separators in the text budget.
        length += 2 if selected else 0
        if size + length > budget:
            if not selected:
                prefix = (
                    block["text"]
                    .encode("utf-8")[:budget]
                    .decode("utf-8", errors="ignore")
                )
                selected = blocks_from_text(prefix)
            break
        selected.append(block)
        size += length
    return [notice, *selected], True


def feishu_blocks(raw):
    by_id = {b["block_id"]: b for b in raw}
    if len(by_id) != len(raw):
        raise Problem("PARSE_ERROR", "文档块 ID 重复，旧版本已保留", 502)
    roots = [b for b in raw if b.get("block_type") == 1]
    if len(roots) != 1:
        raise Problem("PARSE_ERROR", "文档缺少唯一根节点", 502)
    output, visited = [], set()

    def walk(bid, depth):
        if bid in visited or bid not in by_id or depth > 32:
            raise Problem("PARSE_ERROR", "文档结构不完整，旧版本已保留", 502)
        visited.add(bid)
        b = by_id[bid]
        kind = b.get("block_type")
        fields = {
            2: ("text", "paragraph"),
            3: ("heading1", "h1"),
            4: ("heading2", "h2"),
            5: ("heading3", "h3"),
            12: ("bullet", "bullet"),
            13: ("ordered", "ordered"),
            14: ("code", "code"),
            15: ("quote", "quote"),
            17: ("todo", "bullet"),
        }
        if kind in fields:
            field, typ = fields[kind]
            obj = b.get(field, {})
            parts = []
            for el in obj.get("elements", []):
                if "text_run" in el:
                    parts.append(el["text_run"].get("content", ""))
                elif "mention_doc" in el:
                    parts.append(el["mention_doc"].get("title", "[文档链接]"))
                elif "equation" in el:
                    parts.append("[公式：" + el["equation"].get("content", "") + "]")
                else:
                    parts.append("[富文本元素]")
            output.append(
                {
                    "block_id": bid,
                    "type": typ,
                    "text": "".join(parts),
                    "depth": max(0, depth - 1),
                    "meta": obj.get("style", {}),
                }
            )
        elif kind != 1:
            label = {22: "分隔线", 27: "图片", 31: "表格", 32: "单元格"}.get(
                kind, "复杂内容"
            )
            output.append(
                {
                    "block_id": bid,
                    "type": "separator" if kind == 22 else "placeholder",
                    "text": f"[{label}]",
                    "depth": max(0, depth - 1),
                    "meta": {},
                }
            )
        for child in b.get("children", []):
            walk(child, depth + 1)

    walk(roots[0]["block_id"], 0)
    if len(visited) != len(raw):
        raise Problem("PARSE_ERROR", "文档包含未关联内容，旧版本已保留", 502)
    return output
