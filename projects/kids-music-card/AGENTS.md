# 儿童音乐卡开发约定

## 入口与结构

- 本目录是完整 ESP-IDF 工程；以下命令均在 `projects/kids-music-card/` 内运行。
- 阅读 `README.zh_CN.md`（当前实现）和 `docs/需求文档.md`（历史需求草案）。二者存在行为差异，功能变更前需确认，不以草案自动覆盖实现。
- `main/music_app.c`：菜单、播放状态机、音频任务、息屏与设置保存。
- `components/bsp/`：AI Passport 显示、GPIO0 ADC 电阻梯按键和 ES8311 音频。
- `components/opus/`：固定点 Opus 解码器；保留第三方许可证。
- `assets/music/catalog.json`：歌曲目录；`main/music_catalog.h` 由编码工具生成；新增歌曲不应要求修改菜单逻辑。
- `assets/fonts/`：LVGL 中文子集字体和许可证；新增中文 UI/歌名时核查字形覆盖。
- `tools/`、`tests/` 和需求文档属于本项目，不移到仓库根目录。

## 硬件与行为约束

- ESP32-C3、8 MB Flash、无 PSRAM；ESP-IDF 5.5.3。
- 固件读取长度前缀裸 Opus 帧流：16 kHz、单声道、32 kbps CBR、20 ms 帧，不直接读取普通 Ogg/WAV。
- 按帧读取与解码，不能把整首歌曲载入 RAM。文件读取、I2S 写入、音频解码和 NVS 写入不得在按键回调或 LVGL/UI 任务内执行。
- 屏幕关闭不应中断音频。按键长短按、音量、播放模式与 NVS 行为以当前 README 为准。
- 保持分区容量检查；不得为了通过构建悄悄改变硬件容量或压缩参数。

## 验证

```bash
# 仓库根目录进入本项目：
cd projects/kids-music-card

# 静态验证：无需 ESP-IDF 或音频文件。
bash tools/validate.sh --static

# 固件验证：需 ESP-IDF 5.5.3。
source <esp-idf-5.5.3>/export.sh
bash tools/validate.sh --firmware

# 完整验证：静态 + 固件。
bash tools/validate.sh
```

本机 `sdkconfig` 不提交；可复现配置保存在 `sdkconfig.defaults`。验证可复现构建时使用默认配置，不依赖旧缓存。目录迁移后旧 `build/` 不可复用，需重新构建。

原始歌曲由用户自行提供，必要时运行 `python3 tools/encode_music.py --allow-missing` 或 `--only <ID>`，然后运行 `python3 tools/pack_music.py`。没有歌曲时可构建空资源镜像，但这不是可播放版本。音乐资源目录及其 README 不随歌曲文件一起排除。

真实设备仍需检查中文字形、ADC 按键、播放速度/爆音、暂停恢复、三种播放模式、息屏期间连续播放与唤醒。本次静态检查/构建不代替这些验收。

## 提交边界

- 本项目 `.gitignore` 排除 `build/`、`managed_components/`、`sdkconfig`、生成的 `musicfs.img`、原始音频和编码后音频；保留 `.gitkeep`。
- 依赖通过组件清单和 `dependencies.lock` 管理；不要把下载的 `managed_components/` 当成项目源代码提交。
- 先核实资源分发权再修改音频忽略规则；不要用 `git add -f` 绕过忽略规则。
