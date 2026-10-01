# AI Passport Children's Music Player

An offline three-button music player for the FoloToy AI Passport.

This standalone ESP-IDF project lives in `projects/kids-music-card/`. Run all
commands below from that directory; from the repository root:

```bash
cd projects/kids-music-card
```

The [requirements draft](docs/需求文档.md) is historical and differs from the
current implementation in button behavior, default volume, and song count.

The application in `main/music_app.c` owns the song menu, playback modes,
five-second display blanking, and the playback page. `assets/music/catalog.json`
and the generated `main/music_catalog.h` are the catalog boundary. The Opus
decoder is isolated in `components/opus`; SPIFFS resources are streamed frame
by frame by a dedicated audio task. NVS namespace `music` stores mode and
volume (1–10); the default volume is 8 when no setting has been saved.

The playback-page OK long-press threshold is 1.5 seconds. A short OK press while
blank pauses/resumes without waking the display; a long press wakes and returns
to playback without stopping the song when blank, while a long press on the lit
playback page stops the song and returns to the song list.

The requested source format is **Opus, 16 kHz, mono, 32 kbps CBR, 20 ms
frames**. You may provide WAV/MP3/FLAC/OGG/M4A originals; `tools/encode_music.py`
converts them to the length-prefixed raw Opus stream consumed by the firmware.
Use `--only 01` or `--allow-missing` when adding songs in stages. See
`assets/music/README.zh_CN.md` for the exact workflow.

Original and encoded audio files are local-only and are not distributed in
this repository. Supply audio you have the right to use before encoding and
packing it. A build without audio produces an empty resource image; it is not
a playable or hardware-validated release.

Build with ESP-IDF 5.5.3:

```bash
source <esp-idf-5.5.3>/export.sh
idf.py set-target esp32c3
idf.py build
```

Validation entry points:

```bash
bash tools/validate.sh --static    # Python compilation and unit tests
bash tools/validate.sh --firmware  # Pack resources and build (ESP-IDF required)
bash tools/validate.sh             # Both
```

Only `sdkconfig.defaults` is versioned; local `sdkconfig` and `build/` are
ignored. Rebuild after moving the project instead of reusing old build caches.
Real-device acceptance must still cover CJK glyphs, ADC buttons, audio quality
and speed, pause/resume, playback modes, and uninterrupted blank-screen audio.
