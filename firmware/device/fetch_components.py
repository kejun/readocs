#!/usr/bin/env python3
"""Reuse the same verified driver revisions as the hardware diagnostic project."""

import runpy
from pathlib import Path

runpy.run_path(
    str(Path(__file__).resolve().parents[1] / "diagnostics" / "fetch_components.py"),
    run_name="__main__",
)
