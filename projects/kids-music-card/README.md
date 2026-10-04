# AI Passport Kids Music Player

ESP32-C3, 8 MB Flash, no PSRAM; ESP-IDF 5.5.3. Run all commands in
`projects/kids-music-card/`. See [中文说明](README.zh_CN.md) for current behavior.

## Current firmware

The home screen has exactly four entries: online playback, local playback,
cache songs, and system settings. Both playback lists pin random playback first.
The playback page uses Up/Down for previous/next, not volume. Volume 1–10,
Wi-Fi setup, and sequential/random/single-repeat mode live in settings.

Short OK pauses/resumes. After five seconds the display blanks without stopping
audio. While blank, short OK does not wake the screen; long OK wakes without
stopping. While lit, long OK stops and returns to the list (1.5 s threshold).
Buttons enqueue events; audio, filesystem, NVS and network waits run off the UI.

Network firmware implements bounded catalog pagination, HTTPS frame streaming,
phone provisioning via a temporary password-protected AP, and persistent
user-selected caching capped at five tracks. The default origin is
`https://kidmusic.xiyuan.wiki`. See the [device API](docs/曲库API-v1.md), including
machine-readable schemas under `docs/api-v1/`.

**Implementation is not end-to-end acceptance.** Offline/online builds and 42
host tests passed, and an earlier test build completed an injected-event
regression on the actual board. Physical button/display/audio checks and real
HTTPS/cache/power-loss tests remain pending. See [validation status](docs/设备端-v1-交付与验证.md).

The existing six built-in files are preserved. They currently exceed the
conservative new-write watermark, so adding downloaded files needs an explicitly
approved, backed-up migration. The five-track count cap does not guarantee that
five arbitrary tracks fit; no automatic deletion or eviction occurs.

## Build

```bash
cd projects/kids-music-card
source <esp-idf-5.5.3>/export.sh
bash tools/validate.sh --static

idf.py -B build/device-online -D SDKCONFIG=build/device-online/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.online.defaults' build

# Offline build: only sdkconfig.defaults, using its own SDKCONFIG/build directory.
```

Host stream/HTTP tests need `cc`; catalog parser tests additionally need
`IDF_PATH` for portable SDK cJSON sources and explicitly skip without it.
Firmware builds do not prove actual audio, ADC button recognition or networking.

After backup and partition/resource comparison, use **`app-flash`** for an
application-only update. Do not casually use `idf.py flash`: it also rewrites the
music filesystem, potentially destroying user caches. The optional USB test
console is disabled in normal builds; its regression input is
`tests/device_regression.commands`.

## Resources and server handoff

Audio is 16 kHz mono, 32 kbps CBR, 20 ms Opus packets, stored as repeated
little-endian 16-bit length plus raw packet; ordinary Ogg/WAV is not supported.
Use `tools/encode_music.py` and `tools/pack_music.py` for user-provided resources.
Audio files, credentials, local sdkconfig, build products and downloaded
components stay out of Git. No audio distribution rights are assumed.

The Chinese font is a 306-codepoint Noto Sans CJK subset. New titles must pass
`assets/fonts/supported_characters.json` coverage or display their ASCII IDs;
`tools/build_font.py` is the pinned regeneration workflow.

```bash
python3 tools/publish_catalog.py --release r20261002 --output build/kidmusic-public-r20261002
```

This creates a new local static catalog/audio tree, validates framing/hash/font
coverage, and does not deploy or modify server configuration. Read the API
contract before publishing, and confirm rights to distribute the audio.

Historical P0 reports and `docs/需求文档.md` are retained but do not describe the
new home screen. Current deployment plans are not statements that a VPS was
changed.
