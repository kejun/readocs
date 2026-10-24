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
