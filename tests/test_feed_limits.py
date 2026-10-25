from datetime import UTC, datetime, timedelta
from xml.etree.ElementTree import Element, SubElement, tostring

import pytest
from readocs.content import ATOM, FeedIdentities, parse_feed
from readocs.errors import Problem
from readocs.store import canonical, digest

ARTICLE_LIMIT = 64 * 1024
FEED_LIMIT = 16 * 1024 * 1024
TRUNCATION_NOTE = "正文较长，以下为部分内容；完整内容请查看原文。"
TRUNCATED_BODY_BUDGET = ARTICLE_LIMIT - len(TRUNCATION_NOTE.encode()) - 2


def atom(entries):
    root = Element(ATOM + "feed")
    SubElement(root, ATOM + "title").text = "Test feed"
    for values in entries:
        item = SubElement(root, ATOM + "entry")
        if values.get("id") is not None:
            SubElement(item, ATOM + "id").text = values["id"]
        SubElement(item, ATOM + "title").text = values.get("title", "Article")
        if values.get("updated"):
            SubElement(item, ATOM + "updated").text = values["updated"]
        if values.get("url"):
            SubElement(item, ATOM + "link", href=values["url"], rel="alternate")
        field = "summary" if values.get("summary") else "content"
        SubElement(item, ATOM + field, type=values.get("type", "text")).text = values[
            "text"
        ]
    return tostring(root, encoding="utf-8")


def body_text(doc):
    return "\n\n".join(block["text"] for block in doc["blocks"])


def article_blocks(doc):
    if doc["truncated"]:
        note = doc["blocks"][0]
        assert note["block_id"] == "readocs-note-truncated"
        assert note["type"] == "paragraph"
        assert note["text"] == TRUNCATION_NOTE
        return doc["blocks"][1:]
    assert all(block["block_id"] != "readocs-note-truncated" for block in doc["blocks"])
    return doc["blocks"]


def assert_content_revision(doc):
    visible = {
        key: doc[key] for key in ("title", "blocks", "truncated", "summary", "url")
    }
    assert doc["content_revision"] == digest(canonical(visible))


def test_atom_larger_than_two_mib_keeps_latest_fifty_complete_articles():
    entries = [
        {
            "id": f"article-{i}",
            "title": f"Article {i}",
            "text": "x" * 32000,
            "updated": (
                datetime(2026, 1, 1, tzinfo=UTC) + timedelta(days=i)
            ).isoformat(),
        }
        for i in range(80)
    ]
    raw = atom(entries)
    assert 2 * 1024 * 1024 < len(raw) < FEED_LIMIT
    _, docs = parse_feed(raw, "feed")
    assert [doc["title"] for doc in docs] == [f"Article {i}" for i in range(79, 29, -1)]
    assert all(not doc["truncated"] and not doc["summary"] for doc in docs)
    assert all(body_text(doc) == "x" * 32000 for doc in docs)


def test_feed_over_sixteen_mib_reports_size_without_false_dtd_error():
    raw = atom([{"id": "large", "text": "x" * FEED_LIMIT}])
    assert len(raw) > FEED_LIMIT
    with pytest.raises(Problem) as raised:
        parse_feed(raw, "feed")
    assert raised.value.code == "CONTENT_TOO_LARGE"
    assert raised.value.status == 422
    assert "DTD" not in raised.value.message
    assert "实体声明" not in raised.value.message


def test_feed_dtd_remains_forbidden_with_the_larger_transport_budget():
    raw = (
        b'<!DOCTYPE feed [<!ENTITY x "unsafe">]>'
        b'<feed xmlns="http://www.w3.org/2005/Atom"><title>&x;</title></feed>'
    )
    with pytest.raises(Problem) as raised:
        parse_feed(raw, "feed")
    assert raised.value.code == "PARSE_ERROR"


