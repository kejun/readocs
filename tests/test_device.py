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
    tools = types.ModuleType("serial.tools")
    tools.list_ports = types.SimpleNamespace(
        comports=lambda: [
            types.SimpleNamespace(device="/dev/test", vid=0x303A, pid=0x1001)
        ]
    )
    monkeypatch.setitem(sys.modules, "serial.tools", tools)
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


@pytest.mark.parametrize(
    "queued_action, reply_times",
    [
        ("sync", [1, 20, 36, 40]),
        ("download", [1, 60, 75, 80]),
        ("sync", [1, 120, 140, 150]),
    ],
)
def test_queued_command_has_time_after_network_action(
    device_tool, monkeypatch, capsys, queued_action, reply_times
):
    clock = [0]
    replies = SerialReplies(
        [
            "ACCEPTED check",
            "RESULT " + json.dumps({"command": queued_action, "ok": True}),
            "waiting",
            'RESULT {"command":"check","ok":true}',
        ]
    )
    next_line = replies.readline
    times = iter(reply_times)

    def readline():
        clock[0] = next(times)
        return next_line()

    replies.readline = readline
    monkeypatch.setattr(device_tool.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(device_tool.serial, "Serial", lambda *a, **k: replies)
    monkeypatch.setattr(sys, "argv", ["device.py", "check"])
    device_tool.main()
    assert clock[0] == reply_times[-1] and replies.reads == 4
    assert capsys.readouterr().out == 'RESULT {"command":"check","ok":true}\n'


def test_console_error_fails_command(device_tool, monkeypatch, capsys):
    replies = SerialReplies(["ERROR invalid command"])
    monkeypatch.setattr(device_tool.serial, "Serial", lambda *a, **k: replies)
    monkeypatch.setattr(sys, "argv", ["device.py", "check"])
    with pytest.raises(SystemExit) as error:
        device_tool.main()
    assert error.value.code == 1
    assert capsys.readouterr().out == "ERROR invalid command\n"


@pytest.mark.parametrize("count", [0, 2])
def test_ambiguous_or_missing_usb_port_requires_explicit_selection(
    device_tool, monkeypatch, count
):
    monkeypatch.setattr(
        device_tool.list_ports,
        "comports",
        lambda: [
            types.SimpleNamespace(device=f"/dev/usb{i}", vid=0x303A, pid=0x1001)
            for i in range(count)
        ],
    )
    monkeypatch.setattr(sys, "argv", ["device.py", "status"])
    with pytest.raises(SystemExit) as error:
        device_tool.main()
    assert error.value.code == 2


def test_setup_cancel_uses_protocol_name_and_explicit_port(
    device_tool, monkeypatch, capsys
):
    replies = SerialReplies(['RESULT {"command":"setup_cancel","ok":true}'])
    monkeypatch.setattr(device_tool.list_ports, "comports", list)
    monkeypatch.setattr(device_tool.serial, "Serial", lambda *a, **k: replies)
    monkeypatch.setattr(
        sys, "argv", ["device.py", "setup-cancel", "--port", "/dev/explicit"]
    )
    device_tool.main()
    assert replies.sent == {"cmd": "setup_cancel"}
    assert '"ok":true' in capsys.readouterr().out
