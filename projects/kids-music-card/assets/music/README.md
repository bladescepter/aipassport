# Music assets

Paths and commands are relative to `projects/kids-music-card/`, not the
repository root.

Provide source songs in `assets/music/source/01.*` through `07.*` (WAV, MP3,
FLAC, OGG, or M4A). A descriptive filename containing the Chinese title is
also accepted. The encoder converts them to **Opus, 16 kHz, mono, 32 kbps CBR,
20 ms frames**. The firmware stores length-prefixed raw Opus packets (`uint16
little-endian length + packet`) in SPIFFS, rather than reading WAV or an Ogg
container at runtime.

Run `python3 tools/encode_music.py`, or use `--only 01` / `--allow-missing` for
staged testing, then run `python3 tools/pack_music.py`. The encoder regenerates
the catalog header and the packer checks the 8 MB Flash music partition. Only
use audio you have the right to use. Both source and encoded audio are ignored
by Git until redistribution rights are confirmed. After cloning, supply your
own audio and generate playback resources using the commands above. An empty
resource image can build successfully but is not a playable release.
