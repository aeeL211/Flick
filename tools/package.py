#!/usr/bin/env python3
"""Pack a staged Flick module into a deterministic, installable zip.

The layout follows AxManager's own plugins/build.py: sorted files, no directory
entries, explicit Unix modes, LF line endings for text files, and a fixed zip
timestamp.  The same commit and staging tree therefore produce byte-identical
archives.

    python3 tools/package.py --root out --out dist

Outputs in --out:
    Flick-<version>.zip
    Flick-<version>.zip.sha256
    Flick-<version>.manifest.txt   (sha256, size, mode, path)

Set SOURCE_DATE_EPOCH to use a reproducible timestamp other than 1980-01-01.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import time
import zipfile
from pathlib import Path

ID_RULE = re.compile(r"^[a-zA-Z][a-zA-Z0-9._-]+$")
TEXT_SUFFIXES = {
    ".sh",
    ".prop",
    ".html",
    ".txt",
    ".md",
    ".json",
    ".xml",
    ".js",
    ".css",
}
BINARY_SUFFIXES = {
    ".png",
    ".jpg",
    ".jpeg",
    ".webp",
    ".gif",
    ".ico",
    ".so",
    ".zip",
    ".dex",
    ".jar",
}
# The daemon binaries are extensionless; never run the CRLF normaliser on them.
BINARY_PATHS = {"system/bin/flick", "system/bin/flick.arm", "system/bin/flick.arm64"}
EXPECTED_FILES = {
    "module.prop",
    "customize.sh",
    "ctl.sh",
    "hw.sh",
    "notify.sh",
    "uninstall.sh",
    "webroot/index.html",
    "system/bin/flick.arm",
    "system/bin/flick.arm64",
}


def read_prop(path: Path) -> dict[str, str]:
    props: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        props[key.strip()] = value.strip()
    return props


def check_prop(props: dict[str, str]) -> None:
    problems: list[str] = []
    if not ID_RULE.fullmatch(props.get("id", "")):
        problems.append(f"invalid id {props.get('id')!r}")
    if not props.get("versionCode", "").isdigit():
        problems.append("versionCode must be an integer")
    if not props.get("axeronPlugin", "").isdigit():
        problems.append("axeronPlugin must be a number")
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", props.get("version", "")):
        problems.append("version must look like v1.2.3")
    if problems:
        sys.exit("module.prop: " + "; ".join(problems))


def zip_timestamp() -> tuple[int, int, int, int, int, int]:
    epoch = os.environ.get("SOURCE_DATE_EPOCH", "").strip()
    if not epoch:
        return (1980, 1, 1, 0, 0, 0)
    try:
        stamp = time.gmtime(int(epoch))
    except (ValueError, OverflowError) as exc:
        sys.exit(f"SOURCE_DATE_EPOCH is invalid: {exc}")
    if stamp.tm_year < 1980:
        return (1980, 1, 1, 0, 0, 0)
    return (stamp.tm_year, stamp.tm_mon, stamp.tm_mday, stamp.tm_hour, stamp.tm_min, stamp.tm_sec)


def mode_for(name: str) -> int:
    if name.startswith("system/bin/"):
        return 0o755
    if name.endswith(".sh"):
        return 0o755
    return 0o644


def is_text(name: str) -> bool:
    if name in BINARY_PATHS:
        return False
    suffix = Path(name).suffix.lower()
    if suffix in BINARY_SUFFIXES:
        return False
    return suffix in TEXT_SUFFIXES or suffix == ""


def collect(root: Path) -> list[Path]:
    files = sorted((p for p in root.rglob("*") if p.is_file()), key=lambda p: p.relative_to(root).as_posix())
    names = {p.relative_to(root).as_posix() for p in files}
    missing = EXPECTED_FILES - names
    extra = names - EXPECTED_FILES
    if missing:
        sys.exit("staging is missing: " + ", ".join(sorted(missing)))
    if extra:
        sys.exit("staging has unexpected files: " + ", ".join(sorted(extra)))
    return files


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--root", required=True, type=Path, help="staged module tree")
    parser.add_argument("--out", required=True, type=Path, help="output directory")
    parser.add_argument("--name", default="Flick", help="archive base name (default: Flick)")
    args = parser.parse_args()

    props = read_prop(args.root / "module.prop")
    check_prop(props)
    version = props["version"]
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / f"{args.name}-{version}.zip"
    manifest = args.out / f"{args.name}-{version}.manifest.txt"
    checksum = args.out / f"{args.name}-{version}.zip.sha256"

    files = collect(args.root)
    stamp = zip_timestamp()
    lines: list[str] = []
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in files:
            name = path.relative_to(args.root).as_posix()
            data = path.read_bytes()
            if is_text(name):
                data = data.replace(b"\r\n", b"\n")
            info = zipfile.ZipInfo(name, date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # Unix, so external_attr is interpreted as a mode
            info.external_attr = mode_for(name) << 16
            zf.writestr(info, data)
            lines.append(f"{hashlib.sha256(data).hexdigest()}  {len(data):8d}  {mode_for(name):04o}  {name}")

    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    checksum.write_text(f"{digest}  {target.name}\n", encoding="utf-8")
    print(f"{target}  {target.stat().st_size} bytes")
    print(f"{target.name}: {digest}")


if __name__ == "__main__":
    main()
