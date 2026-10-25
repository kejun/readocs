import os
import shutil
import subprocess
from pathlib import Path

import pytest


def function(source, marker):
    return marker + source.split(marker, 1)[1].split("\n}\n", 1)[0] + "\n}\n"


def test_actual_source_metadata_cache_and_early_publication(tmp_path):
    root = Path(__file__).resolve().parents[1]
    sdk = Path(os.environ.get("IDF_PATH", root.parent / "esp/esp-idf"))
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        pytest.skip(
            "ESP-IDF cJSON source is required for the cache integration harness"
        )
    compiler = shutil.which("clang++") or shutil.which("g++")
    c_compiler = shutil.which("clang") or shutil.which("gcc")
    assert compiler and c_compiler, "Host C/C++ compilers are required"
    main = root / "firmware/device/main"
    declarations = (main / "readocs.hpp").read_text()
    helpers = declarations.split("struct JsonDelete {", 1)[1].split(
        "struct Profile", 1
    )[0]
    types = "struct JsonDelete {" + helpers
    for name in ("Entry", "Source"):
        types += (
            "struct "
            + name
            + " {"
            + declarations.split("struct " + name + " {", 1)[1].split("\n};", 1)[0]
            + "\n};\n"
        )
    (tmp_path / "source_types.hpp").write_text(types)
    storage = (main / "storage.cpp").read_text()
    (tmp_path / "source_storage_under_test.hpp").write_text(
        function(storage, "static bool load_index(")
        + function(storage, "bool commit_cache(")
    )
    network = (main / "network.cpp").read_text()
    publication = (
        "auto retained = old;"
        + network.split("auto retained = old;", 1)[1].split("    Json directory(", 1)[0]
    )
    (tmp_path / "source_publication_under_test.hpp").write_text(publication)
    obj = tmp_path / "cJSON.o"
    subprocess.run(
        [c_compiler, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)],
        check=True,
    )
    binary = tmp_path / "sources-test"
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
            str(cjson),
            "-I",
            str(tmp_path),
            str(root / "tests/firmware_sources.cpp"),
            str(obj),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
