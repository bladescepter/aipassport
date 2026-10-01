#!/usr/bin/env python3
"""Encode the supplied songs into the firmware's streamable Opus format.

Input files are intentionally kept outside the SPIFFS image.  For every catalog
entry, place one source file in assets/music/source/<id>.<extension>, then run:

    python3 tools/encode_music.py

The output is assets/music/data/<path>.  It is *not* an Ogg container: it is a
sequence of ``uint16_le packet_length + Opus packet`` records.  This lets the
ESP32-C3 decode one frame at a time without an Ogg demuxer or a whole-song
buffer.  The script also regenerates main/music_catalog.h with measured
 durations and checks the raw payload against the musicfs partition.
"""
from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "assets/music/catalog.json"
SOURCE_DIR = ROOT / "assets/music/source"
DATA_DIR = ROOT / "assets/music/data"
HEADER_PATH = ROOT / "main/music_catalog.h"
PARTITIONS_PATH = ROOT / "partitions.csv"

AUDIO_EXTENSIONS = (".wav", ".mp3", ".ogg", ".opus", ".flac", ".m4a")
SAMPLE_RATE = 16_000
CHANNELS = 1
BITRATE = 32
MAX_PACKET = 1500


def find_tool(name: str) -> str:
    tool = shutil.which(name)
    if not tool:
        raise SystemExit(f"未找到 {name}，请先安装 ffmpeg/ffprobe")
    return tool


def read_partition_size() -> int:
    for line in PARTITIONS_PATH.read_text(encoding="utf-8").splitlines():
        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[0] == "musicfs":
            return int(fields[4], 16)
    raise SystemExit("partitions.csv 中没有 musicfs 分区")


def find_source(entry: dict) -> Optional[Path]:
    candidates = []
    stem = Path(entry["path"]).stem
    for name in (entry["id"], stem, entry["title"]):
        for extension in AUDIO_EXTENSIONS:
            candidate = SOURCE_DIR / f"{name}{extension}"
            if candidate.is_file() and candidate not in candidates:
                candidates.append(candidate)

    # Also accept a descriptive filename such as
    # "歌唱祖国-中国广播艺术团合唱.mp3".  This is useful while adding songs
    # incrementally; exact numeric names remain the reproducible convention.
    for candidate in SOURCE_DIR.iterdir():
        if (candidate.is_file() and candidate.suffix.lower() in AUDIO_EXTENSIONS
                and entry["title"] in candidate.stem and candidate not in candidates):
            candidates.append(candidate)

    if len(candidates) > 1:
        raise SystemExit(f"歌曲 {entry['id']} 有多个候选源文件: {candidates}")
    return candidates[0] if candidates else None


def probe_duration(ffprobe: str, source: Path) -> int:
    result = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", str(source)],
        check=False, capture_output=True, text=True,
    )
    try:
        return max(0, round(float(result.stdout.strip()) * 1000))
    except (TypeError, ValueError):
        return 0


def ogg_packets(path: Path) -> list[bytes]:
    """Read Ogg pages while retaining packets split across page boundaries."""
    data = path.read_bytes()
    offset = 0
    pending = bytearray()
    packets: list[bytes] = []
    while offset < len(data):
        if offset + 27 > len(data) or data[offset:offset + 4] != b"OggS":
            raise RuntimeError(f"不是有效的 Ogg 页面: {path} @ {offset}")
        segment_count = data[offset + 26]
        table_end = offset + 27 + segment_count
        if table_end > len(data):
            raise RuntimeError(f"Ogg 页面截断: {path}")
        lacing = data[offset + 27:table_end]
        body_size = sum(lacing)
        body_end = table_end + body_size
        if body_end > len(data):
            raise RuntimeError(f"Ogg 页面数据截断: {path}")
        body = data[table_end:body_end]
        body_offset = 0
        for segment_length in lacing:
            pending.extend(body[body_offset:body_offset + segment_length])
            body_offset += segment_length
            if segment_length < 255:
                packets.append(bytes(pending))
                pending.clear()
        offset = body_end
    if pending:
        raise RuntimeError(f"Ogg 末尾有未完成的 Opus 包: {path}")
    return packets


