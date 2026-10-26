import os
import shutil
import subprocess
from pathlib import Path

import pytest


def function(source, marker):
    return marker + source.split(marker, 1)[1].split("\n}\n", 1)[0] + "\n}\n"


def test_actual_download_validation_and_error_preservation(tmp_path):
    root = Path(__file__).resolve().parents[1]
    sdk = Path(os.environ.get("IDF_PATH", root.parent / "esp/esp-idf"))
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        pytest.skip(
            "ESP-IDF cJSON source is required for the download integration harness"
        )
    compiler = shutil.which("clang++") or shutil.which("g++")
    c_compiler = shutil.which("clang") or shutil.which("gcc")
    assert compiler and c_compiler, "Host C/C++ compilers are required"
    main = root / "firmware/device/main"
    declarations = (main / "readocs.hpp").read_text()
    types = (
        "struct JsonDelete {"
        + declarations.split("struct JsonDelete {", 1)[1].split("struct Profile", 1)[0]
    )
    for name in ("Entry", "Source"):
        types += (
            "struct "
            + name
            + " {"
            + declarations.split("struct " + name + " {", 1)[1].split("\n};", 1)[0]
            + "\n};\n"
        )
    network = (main / "network.cpp").read_text()
    types += (
        "struct Http {"
        + network.split("struct Http {", 1)[1].split("\n};", 1)[0]
        + "\n};\n"
    )
    (tmp_path / "download_types.hpp").write_text(types)
    (tmp_path / "download_under_test.hpp").write_text(
        "".join(
            function(network, marker)
            for marker in (
                "static bool body_failure(",
                "static bool download(",
                "static bool safe_id(",
                "static bool fetch_entry(",
                "static void repair_external_entries(",
                "static bool download_article(",
            )
        )
    )
    obj = tmp_path / "cJSON.o"
    subprocess.run(
        [c_compiler, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)],
        check=True,
    )
    binary = tmp_path / "download-test"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(cjson),
            "-I",
            str(tmp_path),
            str(root / "tests/firmware_download.cpp"),
            str(obj),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
