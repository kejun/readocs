import shutil
import subprocess
from pathlib import Path


def function(source, marker):
    return marker + source.split(marker, 1)[1].split("\n}\n", 1)[0] + "\n}\n"


def test_actual_sd_reads_invalidate_damaged_or_missing_verified_objects(tmp_path):
    root = Path(__file__).resolve().parents[1]
    main = root / "firmware/device/main"
    source = (main / "storage.cpp").read_text()
    (tmp_path / "sd_storage_under_test.hpp").write_text(
        "".join(
            function(source, marker)
            for marker in (
                "bool cache_body_verified(",
                "static void remember_verified(",
                "static void forget_verified(",
                "std::string read_file(",
                "bool write_file(",
            )
        )
    )
    binary = tmp_path / "sd-storage"
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(main),
            "-I",
            str(tmp_path),
            str(root / "tests/sd_storage.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary), str(tmp_path)], check=True)
