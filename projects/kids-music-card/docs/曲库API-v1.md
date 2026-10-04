# kidmusic.xiyuan.wiki：设备曲库 API v1

工作目录：`projects/kids-music-card/`。本协议对应当前 `music_catalog_protocol.c` 与 `music_library.c`，不是网站部署完成声明。默认源站固定为 **`https://kidmusic.xiyuan.wiki`**。

浏览器网站可以独立设计；设备需要以下静态 JSON 与裸 Opus 文件，不能把播放器网页、普通 Ogg 或登录跳转页面返回给设备。

机器可读字段约束：[`current.schema.json`](api-v1/current.schema.json)、[`manifest.schema.json`](api-v1/manifest.schema.json)、[`page.schema.json`](api-v1/page.schema.json)（JSON Schema 2020-12）。跨响应 release/页数/项目数、ID 全局唯一、UTF-8 字节长度、hash/path 对应关系及 HTTP 限制仍须按本文验证，不能只通过单份 schema 就认为可联调。

## 1. 请求与响应

- HTTPS、公认 CA 证书链；设备联网并完成 SNTP 后才访问。
- 可公开访问，也可统一 HTTP Basic auth；用户名/密码通过设备临时配网热点输入，不进入源码。网站上的浏览器认证方式不应改变这些设备端点。
- 成功 JSON 返回 `200`、正确 `Content-Length`、UTF-8 JSON 对象；正文最多 **16,384 字节**，嵌套最多 8 层。
- 成功首次音频请求返回 `200` 与整个文件的准确 `Content-Length`；恢复请求 `Range: bytes=N-` 返回准确 `206`、`Content-Range: bytes N-(size-1)/size` 与剩余长度。
- 不允许重定向、chunked、gzip/br 压缩或内容变换。设备发送 `Accept-Encoding: identity`；对以上端点关闭压缩，不要伪造长度。
- 认证失败用真实 `401/403`，缺文件用 `404`，服务错误用 `5xx`，不要以 `200` 返回 HTML 错误页。
- URL 不用 query、百分号编码、外部源站或双斜杠。音频必须是 `/v1/audio/<完整小写SHA256>.opus`。
- release 和音频文件发布后不可原地修改。先上传所有音频/分页/manifest，最后原子更新 `current.json`；保留旧 release，正在播放的设备仍使用旧版本。
- 设备单次元数据请求期限 10 秒；音频恢复另有次数与总期限限制，弱网需要真机验收。

## 2. current.json

路径：`/v1/current.json`

```json
{"schema_version":1,"release_id":"r20261002","manifest_path":"/v1/releases/r20261002/manifest.json"}
```

`release_id`：1–47 个 ASCII 字符，只允许字母、数字、`-`、`_`。`manifest_path` 必须由此 release 严格构成；不接受其他路径。

## 3. manifest.json

路径：`/v1/releases/r20261002/manifest.json`

```json
{
  "schema_version":1,
  "release_id":"r20261002",
  "total_tracks":21,
  "page_size":20,
  "total_pages":2,
  "audio_format":{
    "codec":"opus",
    "container":"length-prefixed-le16",
    "sample_rate":16000,
    "channels":1,
    "bitrate":32000,
    "frame_ms":20
  }
}
```

曲目数允许 **0–1000**，页大小固定 **20**。`total_pages = ceil(total_tracks/20)`，空曲库为 0 页。release 必须与 current 一致；音频参数必须完全一致，不能替换成普通 `.opus` Ogg 容器。

## 4. 分页

路径：`/v1/releases/<release>/tracks/all/<四位页号>.json`，页号从 **0000** 开始。

```json
{
  "schema_version":1,
  "release_id":"r20261002",
  "page":1,
  "total_pages":2,
  "tracks":[{
    "id":"song-21",
    "title":"小星星",
    "duration_ms":120000,
    "size_bytes":492000,
    "sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "audio_path":"/v1/audio/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.opus"
  }]
}
```

