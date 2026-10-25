#!/usr/bin/env python3
"""Build pinned native mono fonts and whole-sequence Emoji assets, offline after fetch."""

import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
import urllib.request
from pathlib import Path

import fontTools
import uharfbuzz as hb
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument(
        "--converter",
        type=Path,
        required=True,
        help="lv_font_conv@1.5.3 package directory",
    )
    parser.add_argument("--node", default="node")
    parser.add_argument("--output", type=Path, default=HERE)
    parser.add_argument("--fetch", action="store_true")
    args = parser.parse_args()
    assert fontTools.__version__ == "4.60.1" and hb.__version__ == "0.51.0"
    sources = json.loads((HERE / "sources.lock.json").read_text())
    args.source_dir.mkdir(parents=True, exist_ok=True)
    for item in sources:
        path = args.source_dir / item["name"]
        if not path.exists() and args.fetch:
            with urllib.request.urlopen(item["url"], timeout=45) as response:
                data = response.read(32 * 1024 * 1024)
            assert digest(data) == item["sha256"], item["name"]
            path.write_bytes(data)
        assert path.is_file() and digest(path.read_bytes()) == item["sha256"], item[
            "name"
        ]
    package = json.loads((args.converter / "package.json").read_text())
    assert package["name"] == "lv_font_conv" and package["version"] == "1.5.3"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    chars = {chr(cp) for cp in range(32, 127)}
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                chars.add(bytes([high, low]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    chars.update("•…—–─│")
    symbols = "".join(sorted(chars))
    emoji_raw = (args.source_dir / "NotoEmoji.ttf").read_bytes()
    shaper = hb.Font(hb.Face(emoji_raw))
    hb.ot_font_set_funcs(shaper)
    ttf = TTFont(args.source_dir / "NotoEmoji.ttf", recalcTimestamp=False)
    names = ttf.getGlyphOrder()
    glyphs, sequences = {}, {}
    unsupported = 0
    for line in (args.source_dir / "emoji-test.txt").read_text().splitlines():
        if ";" not in line or line.startswith("#"):
            continue
        codes, rest = line.split(";", 1)
        kind = rest.split("#", 1)[0].strip()
        if kind not in ("fully-qualified", "minimally-qualified", "unqualified"):
            continue
        text = "".join(chr(int(cp, 16)) for cp in codes.split())
        buffer = hb.Buffer()
        buffer.flags = hb.BufferFlags.REMOVE_DEFAULT_IGNORABLES
        buffer.add_str(text)
        buffer.guess_segment_properties()
        hb.shape(shaper, buffer)
        ids = [info.codepoint for info in buffer.glyph_infos]
        if len(ids) != 1 or ids[0] == 0:
            unsupported += kind == "fully-qualified"
            continue
        gid = ids[0]
        if gid not in glyphs:
            glyphs[gid] = 0xE000 + len(glyphs)
        sequences[text] = glyphs[gid]
    assert len(glyphs) < 0x1900
    for table in ttf["cmap"].tables:
        if table.isUnicode():
            table.cmap.update({code: names[gid] for gid, code in glyphs.items()})
    with tempfile.TemporaryDirectory(prefix="readocs-fonts-") as temp:
        mapped = Path(temp) / "emoji.ttf"
        ttf.save(mapped)
        for family, source, charset in (
            ("noto_sc", args.source_dir / "NotoSansSC-Regular.otf", symbols),
            (
                "noto_emoji",
                mapped,
                "".join(chr(code) for code in sorted(glyphs.values())),
            ),
        ):
            for size in (16, 24, 32):
                subprocess.run(
                    [
                        args.node,
                        str(args.converter / "lv_font_conv.js"),
                        "--font",
                        str(source),
                        "--symbols",
                        charset,
                        "--size",
                        str(size),
                        "--bpp",
                        "1",
                        "--format",
                        "bin",
                        "--no-kerning",
                        "-o",
                        str(output / f"{family}_{size}.bff"),
                    ],
                    check=True,
                    timeout=60,
                )
    strings, entries = bytearray(), bytearray()
    for text, code in sorted(sequences.items(), key=lambda item: item[0].encode()):
        value = text.encode()
        entries += struct.pack("<IHH", len(strings), len(value), code)
        strings += value
    mapping = (
        b"RDEM"
        + struct.pack("<III", len(sequences), 16 + len(entries), 0)
        + entries
        + strings
    )
    (output / "emoji.emap").write_bytes(mapping)
    files = {
        path.name: {"bytes": path.stat().st_size, "sha256": digest(path.read_bytes())}
        for path in sorted(output.iterdir())
        if path.suffix in (".bff", ".emap")
    }
    revision = digest(json.dumps(files, sort_keys=True).encode())
    (output / "revision.hpp").write_text(
        "#pragma once\n#include <cstdint>\nnamespace rd {\n"
        f"inline constexpr uint32_t font_asset_revision = 0x{int(revision[:8], 16) & 0x7FFFFFFF:08x};\n"
        f"inline constexpr unsigned emoji_sequence_count = {len(sequences)};\n}}\n"
    )
    manifest = {
        "family": "Noto Sans SC Regular",
        "sizes_px": [16, 24, 32],
        "bpp": 1,
        "chinese_charset_count": len(symbols),
        "charset_sha256": digest(symbols.encode()),
        "emoji_family": "Noto Emoji monochrome",
        "emoji_sequences": len(sequences),
        "emoji_designs": len(glyphs),
        "emoji_data_version": "17.0",
        "unsupported_fully_qualified": unsupported,
        "revision": revision,
        "files": files,
        "tools": {
            "lv_font_conv": "1.5.3",
            "fonttools": "4.60.1",
            "uharfbuzz": "0.51.0",
        },
        "source_files": sources,
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    )
    print(
        json.dumps(
            {
                "revision": revision[:8],
                "resource_bytes": sum(info["bytes"] for info in files.values()),
                "emoji_sequences": len(sequences),
                "unsupported_fully_qualified": unsupported,
            }
        )
    )


if __name__ == "__main__":
    main()
