import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "firmware/device/fonts"


def prepare_font_test(tmp_path):
    gfx = ROOT / "firmware/diagnostics/components/M5GFX/src/lgfx"
    if not (gfx / "v1/lgfx_fonts.cpp").is_file():
        pytest.skip("Pinned M5GFX sources are required")
    source = (gfx / "v1/lgfx_fonts.cpp").read_text()
    impl = (
        "  static size_t draw_alpha_bitmap_common("
        + source.split("  static size_t draw_alpha_bitmap_common(", 1)[1].split(
            "  void VLWfont::getDefaultMetric", 1
        )[0]
    )
    impl = impl.replace(
        "bool BFFfont::updateFontMetric(FontMetrics *metrics, uint16_t uniCode) const\n  {",
        "bool BFFfont::updateFontMetric(FontMetrics *metrics, uint16_t uniCode) const\n  {\n    ++font_metric_calls;",
    )
    (tmp_path / "bff_under_test.hpp").write_text(impl)
    return gfx


def test_font_resources_match_manifest():
    manifest = json.loads((FONTS / "manifest.json").read_text())
    for name, info in manifest["files"].items():
        data = (FONTS / name).read_bytes()
        assert len(data) == info["bytes"]
        assert hashlib.sha256(data).hexdigest() == info["sha256"]
    revision = (FONTS / "revision.hpp").read_text()
    assert int(re.search(r"0x([a-f0-9]+)", revision)[1], 16) == (
        int(manifest["revision"][:8], 16) & 0x7FFFFFFF
    )


def test_actual_font_decoder_shaping_and_pixels(tmp_path):
    gfx = prepare_font_test(tmp_path)
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler
    binary = tmp_path / "fonts"
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "firmware/device/main"),
            "-I",
            str(ROOT / "tests"),
            "-isystem",
            str(gfx.parent),
            "-I",
            str(tmp_path),
            str(ROOT / "tests/fonts.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary), str(FONTS)], check=True, timeout=30)
