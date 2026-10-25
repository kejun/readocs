import importlib.util
import json
import sys
import types
from pathlib import Path

import pytest


@pytest.fixture
def device_tool(monkeypatch):
    # Protocol tests do not need the optional pyserial package or real USB.
    serial = types.ModuleType("serial")
    serial.Serial = None
    serial.SerialException = type("SerialException", (OSError,), {})
    monkeypatch.setitem(sys.modules, "serial", serial)
    path = Path(__file__).resolve().parents[1] / "scripts/device.py"
    spec = importlib.util.spec_from_file_location("readocs_device_tool", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SerialReplies:
    def __init__(self, replies):
        self.replies = iter(replies)
        self.reads = 0
        self.sent = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def reset_input_buffer(self):
        pass

    def write(self, body):
        self.sent = json.loads(body)

    def flush(self):
        pass

    def readline(self):
        self.reads += 1
        return (next(self.replies) + "\n").encode()


@pytest.mark.parametrize("ok", [True, False])
def test_bind_waits_for_its_own_result(device_tool, monkeypatch, capsys, ok):
    replies = SerialReplies(
        [
            'RESULT {"command":"sync","ok":true}',
            'STATUS {"busy":true}',
            "RESULT malformed",
            'RESULT {"command":"bind","ok":"true"}',
            "RESULT " + json.dumps({"command": "bind", "ok": ok}),
        ]
    )
    monkeypatch.setattr(device_tool.serial, "Serial", lambda *a, **k: replies)
    monkeypatch.setattr("builtins.input", lambda _: "pairing-code")
    monkeypatch.setattr(
        sys, "argv", ["device.py", "bind", "--server", "https://device.example"]
    )
    if ok:
        device_tool.main()
    else:
        with pytest.raises(SystemExit) as error:
            device_tool.main()
        assert error.value.code == 1
    assert replies.reads == 5
    assert replies.sent["cmd"] == "bind"
    output = capsys.readouterr().out
    assert '"command": "bind"' in output
    assert "sync" not in output and "STATUS" not in output
    assert "pairing-code" not in output


def test_status_ignores_background_results(device_tool, monkeypatch, capsys):
    replies = SerialReplies(
        ['RESULT {"command":"sync","ok":true}', 'STATUS {"busy":false}']
    )
    monkeypatch.setattr(device_tool.serial, "Serial", lambda *a, **k: replies)
    monkeypatch.setattr(sys, "argv", ["device.py", "status"])
    device_tool.main()
    assert replies.reads == 2
    assert capsys.readouterr().out == 'STATUS {"busy":false}\n'


def test_queued_command_has_time_after_automatic_sync(device_tool, monkeypatch, capsys):
    clock = [0]
    replies = SerialReplies(
        [
            "ACCEPTED check",
            'RESULT {"command":"sync","ok":true}',
            "waiting",
            'RESULT {"command":"check","ok":true}',
        ]
    )
    next_line = replies.readline
    times = iter([1, 20, 36, 40])

    def readline():
        clock[0] = next(times)
        return next_line()

    replies.readline = readline
    monkeypatch.setattr(device_tool.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(device_tool.serial, "Serial", lambda *a, **k: replies)
    monkeypatch.setattr(sys, "argv", ["device.py", "check"])
    device_tool.main()
    assert clock[0] == 40 and replies.reads == 4
    assert capsys.readouterr().out == 'RESULT {"command":"check","ok":true}\n'


def test_console_error_fails_command(device_tool, monkeypatch, capsys):
    replies = SerialReplies(["ERROR invalid command"])
    monkeypatch.setattr(device_tool.serial, "Serial", lambda *a, **k: replies)
    monkeypatch.setattr(sys, "argv", ["device.py", "check"])
    with pytest.raises(SystemExit) as error:
        device_tool.main()
    assert error.value.code == 1
    assert capsys.readouterr().out == "ERROR invalid command\n"
