import shutil
import subprocess
from pathlib import Path


def test_actual_sleep_guard_does_not_discard_input(tmp_path):
    root = Path(__file__).resolve().parents[1]
    source = (root / "firmware/device/main/main.cpp").read_text()
    guard = ""
    for signature in (
        "static bool ui_idle(",
        "static bool ready_to_leave_ui(",
        "static void sleep_now(",
    ):
        guard += (
            signature + source.split(signature, 1)[1].split("\n}\n", 1)[0] + "\n}\n"
        )
    (tmp_path / "sleep_guard_under_test.hpp").write_text(guard)
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler, "A host C++ compiler is required for firmware logic tests"
    binary = tmp_path / "sleep-test"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(tmp_path),
            "-I",
            str(root / "firmware/device/main"),
            str(root / "tests/sleep_logic.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
