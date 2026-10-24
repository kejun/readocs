import re
from datetime import UTC, datetime, timedelta, timezone
from email.utils import parsedate_to_datetime

TITLE_DATE = re.compile(
    r"(?<!\d)(20\d{2})\s*[-/.年]\s*(\d{1,2})\s*[-/.月]\s*(\d{1,2})(?:日)?(?!\d)"
)
CHINA_TIME = timezone(timedelta(hours=8))


def timestamp(value):
    """Accept upstream Unix seconds, ISO 8601 and RSS/RFC 2822 dates."""
    if isinstance(value, bool) or not isinstance(value, (str, int, float)):
        return None
    try:
        text = str(value).strip()
        if not text or len(text) > 128:
            return None
        if re.fullmatch(r"-?\d+(?:\.\d+)?", text):
            date = datetime.fromtimestamp(float(text), UTC)
        else:
            try:
                date = datetime.fromisoformat(text)
            except ValueError:
                date = parsedate_to_datetime(text)
        if date.tzinfo is None:
            date = date.replace(tzinfo=UTC)
        return date.astimezone(UTC).timestamp()
    except (ValueError, TypeError, OverflowError, OSError):
        return None


def upstream_time(value):
    seconds = timestamp(value)
    if seconds is not None:
        return datetime.fromtimestamp(seconds, UTC).isoformat()
    return None


def title_timestamp(title):
    for match in TITLE_DATE.finditer(title):
        try:
            return datetime(
                *(int(part) for part in match.groups()), tzinfo=CHINA_TIME
            ).timestamp()
        except ValueError:
            continue
    return None


def article_order(item, *, feishu=False):
    candidates = (
        (
            title_timestamp(item["title"]),
            timestamp(item.get("content_created_at")),
            timestamp(item.get("content_updated_at")),
        )
        if feishu
        else (timestamp(item.get("content_updated_at")),)
    )
    date = next((value for value in candidates if value is not None), None)
    # Unknown dates sort last; equal dates always keep the same pagination order.
    return (date is None, -date if date is not None else 0, item["document_id"])
