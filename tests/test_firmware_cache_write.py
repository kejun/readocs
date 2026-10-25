import shutil
import subprocess
from pathlib import Path


def test_actual_chunked_cache_publication(tmp_path):
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler, "A host C++ compiler is required for cache write tests"
    binary = tmp_path / "cache-write-test"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(root / "firmware/device/main"),
            str(root / "tests/firmware_cache_write.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary), str(tmp_path)], check=True)
