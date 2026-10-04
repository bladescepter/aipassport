#!/usr/bin/env python3
"""Generate a fresh, bounded static device catalog. Does not deploy or alter local firmware catalog.

Audio stays private: use an ignored build/ output or a directory outside Git.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGE_SIZE = 20
MAX_TRACKS = 1000
JSON_LIMIT = 16384


def packet_samples(packet: bytes) -> int:
    toc = packet[0]
    if toc & 4:  # Stereo is not the device's mono source contract.
        return 0
    if toc & 0x80:
        frame = (16000 << ((toc >> 3) & 3)) // 400
    elif toc & 0x60 == 0x60:
        frame = 320 if toc & 8 else 160
    else:
        size = (toc >> 3) & 3
        frame = 960 if size == 3 else (16000 << size) // 100
    code = toc & 3
    count = 1 if code == 0 else 2 if code in (1, 2) else (packet[1] & 63 if len(packet) > 1 else 0)
    return frame * count


def inspect_audio(path: Path) -> tuple[str, int, int]:
    digest = hashlib.sha256()
    frames = size = 0
    with path.open('rb') as stream:
        while prefix := stream.read(2):
            if len(prefix) != 2:
                raise ValueError(f'{path.name}: truncated prefix')
            length = int.from_bytes(prefix, 'little')
            if not 1 <= length <= 1500:
                raise ValueError(f'{path.name}: invalid frame length')
            packet = stream.read(length)
            if len(packet) != length or packet_samples(packet) != 320:
                raise ValueError(f'{path.name}: truncated or non-20ms packet')
            digest.update(prefix + packet)
            frames += 1
            size += 2 + length
    if not 0 < size <= 64 * 1024 * 1024:
        raise ValueError(f'{path.name}: empty/oversized audio')
    return digest.hexdigest(), size, frames * 20


def identifier(value: str, maximum: int) -> bool:
    return isinstance(value, str) and 1 <= len(value.encode()) <= maximum and bool(re.fullmatch(r'[A-Za-z0-9_-]+', value))


def document(value: dict) -> bytes:
    body = (json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n').encode()
    if len(body) > JSON_LIMIT:
        raise ValueError('JSON exceeds device 16 KiB limit')
    return body


def publish(catalog: Path, audio: Path, output: Path, release: str, characters: Path) -> int:
    if not identifier(release, 47):
        raise ValueError('Invalid release ID')
    resolved = output.resolve()
    if resolved.is_relative_to(ROOT) and not resolved.is_relative_to(ROOT / 'build'):
        raise ValueError('Private audio output inside this project must be under ignored build/')
    if output.exists():
        raise ValueError('Output must not exist: never overwrite an earlier release')
    rows = json.loads(catalog.read_text(encoding='utf-8'))
    if not isinstance(rows, list) or len(rows) > MAX_TRACKS:
        raise ValueError('Catalog must be an array with at most 1000 entries')
    supported = set(json.loads(characters.read_text(encoding='utf-8'))['characters'])
    tracks, sources, ids = [], {}, set()
    for row in rows:
        id_ = row['id']
        title = row['title']
        name = row['path']
        if not identifier(id_, 39) or id_ in ids:
            raise ValueError('Invalid or duplicate track ID')
        ids.add(id_)
        if not isinstance(title, str) or not 1 <= len(title.encode()) <= 95 or any(ord(c) < 32 or ord(c) == 127 for c in title):
            raise ValueError(f'{id_}: invalid title')
        missing = set(title) - supported
        if missing:
            raise ValueError(f'{id_}: unsupported display characters: {"".join(sorted(missing))}')
        if not isinstance(name, str) or Path(name).name != name or name in ('.', '..'):
            raise ValueError(f'{id_}: invalid local source path')
        source = audio / name
        if source.is_symlink():
            raise ValueError(f'{id_}: symlink source forbidden')
        hash_, size, duration = inspect_audio(source)
        sources[hash_] = source
        tracks.append({'id': id_, 'title': title, 'duration_ms': duration,
                       'size_bytes': size, 'sha256': hash_, 'audio_path': f'/v1/audio/{hash_}.opus'})
    pages = (len(tracks) + PAGE_SIZE - 1) // PAGE_SIZE
    docs = {
        'v1/current.json': {'schema_version': 1, 'release_id': release,
                            'manifest_path': f'/v1/releases/{release}/manifest.json'},
        f'v1/releases/{release}/manifest.json': {
            'schema_version': 1, 'release_id': release, 'total_tracks': len(tracks),
            'page_size': PAGE_SIZE, 'total_pages': pages,
            'audio_format': {'codec': 'opus', 'container': 'length-prefixed-le16',
                             'sample_rate': 16000, 'channels': 1, 'bitrate': 32000, 'frame_ms': 20}},
    }
    for page in range(pages):
        docs[f'v1/releases/{release}/tracks/all/{page:04d}.json'] = {
            'schema_version': 1, 'release_id': release, 'page': page, 'total_pages': pages,
            'tracks': tracks[page * PAGE_SIZE:(page + 1) * PAGE_SIZE]}
    bodies = {name: document(value) for name, value in docs.items()}
    output.mkdir(parents=True)
    for hash_, source in sources.items():
        destination = output / f'v1/audio/{hash_}.opus'
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        if inspect_audio(destination)[0] != hash_:
            raise ValueError('Source changed during publication; output must not be deployed')
    # current is written last; remote upload/atomic switch remains a separate controlled action.
    for name in sorted(bodies, key=lambda name: name == 'v1/current.json'):
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(bodies[name])
    return len(tracks)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--catalog', type=Path, default=ROOT / 'assets/music/catalog.json')
    parser.add_argument('--audio', type=Path, default=ROOT / 'assets/music/data')
    parser.add_argument('--characters', type=Path, default=ROOT / 'assets/fonts/supported_characters.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--release', required=True)
    args = parser.parse_args()
    try:
        count = publish(args.catalog, args.audio, args.output, args.release, args.characters)
    except (ValueError, KeyError, OSError, TypeError) as error:
        parser.exit(1, f'Publication failed: {error}\n')
    print(f'{count} tracks published locally to {args.output}; no server changes made')


if __name__ == '__main__':
    main()
