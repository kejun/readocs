import shutil
import subprocess
from pathlib import Path


def test_actual_ap_access_accepts_real_dual_stack_ipv4_socket(tmp_path):
    root = Path(__file__).resolve().parents[1]
    source = (root / "firmware/device/main/network.cpp").read_text()
    signature = "static bool on_ap("
    actual = signature + source.split(signature, 1)[1].split("\n}\n", 1)[0] + "\n}\n"
    (tmp_path / "setup_access_under_test.hpp").write_text(actual)
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler
    binary = tmp_path / "setup-access"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(root / "firmware/device/main"),
            "-I",
            str(tmp_path),
            str(root / "tests/setup_access.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
