#!/usr/bin/env python3
"""Pack assets/music/data into the musicfs SPIFFS image.

This step needs only ESP-IDF's spiffsgen.py and is therefore suitable for CI
and for rebuilding the data image after the already-encoded .opus files have
been supplied.  The output image is generated, not source media.
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = ROOT / "assets/music/data"
PARTITIONS = ROOT / "partitions.csv"
OUTPUT = ROOT / "assets/music/musicfs.img"


def partition() -> tuple[int, int]:
    for line in PARTITIONS.read_text(encoding="utf-8").splitlines():
        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[0] == "musicfs":
            return int(fields[3], 16), int(fields[4], 16)
    raise SystemExit("partitions.csv 中没有 musicfs 分区")


def find_spiffsgen() -> str:
    idf_path = os.environ.get("IDF_PATH")
    candidates = []
    if idf_path:
        candidates.append(Path(idf_path) / "components/spiffs/spiffsgen.py")
    candidates += [Path(p) for p in glob.glob("/opt/esp/*/components/spiffs/spiffsgen.py")]
    candidates += [Path(p) for p in glob.glob("/root/esp/*/components/spiffs/spiffsgen.py")]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    on_path = shutil.which("spiffsgen.py")
    if on_path:
        return on_path
    raise SystemExit("未找到 spiffsgen.py，请先 source ESP-IDF 5.5.3 的 export.sh")


def main() -> int:
    offset, size = partition()
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    # Check the source payload before invoking spiffsgen so the error is clear.
    payload = sum(path.stat().st_size for path in DATA_DIR.rglob("*") if path.is_file())
    if payload >= size:
        raise SystemExit(f"musicfs 音频文件合计 {payload} bytes >= 分区 {size} bytes")

    spiffsgen = find_spiffsgen()
    with tempfile.TemporaryDirectory(prefix="musicfs-") as temporary:
        image_root = Path(temporary)
        for child in DATA_DIR.iterdir():
            if child.name == ".gitkeep":
                continue
            destination = image_root / child.name
            if child.is_dir():
                shutil.copytree(child, destination)
            else:
                shutil.copy2(child, destination)
        result = subprocess.run(
            [sys.executable, spiffsgen, str(size), str(image_root), str(OUTPUT)],
            check=False, capture_output=True, text=True, timeout=300,
        )
        if result.returncode != 0:
            print(result.stdout, file=sys.stderr)
            print(result.stderr, file=sys.stderr)
            return result.returncode

    actual = OUTPUT.stat().st_size
    if actual != size:
        raise SystemExit(f"spiffsgen 输出大小异常: {actual}, 期望 {size}")
    print(f"{OUTPUT}: offset=0x{offset:x}, size=0x{size:x}, payload={payload} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
