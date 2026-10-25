import os
import shutil
import subprocess
from pathlib import Path

import pytest


def function(source, marker):
    return marker + source.split(marker, 1)[1].split("\n}\n", 1)[0] + "\n}\n"


def test_actual_cache_gc_and_commit_diagnostics(tmp_path):
    root = Path(__file__).resolve().parents[1]
    sdk = Path(os.environ.get("IDF_PATH", root.parent / "esp/esp-idf"))
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        pytest.skip("ESP-IDF cJSON source is required for the cache GC harness")
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
    (tmp_path / "gc_types.hpp").write_text(types)
    storage = (main / "storage.cpp").read_text()
    extracted = "".join(
        function(storage, marker)
        for marker in (
            "static bool cache_pointer(",
            "static bool load_index(",
            "static bool cache_commit_failure(",
            "bool commit_cache(",
            "static bool mark_cache_links(",
            "void collect_cache(",
        )
    )
    # Redirect the real collector's filesystem root into this test's directory.
    # Its parsing, reachability, retirement and deletion logic remain unchanged.
    extracted = extracted.replace('opendir("/cache")', "opendir(cache_root.c_str())")
    extracted = extracted.replace('("/cache/" + name)', '(cache_root + "/" + name)')
    (tmp_path / "gc_under_test.hpp").write_text(extracted)
    obj = tmp_path / "cJSON.o"
    subprocess.run(
        [c_compiler, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)],
        check=True,
    )
    binary = tmp_path / "gc-test"
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
            str(root / "tests/firmware_gc.cpp"),
            str(obj),
            "-o",
            str(binary),
        ],
        check=True,
    )
    cache = tmp_path / "cache"
    cache.mkdir()
    subprocess.run([str(binary), str(cache)], check=True)