def convert_to_raw_opus(ffmpeg: str, source: Path, output: Path) -> int:
    with tempfile.TemporaryDirectory(prefix="music-opus-") as temporary:
        ogg = Path(temporary) / "encoded.opus"
        command = [
            ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-i", str(source), "-map", "0:a:0", "-vn", "-sn", "-dn",
            "-c:a", "libopus", "-b:a", f"{BITRATE}k",
            "-ar", str(SAMPLE_RATE), "-ac", str(CHANNELS),
            "-application", "audio", "-frame_duration", "20",
            "-vbr", "off", "-f", "ogg", str(ogg),
        ]
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        if result.returncode != 0 or not ogg.is_file():
            raise RuntimeError(f"ffmpeg 编码失败 {source}: {result.stderr[-800:]}")

        packets = ogg_packets(ogg)
        audio_packets = [
            packet for packet in packets
            if packet and not packet.startswith(b"OpusHead")
            and not packet.startswith(b"OpusTags")
        ]
        if not audio_packets:
            raise RuntimeError(f"没有找到音频 Opus 包: {source}")
        if any(len(packet) > MAX_PACKET for packet in audio_packets):
            raise RuntimeError(f"Opus 包超过 {MAX_PACKET} 字节: {source}")

        payload = b"".join(struct.pack("<H", len(packet)) + packet
                            for packet in audio_packets)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(payload)
        return len(payload)


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def write_catalog_header(entries: list[dict]) -> None:
    lines = [
        "// Generated from assets/music/catalog.json by tools/encode_music.py.",
        "#pragma once", "", "#include <stddef.h>", "#include <stdint.h>", "",
        "typedef struct {",
        "    const char *id;", "    const char *title;", "    const char *path;",
        "    uint32_t duration_ms;", "    uint16_t order;", "} music_track_t;", "",
        "static const music_track_t MUSIC_CATALOG[] = {",
    ]
    for entry in entries:
        lines.append(
            f"    {{{c_string(entry['id'])}, {c_string(entry['title'])}, "
            f"{c_string(entry['path'])}, {int(entry.get('duration_ms', 0))}u, "
            f"{int(entry.get('order', 0))}u}},"
        )
    lines += [
        "};", "", "#define MUSIC_CATALOG_COUNT "
        "(sizeof(MUSIC_CATALOG) / sizeof(MUSIC_CATALOG[0]))", "",
    ]
    HEADER_PATH.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bitrate", type=int, default=BITRATE,
                        help="仅用于提示；固件当前按 32 kbps 资源预算")
    parser.add_argument(
        "--only", nargs="+", metavar="ID",
        help="只编码指定歌曲，例如 --only 01；未指定时处理整个目录")
    parser.add_argument(
        "--allow-missing", action="store_true",
        help="允许目录中尚未提供的歌曲，适合分批添加资源")
    args = parser.parse_args()
    if args.bitrate != BITRATE:
        raise SystemExit("固件当前固定使用 32 kbps；修改编码器和容量预算后再改变该值")

    ffmpeg = find_tool("ffmpeg")
    ffprobe = find_tool("ffprobe")
    entries = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    if not entries:
        raise SystemExit("catalog.json 为空")

    selected_ids = set(args.only or [])
    unknown_ids = selected_ids - {entry["id"] for entry in entries}
    if unknown_ids:
        raise SystemExit(f"catalog.json 中不存在歌曲 ID: {sorted(unknown_ids)}")
    work_entries = [entry for entry in entries
                    if not selected_ids or entry["id"] in selected_ids]

    partition_size = read_partition_size()
    total_bytes = 0
    encoded_count = 0
    for entry in work_entries:
        source = find_source(entry)
        if source is None:
            if args.allow_missing and not selected_ids:
                print(f"跳过 {entry['id']}（尚未提供源文件）")
                continue
            raise SystemExit(
                f"缺少歌曲 {entry['id']}：请放入 {SOURCE_DIR}/{entry['id']}.wav|mp3|..."
            )
        output = DATA_DIR / entry["path"]
        size = convert_to_raw_opus(ffmpeg, source, output)
        entry["duration_ms"] = probe_duration(ffprobe, source)
        total_bytes += size
        encoded_count += 1
        print(f"{entry['id']}: {source.name} -> {output.name}, "
              f"{entry['duration_ms'] / 1000:.1f}s, {size} bytes")

    if encoded_count == 0:
        raise SystemExit("没有可编码的歌曲源文件")

    if total_bytes >= partition_size:
        raise SystemExit(
            f"音频资源超出 musicfs：{total_bytes} bytes >= {partition_size} bytes；"
            "请降低码率/缩短歌曲或调整分区并重新审查固件容量"
        )

    CATALOG_PATH.write_text(json.dumps(entries, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")
    write_catalog_header(entries)
    print(f"本次编码 {encoded_count} 首；音频裸流合计 {total_bytes} bytes，"
          f"musicfs 原始余量 {partition_size - total_bytes} bytes")
    print("下一步运行: python3 tools/pack_music.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
