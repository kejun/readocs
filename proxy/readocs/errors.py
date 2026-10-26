class Problem(Exception):
    def __init__(
        self, code: str, message: str, status: int = 400, retry_after: int = 0
    ):
        self.code, self.message, self.status, self.retry_after = (
            code,
            message,
            status,
            retry_after,
        )
        super().__init__(code)


TRANSIENT_ERRORS = {"UPSTREAM_TIMEOUT", "RATE_LIMITED", "REVISION_CHANGED"}


def retry_delay(error, failures, interval):
    """Recovery deadlines are independent of the normal content check interval."""
    code = error.code if isinstance(error, Problem) else "UPSTREAM_TIMEOUT"
    if code == "REVISION_CHANGED":
        base, cap = 5, 60
    elif code in TRANSIENT_ERRORS:
        base, cap = 30, 900
    else:
        base, cap = interval, 21600
    delay = min(cap, base * 2 ** min(max(0, failures - 1), 6))
    return max(delay, error.retry_after if isinstance(error, Problem) else 0)
