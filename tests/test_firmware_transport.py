import os
import shutil
import subprocess
from pathlib import Path

import pytest


def function(source, marker):
    return marker + source.split(marker, 1)[1].split("\n}\n", 1)[0] + "\n}\n"


def test_actual_transport_reconnects_local_get_before_public_fallback(tmp_path):
    root = Path(__file__).resolve().parents[1]
    main = root / "firmware/device/main"
    sdk = Path(os.environ.get("IDF_PATH", root.parent / "esp/esp-idf"))
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        pytest.skip("ESP-IDF cJSON sources are required")
    declarations = (main / "readocs.hpp").read_text()
    types = (
        "struct JsonDelete {"
        + declarations.split("struct JsonDelete {", 1)[1].split("struct Profile", 1)[0]
    )
    network = (main / "network.cpp").read_text()
    types += (
        "struct Http {"
        + network.split("struct Http {", 1)[1].split("\n};", 1)[0]
        + "\n};\n"
    )
    (tmp_path / "transport_types.hpp").write_text(types)
    (tmp_path / "transport_under_test.hpp").write_text(
        function(network, "static void close_http(")
        + function(network, "static int request(")
    )
    cc, cxx = shutil.which("clang"), shutil.which("clang++")
    assert cc and cxx
    obj = tmp_path / "json.o"
    subprocess.run(
        [cc, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True
    )
    binary = tmp_path / "transport"
    subprocess.run(
        [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(cjson),
            "-I",
            str(tmp_path),
            str(root / "tests/firmware_transport.cpp"),
            str(obj),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
