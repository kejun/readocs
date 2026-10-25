import shutil
import subprocess
from pathlib import Path

import pytest


@pytest.mark.parametrize(
    "source",
    [
        "input_logic.cpp",
        "navigation_logic.cpp",
        "source_navigation.cpp",
        "layout_width.cpp",
    ],
)
def test_actual_firmware_input_logic(tmp_path, source):
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler, "A host C++ compiler is required for firmware logic tests"
    binary = tmp_path / "input-test"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(root / "firmware/device/main"),
            str(root / "tests" / source),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
