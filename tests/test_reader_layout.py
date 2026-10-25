import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest


def function(source, marker):
    tail = source.split(marker, 1)[1]
    # These source files use two-space indentation for font implementation.
    end = "\n  }\n" if marker.startswith(("  ", "int32_t LGFX")) else "\n}\n"
    return marker + tail.split(end, 1)[0] + end


def test_actual_layout_matches_m5gfx_font_metrics(tmp_path):
    root = Path(__file__).resolve().parents[1]
    main = root / "firmware/device/main"
    gfx = root / "firmware/diagnostics/components/M5GFX/src/lgfx"
    sdk = Path(os.environ.get("IDF_PATH", root.parent / "esp/esp-idf"))
    cjson = sdk / "components/json/cJSON"
    if not (gfx / "v1/lgfx_fonts.cpp").is_file() or not (cjson / "cJSON.c").is_file():
        pytest.skip("Local M5GFX font and ESP-IDF cJSON sources are required")
    compiler = shutil.which("clang++") or shutil.which("g++")
    c_compiler = shutil.which("clang") or shutil.which("gcc")
    assert compiler and c_compiler
    fonts = (gfx / "v1/lgfx_fonts.cpp").read_text()
    font_impl = fonts.split("  struct u8g2_font_decode_t", 1)[1].split(
        "  size_t U8g2font::drawChar", 1
    )[0]
    font_impl = "  struct u8g2_font_decode_t" + font_impl
    font_impl = font_impl.replace(
        "bool U8g2font::updateFontMetric(lgfx::FontMetrics *metrics, uint16_t uniCode) const\n  {",
        "bool U8g2font::updateFontMetric(lgfx::FontMetrics *metrics, uint16_t uniCode) const\n  {\n    ++font_metric_calls;",
    )
    (tmp_path / "font_under_test.hpp").write_text(font_impl)
    data = (gfx / "Fonts/efont/lgfx_efont_cn.c").read_text()
    arrays = []
    for name in ("16", "24", "24_b"):
        match = re.search(
            rf"PROGMEM const uint8_t lgfx_efont_cn_{name}\[\d+\] =.*?;\n",
            data,
            re.DOTALL,
        )
        assert match
        arrays.append(match.group().replace("PROGMEM ", ""))
    (tmp_path / "font_data.c").write_text("#include <stdint.h>\n" + "\n".join(arrays))
    lgfx = (gfx / "v1/LGFXBase.cpp").read_text()
    width = function(lgfx, "int32_t LGFXBase::text_width(").replace(
        "LGFXBase::text_width(", "text_width("
    )
    decode = function(lgfx, "  uint32_t LGFXBase::decodeUTF8(").replace(
        "LGFXBase::decodeUTF8(", "decodeUTF8("
    )
    (tmp_path / "width_reference.hpp").write_text(decode + width)
    reader = (main / "reader.cpp").read_text()
    helpers = reader.split("static void font(", 1)[1].split(
        "std::vector<Page> layout(", 1
    )[0]
    layout = function(reader, "std::vector<Page> layout(")
    reference = layout.replace("layout(", "layout_reference(", 1).replace(
        "next_width.width() > 476 - indent",
        "M5.Display.textWidth((line + cluster).c_str()) > 476 - indent",
    )
    (tmp_path / "layout_under_test.hpp").write_text(
        "static void font(" + helpers + layout + reference
    )
    objects = []
    for source in (cjson / "cJSON.c", tmp_path / "font_data.c"):
        obj = tmp_path / (source.stem + ".o")
        subprocess.run(
            [c_compiler, "-std=c99", "-c", str(source), "-o", str(obj)], check=True
        )
        objects.append(str(obj))
    binary = tmp_path / "layout-test"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(main),
            "-I",
            str(gfx / "v1"),
            "-I",
            str(cjson),
            "-I",
            str(tmp_path),
            str(root / "tests/reader_layout.cpp"),
            *objects,
            "-o",
            str(binary),
        ],
        check=True,
    )
    fixture = os.environ.get("READOCS_LAYOUT_FIXTURE")
    subprocess.run([str(binary), *([fixture] if fixture else [])], check=True)
