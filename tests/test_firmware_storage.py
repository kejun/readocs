import shutil
import subprocess
from pathlib import Path


def test_firmware_cache_and_position_storage(tmp_path):
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler, "A host C++ compiler is required for firmware storage tests"
    binary = tmp_path / "storage-test"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(root / "tests/firmware_stubs"),
            "-I",
            str(root / "firmware/device/main"),
            str(root / "tests/firmware_storage.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary), str(tmp_path)], check=True)
