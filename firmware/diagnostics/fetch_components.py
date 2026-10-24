#!/usr/bin/env python3
"""Fetch the PaperDIY factory driver's pinned versions without duplicate managed M5GFX."""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
LOCK = json.loads((ROOT / "components.lock.json").read_text())
MANAGED_DEP = 'dependencies:\n  m5stack/m5gfx:\n    version: ">=0.2.26"\n'

for repo in LOCK["components"]:
    dest = ROOT / "components" / repo["name"]
    if not dest.exists():
        dest.mkdir(parents=True)
        subprocess.run(["git", "init", str(dest)], check=True)
        subprocess.run(["git", "-C", str(dest), "remote", "add", "origin", repo["url"]], check=True)
        subprocess.run(["git", "-C", str(dest), "fetch", "--depth=1", "origin", repo["commit"]], check=True)
        subprocess.run(["git", "-C", str(dest), "checkout", "--detach", "FETCH_HEAD"], check=True)
    actual = subprocess.check_output(["git", "-C", str(dest), "rev-parse", "HEAD"], text=True).strip()
    if actual != repo["commit"]:
        raise SystemExit(f"{dest}: unexpected revision {actual}; existing files were preserved")
    if repo["name"] == "M5Unified":
        manifest = dest / "idf_component.yml"
        text = manifest.read_text()
        if MANAGED_DEP in text:
            manifest.write_text(text.replace(MANAGED_DEP, ""))
        elif "dependencies:" in text or "m5stack/m5gfx" in text:
            raise SystemExit(f"{manifest}: unexpected dependency declaration; review manually")
    print(f"{repo['name']}: {actual}")
