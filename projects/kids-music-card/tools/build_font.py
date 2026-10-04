#!/usr/bin/env python3
"""Rebuild the licensed UI font and a server-side supported-character report.

Run from the project with a Python environment containing fonttools==4.59.0.
Install the locked converter with npm ci --prefix tools/font first.
"""
from __future__ import annotations
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--font', type=Path, required=True, help='Noto Sans CJK SC OTF or TTC')
    args = parser.parse_args()
    from fontTools.ttLib import TTCollection
    output = ROOT / 'build' / 'font'
    output.mkdir(parents=True, exist_ok=True)
    font = args.font.resolve()
    if font.suffix.lower() == '.ttc':
        collection = TTCollection(font)
        matches = [f for f in collection.fonts if any(
            n.nameID in (1, 16) and n.toUnicode() == 'Noto Sans CJK SC'
            for n in f['name'].names)]
        if len(matches) != 1:
            raise SystemExit('Expected exactly one Noto Sans CJK SC face')
        font = output / 'NotoSansCJKsc-Regular.otf'
        matches[0].save(font)
        collection.close()
    texts = [p.read_text(encoding='utf-8') for p in (ROOT / 'main').glob('*.c')]
    texts.append((ROOT / 'assets/music/catalog.json').read_text(encoding='utf-8'))
    # Include all CJK codepoints in sources/catalog, not just a manually curated list.
    symbols = ''.join(sorted({c for text in texts for c in text if ord(c) > 127}))
    executable = ROOT / 'tools/font/node_modules/.bin/lv_font_conv'
    subprocess.run([str(executable), '--font', str(font), '--size', '24', '--bpp', '4',
                    '--format', 'lvgl', '--range', '0x20-0x7e', '--symbols', symbols,
                    '--no-compress', '--lv-font-name', 'music_font_18',
                    '--output', str(ROOT / 'assets/fonts/music_font_18.c')], check=True)
    generated = ROOT / 'assets/fonts/music_font_18.c'
    font_text = generated.read_text(encoding='utf-8')
    # Converter's Opts comment must not publish local absolute paths.
    generated.write_text(font_text.replace(str(font), font.name).replace(str(ROOT), '.'), encoding='utf-8')
    characters = ''.join(chr(i) for i in range(32, 127)) + symbols
    (ROOT / 'assets/fonts/supported_characters.json').write_text(
        json.dumps({'font': 'music_font_18', 'characters': characters}, ensure_ascii=False, indent=2) + '\n',
        encoding='utf-8')
    print(f'Generated font: {len(characters)} codepoints')


if __name__ == '__main__':
    main()