def test_long_articles_are_independent_prefixes_with_links_and_summary_semantics():
    raw = atom(
        [
            {"id": "normal", "title": "Normal", "text": "Complete article"},
            {
                "id": "long",
                "title": "Long",
                "type": "html",
                "text": "<p>Lead</p><p>" + "x" * 65000 + "</p><p>" + "y" * 600 + "</p>",
                "url": "https://example.com/long",
            },
            {
                "id": "summary",
                "title": "Summary",
                "text": "s" * (ARTICLE_LIMIT + 1),
                "summary": True,
                "url": "https://example.com/summary",
            },
        ]
    )
    _, docs = parse_feed(raw, "feed")
    by_title = {doc["title"]: doc for doc in docs}
    assert set(by_title) == {"Normal", "Long", "Summary"}
    assert body_text(by_title["Normal"]) == "Complete article"
    assert not by_title["Normal"]["truncated"]
    assert [block["text"] for block in article_blocks(by_title["Long"])] == [
        "Lead",
        "x" * 65000,
    ]
    assert by_title["Long"]["truncated"]
    assert not by_title["Long"]["summary"]
    assert by_title["Long"]["url"] == "https://example.com/long"
    assert by_title["Summary"]["summary"] and by_title["Summary"]["truncated"]
    assert by_title["Summary"]["url"] == "https://example.com/summary"
    for doc in docs:
        article_blocks(doc)
        assert len(body_text(doc).encode()) <= ARTICLE_LIMIT
        assert len(canonical(doc)) <= 300 * 1024
        assert_content_revision(doc)
    assert parse_feed(raw, "feed")[1] == docs


@pytest.mark.parametrize(
    ("text", "expected", "truncated"),
    [
        ("a" * ARTICLE_LIMIT, "a" * ARTICLE_LIMIT, False),
        ("a" * (ARTICLE_LIMIT + 1), "a" * TRUNCATED_BODY_BUDGET, True),
        ("中" * 21845, "中" * 21845, False),
        ("中" * 21846, "中" * (TRUNCATED_BODY_BUDGET // 3), True),
        ("a" + "中" * 21845, "a" + "中" * 21845, False),
        (
            "ab" + "中" * 21845,
            "ab" + "中" * ((TRUNCATED_BODY_BUDGET - 2) // 3),
            True,
        ),
    ],
    ids=(
        "ascii-exact",
        "ascii-over",
        "chinese-below",
        "chinese-over",
        "mixed-exact",
        "mixed-over",
    ),
)
def test_single_paragraph_byte_boundaries_preserve_valid_utf8(
    text, expected, truncated
):
    doc = parse_feed(atom([{"id": "entry", "text": text}]), "feed")[1][0]
    assert [block["text"] for block in article_blocks(doc)] == [expected]
    assert doc["truncated"] is truncated
    assert len(body_text(doc).encode()) <= ARTICLE_LIMIT
    assert "\ufffd" not in body_text(doc)
    assert_content_revision(doc)


@pytest.mark.parametrize(
    ("second", "truncated"), [("bbb", False), ("bbbb", False), ("bbbbb", True)]
)
def test_paragraph_separators_count_toward_the_article_budget(second, truncated):
    first = "a" * (ARTICLE_LIMIT - 6)
    doc = parse_feed(atom([{"id": "entry", "text": first + "\n\n" + second}]), "feed")[
        1
    ][0]
    assert doc["truncated"] is truncated
    assert [block["text"] for block in article_blocks(doc)] == (
        ["a" * TRUNCATED_BODY_BUDGET] if truncated else [first, second]
    )
    assert len(body_text(doc).encode()) <= ARTICLE_LIMIT


@pytest.mark.parametrize("count", [500, 501])
def test_the_snapshot_block_limit_still_applies_to_small_paragraphs(count):
    paragraphs = [f"Paragraph {i}" for i in range(count)]
    text = "\n\n".join(paragraphs)
    assert len(text.encode()) < ARTICLE_LIMIT
    doc = parse_feed(atom([{"id": "entry", "text": text}]), "feed")[1][0]
    assert [block["text"] for block in article_blocks(doc)] == paragraphs[
        : 499 if count > 500 else 500
    ]
    assert len(doc["blocks"]) == 500
    assert doc["truncated"] is (count > 500)
    assert_content_revision(doc)


def test_degraded_ids_use_the_full_body_beyond_a_shared_truncated_prefix():
    prefix = "x" * (ARTICLE_LIMIT + 100)
    entries = [
        {"text": prefix + "First tail"},
        {"text": prefix + "Second tail"},
    ]
    identities = FeedIdentities()
    _, docs = parse_feed(atom(entries), "feed", identities=identities)
    assert len(docs) == 2
    assert all(doc["truncated"] for doc in docs)
    assert [block["text"] for block in article_blocks(docs[0])] == [
        "x" * TRUNCATED_BODY_BUDGET
    ]
    assert article_blocks(docs[0]) == article_blocks(docs[1])
    assert len({row["fingerprint"] for row in identities.records}) == 2
    assigned = {row["fingerprint"]: row["document_id"] for row in identities.records}
    restored = FeedIdentities(identities.records)
    _, reordered = parse_feed(
        atom(list(reversed(entries))), "feed", identities=restored
    )
    assert {
        row["fingerprint"]: row["document_id"] for row in restored.records
    } == assigned
    assert reordered == docs