**上面的大小/hash 为格式示例，不是可播放文件。** 21 首时第一页必须有 20 首、第二页必须有 1 首。数组顺序就是播放顺序；同一 release 内 ID 全局唯一，排序不得变动。

| 字段 | 约束 |
|---|---|
| `id` | 稳定标识，1–39 个 ASCII 字符，只允许字母、数字、`-`、`_`；换标题或重新编码不应换 ID |
| `title` | 非空、合法 UTF-8、最多 95 **字节**，无控制字符；需符合设备字体字库，否则设备显示 ASCII ID |
| `size_bytes` | 整数，1–67,108,864；包含所有 2 字节帧前缀 |
| `sha256` | 对整个最终裸帧流计算的完整 SHA-256，64 个小写十六进制字符 |
| `audio_path` | 必须与 SHA 构成 `/v1/audio/<sha256>.opus`，不得使用 ID 文件名 |
| `duration_ms` | 网站辅助字段；设备当前不依赖它，发布工具按帧数计算 |

关键字段不可重复；不得用浮点数代替整数字段。额外辅助字段可以存在，但不要增加嵌套或超出正文上限。所有响应中的 release、页号、页数和曲目数量都要相互一致。设备进入在线列表时读取 current；播放轮次固定使用已选择 release，不在中途追随新发布。

## 5. 音频

```text
[length uint16 little-endian][one raw Opus packet]
[length uint16 little-endian][one raw Opus packet]
...
```

每包为 20 ms、16 kHz 单声道、32 kbps CBR Opus。帧长度 1–1500；前缀或包截断必须视为坏文件。文件扩展名 `.opus` 不代表 Ogg，服务器只原样提供字节。

设备在线播放按帧读取，不把整首歌装入 RAM，不隐式回退本地文件。缓存下载先检查容量、写临时文件、检查长度和 SHA，再更新双槽索引；网页无需管理设备缓存。

## 6. 本地生成可交付文件树

已有项目编码资源时：

```bash
cd projects/kids-music-card
python3 tools/publish_catalog.py --release r20261002 --output build/kidmusic-public-r20261002
```

输出必须是新目录，工具拒绝覆盖旧输出。在项目内只能写入已忽略的 `build/`；音频不得加入 Git。工具校验 ID、中文字体覆盖、分页、帧长度/20 ms mono TOC、文件大小及 SHA，并复制现有裸帧文件，不重新压缩，不修改本地 catalog，不连接服务器。它不是完整 Opus 解码测试，音源仍应由正式编码工具生成。

```text
<输出目录>/
  v1/current.json
  v1/releases/r20261002/manifest.json
  v1/releases/r20261002/tracks/all/0000.json
  v1/audio/<sha256>.opus
```

自备新曲库可传 `--catalog <数组JSON> --audio <裸帧目录>`，数组元素至少有 `id/title/path`。标题字库清单在 `assets/fonts/supported_characters.json`；新标题缺字时应先确认字体重建，或选用已支持标题。

本次已在本地生成六首曲目的 `build/kidmusic-public/`（release `device-v1`），**没有上传或部署**。仅在拥有音频分发授权时用于网站。

## 7. 上线后必须联调

1. 配网保存、重启自动连接、校时；不在聊天、日志或 Git 提供密码。
2. current/manifest/两页列表、分页随机与切歌、单曲循环。
3. HTTPS handshake 的峰值内存、首声延迟、连续播放、暂停、快速切歌、断网恢复。
4. 1–5 首缓存、重启离线播放、超选五首、容量不足、认证错误、错误 SHA、取消下载与断电恢复。
5. 实际设备目前保留六首旧资源。它们已占据大部分 Flash，保守容量策略下没有新增缓存余量；**最多五首是数量上限，不保证任意五首可存入**。迁移/减少旧资源须另行备份和确认，不能为联调自动删歌。
