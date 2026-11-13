#!/usr/bin/env python3
"""Extract the single inline script from index.html and syntax-check it with node.

This is deliberately dependency-free: it catches a broken WebUI before packaging,
without pulling a browser or a JavaScript toolchain into the plugin.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = re.compile(r"<script\b[^>]*>(.*?)</script\s*>", re.IGNORECASE | re.DOTALL)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("html", nargs="?", default="index.html", type=Path)
    args = parser.parse_args()

    text = args.html.read_text(encoding="utf-8")
    blocks = SCRIPT.findall(text)
    if len(blocks) != 1:
        sys.exit(f"{args.html}: expected exactly one inline script, found {len(blocks)}")
    if "<script" in blocks[0].lower():
        sys.exit(f"{args.html}: nested script tag in inline block")
    with tempfile.NamedTemporaryFile("w", suffix=".js", encoding="utf-8", delete=False) as tmp:
        tmp.write(blocks[0])
        name = tmp.name
    try:
        proc = subprocess.run(["node", "--check", name], capture_output=True, text=True)
    finally:
        Path(name).unlink(missing_ok=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        sys.exit(f"{args.html}: inline JavaScript failed node --check")
    print(f"{args.html}: inline JavaScript syntax OK ({len(blocks[0])} bytes)")


if __name__ == "__main__":
    main()
