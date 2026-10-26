import os
import shutil
import subprocess
from pathlib import Path

import pytest


def function(source, marker):
    return marker + source.split(marker, 1)[1].split("\n}\n", 1)[0] + "\n}\n"


def test_actual_full_sd_sync_resumes_without_starving_later_articles(tmp_path):
    root = Path(__file__).resolve().parents[1]
    main = root / "firmware/device/main"
    sdk = Path(os.environ.get("IDF_PATH", root.parent / "esp/esp-idf"))
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        pytest.skip("ESP-IDF cJSON sources are required")
    cc, cxx = shutil.which("clang"), shutil.which("clang++")
    assert cc and cxx
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
    (tmp_path / "sd_types.hpp").write_text(types)
    markers = (
        "static bool body_failure(",
        "static bool download(",
        "static bool safe_id(",
        "static bool save_directory_progress(",
        "static void compact_directory_progress(",
        "static bool fetch_entry(",
        "static void repair_external_entries(",
        "static bool sync(",
    )
    (tmp_path / "sd_sync_under_test.hpp").write_text(
        "static constexpr size_t directory_limit = 512 * 1024;\n"
        + "".join(function(network, marker) for marker in markers)
    )
    obj = tmp_path / "json.o"
    subprocess.run(
        [cc, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True
    )
    binary = tmp_path / "sd-sync"
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
            str(main),
            "-I",
            str(tmp_path),
            str(root / "tests/sd_sync.cpp"),
            str(obj),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
