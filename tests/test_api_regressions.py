import asyncio
import shutil
import subprocess
from pathlib import Path

import httpx
import pytest
from aiohttp import web
from readocs.app import create_app
from readocs.errors import Problem
from readocs.network import Network


class CredentialNetwork:
    def __init__(self):
        self.block = False
        self.pending = asyncio.Queue()

    async def json(self, method, url, **kwargs):
        if self.block:
            completed = asyncio.get_running_loop().create_future()
            self.pending.put_nowait(completed)
            await completed
        return 200, {"access_token": "test-token", "expires_in": 7200}


@pytest.fixture
async def api(tmp_path):
    network = CredentialNetwork()
    app = create_app(tmp_path, network=network, background=False)
    async with httpx.AsyncClient(
        transport=httpx.ASGITransport(app=app),
        base_url="http://127.0.0.1",
        headers={"X-Readocs-Request": "1"},
    ) as client:
        response = await client.post(
            "/api/login", json={"key": app.state.store.admin_key}
        )
        assert response.status_code == 200
        yield app, client, network
    app.state.store.db.close()


async def start_save(client, network, secret):
    network.block = True
    task = asyncio.create_task(
        client.put("/api/feishu", json={"app_id": "test-app", "secret": secret})
    )
    completion = await asyncio.wait_for(network.pending.get(), 2)
    return task, completion


@pytest.mark.parametrize("initially_configured", [False, True])
async def test_pending_save_cannot_restore_removed_configuration(
    api, initially_configured
):
    app, client, network = api
    if initially_configured:
        assert (
            await client.put(
                "/api/feishu", json={"app_id": "test-app", "secret": "original"}
            )
        ).status_code == 200

    pending, completion = await start_save(client, network, "pending-secret")
    assert (await client.delete("/api/feishu")).status_code == 200
    completion.set_result(None)
    response = await pending

    assert response.status_code == 409
    assert response.json()["code"] == "CONFIG_CHANGED"
    assert app.state.store.get("feishu") is None
    assert app.state.store.source("feishu") is None
    assert app.state.feishu.cached is None


async def test_pending_save_cannot_replace_recreated_configuration(api):
    app, client, network = api
    pending, completion = await start_save(client, network, "stale-secret")
    assert (await client.delete("/api/feishu")).status_code == 200
    replacement, replacement_completion = await start_save(
        client, network, "replacement-secret"
    )
    replacement_completion.set_result(None)
    assert (await replacement).status_code == 200
    generation = app.state.store.get("feishu")["generation"]
    completion.set_result(None)

    assert (await pending).status_code == 409
    saved = app.state.store.get("feishu")
    assert saved["secret"] == "replacement-secret"
    assert saved["generation"] == generation
    assert app.state.store.source("feishu")["generation"] == generation


async def test_concurrent_saves_do_not_silently_overwrite_a_completed_save(api):
    app, client, network = api
    earlier, earlier_completion = await start_save(client, network, "earlier-secret")
    later, later_completion = await start_save(client, network, "later-secret")
    later_completion.set_result(None)
    assert (await later).status_code == 200
    earlier_completion.set_result(None)

    assert (await earlier).status_code == 409
    assert app.state.store.get("feishu")["secret"] == "later-secret"


@pytest.mark.parametrize("method,status", [("GET", 304), ("GET", 204), ("HEAD", 200)])
async def test_bodyless_http_response_keeps_compression_metadata(
    monkeypatch, method, status
):
    app = web.Application()

    async def respond(request):
        return web.Response(
            status=status, headers={"Content-Encoding": "gzip", "ETag": '"same"'}
        )

    app.router.add_route(method, "/feed", respond)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    # The production URL checks remain intact; only this local HTTP fixture bypasses them.
    monkeypatch.setattr("readocs.network.validate_url", lambda url: url)
    try:
        actual_status, headers, body = await Network().request(
            method, f"http://127.0.0.1:{port}/feed"
        )
        assert actual_status == status
        assert headers["Content-Encoding"] == "gzip"
        assert headers["Etag"] == '"same"'
        assert body == b""
    finally:
        await runner.cleanup()


async def test_truncated_compressed_body_is_still_rejected(monkeypatch):
    app = web.Application()

    async def respond(request):
        return web.Response(body=b"\x1f\x8b", headers={"Content-Encoding": "gzip"})

    app.router.add_get("/feed", respond)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    monkeypatch.setattr("readocs.network.validate_url", lambda url: url)
    try:
        with pytest.raises(Problem, match="PARSE_ERROR"):
            await Network().request("GET", f"http://127.0.0.1:{port}/feed")
    finally:
        await runner.cleanup()


def test_actual_browser_preview_request_order():
    node = shutil.which("node")
    assert node, "Node.js is required for browser request ordering tests"
    subprocess.run(
        [node, str(Path(__file__).with_name("preview_logic.js"))], check=True
    )
