# 字库

`music_font_18.c` 是使用 Noto Sans CJK SC（SIL Open Font License）和
`lv_font_conv` 生成的 18 px、4 bpp LVGL 子集，覆盖播放器固定中文文案、六首
已安装歌曲名、ASCII 和所需标点。不要用默认 Montserrat 字体替换它，否则中文会
显示为方框。

当前字库含 306 个 codepoint；`supported_characters.json` 是给曲库发布工具使用的覆盖清单。字体许可证保留在 `NOTO_SANS_CJK_LICENSE.txt`。

重建前准备 Python `fonttools==4.59.0` 与 Node/npm；在项目目录执行：

```bash
npm ci --prefix tools/font
python3 tools/build_font.py --font <Noto-Sans-CJK-SC.otf或TTC>
```

转换器 `lv_font_conv==1.5.3` 由 `tools/font/package-lock.json` 锁定。工具从 `main/*.c` 与本地 catalog 收集非 ASCII 字符，并补充 ASCII；TTC 只提取 Noto Sans CJK SC 字面。新远端歌名应先核查清单，必要时经确认把其文案纳入正式字库输入，再重建字体；不能只修改清单假装字体已有字形。字体文件与清单需一起更新并构建验证。
