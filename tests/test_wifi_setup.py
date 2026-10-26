import os
import shutil
import subprocess
from pathlib import Path

import pytest


def function(source, marker):
    return marker + source.split(marker, 1)[1].split("\n}\n", 1)[0] + "\n}\n"


def test_actual_wifi_setup_http_and_connection_lifecycle(tmp_path):
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
    for name in ("Profile", "Config", "Action"):
        marker = "struct " + name + " {"
        types += (
            marker + declarations.split(marker, 1)[1].split("\n};", 1)[0] + "\n};\n"
        )
    network = (main / "network.cpp").read_text()
    marker = "struct SetupOperation {"
    types += marker + network.split(marker, 1)[1].split("\n};", 1)[0] + "\n};\n"
    (tmp_path / "setup_types.hpp").write_text(types)
    markers = (
        "bool enqueue(",
        "static bool connect_profile(",
        "bool stop_setup(",
        "bool start_setup(",
        "static esp_err_t send_json(",
        "static esp_err_t setup_error(",
        "static void setup_result(",
        "static bool setup_failure(",
        "static bool configure_wifi(",
        "static void service_setup(",
        "static bool setup_action_is_current(",
        "static esp_err_t web(",
        "static esp_err_t setup_socket_open(",
    )
    (tmp_path / "setup_under_test.hpp").write_text(
        "".join(function(network, marker) for marker in markers)
    )
    cc, cxx = shutil.which("clang"), shutil.which("clang++")
    assert cc and cxx
    obj = tmp_path / "json.o"
    subprocess.run(
        [cc, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True
    )
    binary = tmp_path / "wifi-setup"
    subprocess.run(
        [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            "-I",
            str(cjson),
            "-I",
            str(main),
            "-I",
            str(tmp_path),
            str(root / "tests/wifi_setup.cpp"),
            str(obj),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=15)
