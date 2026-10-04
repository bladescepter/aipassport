# Fonts

`music_font_18.c` is an 18 px, 4-bpp LVGL subset generated from Noto Sans CJK
SC under the SIL Open Font License. It covers the fixed Chinese UI strings,
the six installed song titles, ASCII, and the required punctuation.

The current font has 306 codepoints. `supported_characters.json` accompanies
this generated font and is used by the static catalog publisher. Keep the font
and report synchronized; changing the report alone does not add glyphs.

From the project directory, with `fonttools==4.59.0` and Node/npm available:

```bash
npm ci --prefix tools/font
python3 tools/build_font.py --font <Noto-Sans-CJK-SC.otf-or-ttc>
```

The converter is locked to `lv_font_conv==1.5.3`. The tool collects non-ASCII
characters from `main/*.c` and the local catalog, adds printable ASCII, and can
extract the SC face from a TTC. See the Chinese font README for details.
