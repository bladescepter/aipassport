# 音乐资源

本文路径与命令均相对于项目目录 `projects/kids-music-card/`，不是仓库根目录。

## 请提供什么音频

优先提供原始歌曲文件（WAV、MP3、FLAC、OGG 或 M4A 均可），不要直接提供未压缩 PCM 给固件。工程会用 `ffmpeg` 统一转换为：

- **Opus**，Ogg Opus 作为转换中间格式
- **16 kHz** 采样率
- **单声道**
- **32 kbps CBR**
- 20 ms 一帧

固件最终使用的是 SPIFFS 中的裸 Opus 帧流：每帧为 `2 字节小端长度 + Opus 帧`，不是直接读取 `.wav` 或普通 Ogg 容器。请优先按目录清单中的 ID 命名，例如 `assets/music/source/01.wav` 到 `07.wav`。也支持包含歌曲名的描述性文件名，例如 `歌唱祖国-合唱.mp3`。然后运行：

```bash
# 全部歌曲到齐时
python3 tools/encode_music.py

# 也可以分批编码；例如当前只测试 01，或允许跳过尚未提供的歌曲
python3 tools/encode_music.py --only 01
# python3 tools/encode_music.py --allow-missing

python3 tools/pack_music.py
```

编码后的文件放在 `assets/music/data/`，目录与 `main/music_catalog.h` 会自动更新。歌曲总时长受 8 MB Flash 限制；当前分区大小和固件会在构建/打包时检查，超出会直接报错。

## 版权

只使用有权使用的音频。原始音频与编码后的 `data/*.opus` 均默认被 `.gitignore` 忽略，未核实分发权前不上传；编码不改变音频的版权要求。仓库只保留资源说明、歌曲目录和目录占位，克隆后需自行提供音频，再按上述流程生成播放资源。没有歌曲时可构建空镜像，但不能据此视为可播放或已通过设备验收。
