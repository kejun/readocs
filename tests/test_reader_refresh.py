import os
import shutil
import subprocess
from pathlib import Path

import pytest


def test_actual_reader_keeps_fast_partial_refresh(tmp_path):
    root = Path(__file__).resolve().parents[1]
    main = root / "firmware/device/main"
    sdk = Path(os.environ.get("IDF_PATH", root.parent / "esp/esp-idf"))
    cjson = sdk / "components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        pytest.skip("ESP-IDF cJSON source is required")
    declarations = (main / "readocs.hpp").read_text()
    types = (
        "struct JsonDelete {"
        + declarations.split("struct JsonDelete {", 1)[1].split("struct Profile", 1)[0]
    )
    for name in ("Entry", "Source", "Line", "Page", "Position"):
        types += (
            "struct "
            + name
            + " {"
            + declarations.split("struct " + name + " {", 1)[1].split("\n};", 1)[0]
            + "\n};\n"
        )
    (tmp_path / "refresh_types.hpp").write_text("namespace rd {\n" + types + "}\n")
    reader = (main / "reader.cpp").read_text()
    globals_ = (
        "static constexpr auto kFullMode"
        + reader.split("static constexpr auto kFullMode", 1)[1].split(
            "static const char *epd_mode_name", 1
        )[0]
    )
    functions = []
    for marker in (
        "static const char *epd_mode_name(",
        "static void font(",
        "static void begin(",
        "static void finish(",
        "static void finish_partial(",
        "bool quality_cleanup_due(",
        "void request_quality_cleanup(",
        "int display_partial_count(",
        "int display_partial_limit(",
        "void set_display_partial_limit(",
        "void draw_page(",
        "static std::vector<std::string> article_title(",
        "static std::string article_signature_of(",
        "static std::string menu_signature_of(",
        "static void draw_article_row(",
        "static void draw_menu_row(",
        "static std::string fit_header(",
        "static void draw_source_row(",
        "void draw_sources(",
        "void draw_articles(",
        "void draw_menu(",
    ):
        tail = reader.split(marker, 1)[1]
        # Two counter getters are deliberately single-line definitions.
        end = (
            tail.index("\n")
            if tail.split("\n", 1)[0].endswith("}")
            else tail.index("\n}\n") + 2
        )
        functions.append(marker + tail[:end] + "\n")
    (tmp_path / "refresh_under_test.hpp").write_text(globals_ + "\n".join(functions))
    cc = shutil.which("clang") or shutil.which("gcc")
    cxx = shutil.which("clang++") or shutil.which("g++")
    assert cc and cxx
    obj = tmp_path / "cjson.o"
    subprocess.run(
        [cc, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True
    )
    binary = tmp_path / "reader-refresh"
    subprocess.run(
        [
            cxx,
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
            str(root / "tests/reader_refresh.cpp"),
            str(obj),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
