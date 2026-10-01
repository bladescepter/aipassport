# AI Passport 儿童音乐播放器

本工程把 FoloToy AI Passport 改造成离线三键音乐播放器。

工程目录为 `projects/kids-music-card/`。以下命令均在该目录执行；从仓库根目录进入：

```bash
cd projects/kids-music-card
```

[需求文档](docs/需求文档.md) 保留历史草案，按键行为、默认音量及已提供歌曲数与当前实现存在差异；当前实现概要以本说明为准。

## 升级规划

[在线播放与设备端离线收藏实施方案](docs/在线播放与离线收藏实施方案.md) 说明远程曲库分页浏览、HTTPS 流式播放、设备自主下载收藏、VPS 部署及分阶段验收。该文档为待实施方案，不代表当前固件已经支持联网。

[P0 开发记录](docs/P0-开发记录.md) 记录统一音频源、帧读取、有界缓冲、HTTP 响应/恢复策略及默认关闭的 `HTTPS (P0)` 试验入口。Wi-Fi/HTTPS 适配器与入口源码已编写，尚未完成 ESP-IDF 构建或真机验收；默认配置仍为离线。远程曲库、下载收藏和手机配网尚未实现。

## 实现概要

- `main/music_app.c`：歌曲菜单、播放模式、播放页、5 秒息屏和按键状态机。
- `main/music_catalog.h` / `assets/music/catalog.json`：歌曲目录；菜单不硬编码歌曲数量。
- `components/opus/`：ESP32-C3 固定点 Opus 解码器。
- `assets/music/data/`：SPIFFS 音频资源；播放任务通过 `music_file_source` / `music_frame_reader` 按帧读取，不把整首歌载入 RAM；帧头或正文截断显式报错。
- `components/bsp/`：官方 AI Passport 显示、ADC 电阻梯按键、ES8311 音频 BSP。
- NVS 命名空间 `music` 保存播放模式和 1–10 音量；无历史设置时默认音量为 8。

播放界面的确定键长按阈值在 BSP 中固定为 **1.5 秒**。熄屏后短按确定只暂停/播放且保持熄屏；熄屏时长按唤醒并回到播放界面且不中断歌曲，亮屏时长按停止当前歌曲并返回歌单。按键回调只入队；音频解码、文件读取、I2S 写入和 NVS 写入均不在回调或 LVGL 任务中执行。

## 音频资源

请提供原始歌曲（WAV/MP3/FLAC/OGG/M4A 均可），工程统一编码为：

> **Opus、16 kHz、单声道、32 kbps CBR、20 ms 帧**

固件实际读取的是 `2 字节小端长度 + Opus 帧` 的裸流。将源文件命名为
`assets/music/source/01.*` … `07.*`（也支持包含歌曲名的描述性文件名），执行：

```bash
python3 tools/encode_music.py
python3 tools/pack_music.py
```

歌曲的原始文件与编码后的 `data/*.opus` 均保留本地、不随仓库分发；克隆仓库后需自行提供有权使用的音频。没有歌曲时可以构建，但生成的空资源镜像不能用于播放验收。

资源也可以分批编码：测试第一首时使用 `python3 tools/encode_music.py --only 01`，
或使用 `--allow-missing` 跳过尚未提供的歌曲。

`encode_music.py` 需要带 `libopus` 的 `ffmpeg`；目标资源会写入
`assets/music/data/`，并更新歌曲时长目录。`musicfs` 分区为 `0x5B0000` 字节，
打包或固件构建超出容量会失败。

## 构建

使用 ESP-IDF 5.5.3：

```bash
source <esp-idf-5.5.3>/export.sh
idf.py set-target esp32c3
idf.py build
```

现有验证入口：

```bash
bash tools/validate.sh --static    # Python 检查、资源测试及主机 C 流式核心测试
bash tools/validate.sh --firmware  # 资源打包与固件构建，需先加载 ESP-IDF
bash tools/validate.sh             # 两者都执行
```

主机 C 测试需要 `cc` 或 `gcc`，不依赖 ESP-IDF；无主机编译器时会明确报告跳过。这些测试只编译音频源、帧读取、缓冲和 HTTP 策略核心，不编译网络任务、Wi-Fi/NVS 适配器、完整固件或硬件。

P0 联网试验的 Kconfig 前置条件、16/32 KiB 候选、私有 NVS profile 键及日志安全约束见开发记录；不要把真实凭据写入源码或配置文件提交，也不要未经备份覆盖设备 NVS。

版本控制仅保留 `sdkconfig.defaults`；本机 `sdkconfig` 和 `build/` 均忽略。迁移目录后必须重新构建，不复用含旧绝对路径的构建缓存。

真实设备验收仍必须检查中文字形、按键 ADC 识别、播放速度/爆音、暂停恢复、三种播放模式和黑屏期间音频是否继续。
