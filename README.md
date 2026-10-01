# AI Passport 项目集

围绕 FoloToy AI Passport 的独立应用与工具项目。每个主题在 `projects/<项目名>/` 下独立维护代码、资源、工具、测试和文档；仓库根目录不作为固件构建入口。

## 项目

| 项目 | 说明 | 文档 |
| --- | --- | --- |
| [儿童音乐卡](projects/kids-music-card/) | 离线三键音乐播放器，ESP32-C3 / 8 MB Flash | [中文说明](projects/kids-music-card/README.zh_CN.md) · [English](projects/kids-music-card/README.md) · [需求草案](projects/kids-music-card/docs/需求文档.md) |

## 开发入口

```bash
cd projects/kids-music-card
bash tools/validate.sh --static

# 固件构建需要 ESP-IDF 5.5.3；音频需自行提供并确保有权使用。
source <esp-idf-5.5.3>/export.sh
bash tools/validate.sh --firmware
```

仓库不包含原始或编码后的歌曲文件、生成的资源镜像、构建产物、下载的依赖或本机会话记录。没有提供音频时可以构建固件，但不能据此视为可播放或已通过设备验收。

## 新增项目

- 使用 `projects/<英文短横线名称>/`，各项目保留自己的构建入口和 README。
- 项目专属资源、工具、测试、配置与需求文档归入该项目，不混放于根目录。
- 更新本页项目列表；仅在确有多个项目复用时再抽取公共组件。
- 第三方组件保留已有许可证；音频等资源须先核实分发权再考虑提交。

代理操作约定见 [AGENTS.md](AGENTS.md) 及各项目的 `AGENTS.md`。
