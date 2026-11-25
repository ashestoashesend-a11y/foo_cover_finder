# foo_cover_finder

Finds album art online and saves the one you pick as `cover.jpg` in the album folder — right from the foobar2000 playlist context menu.

中文见下。

## Two ways to use it

- **One album:** right-click the album in the playlist → *Find cover art (online)* → pick from the previews. Anything goes here: you see the candidates before committing.
- **The whole library:** *Library → Fill missing cover art...* scans the media library, lists the albums that have no cover art, and fills the ones it can match with confidence. Nothing is written before you confirm, and unmatched albums are reported so you can handle them with the single-album flow.

## What it does

Right-click one or more tracks of an album → **Find cover art (online)**.

1. Reads `%album%` and `%album artist%` (falling back to `%artist%`) from the selection, and insists the selection stays inside one folder.
2. Searches four sources at once, none of which needs an API key: **iTunes** (JP store), **NetEase Cloud Music**, **QQ Music** and **MusicBrainz** (artwork via Cover Art Archive).
3. Scores the hits against your tags, drops the obvious misses, and shows the best 12 as thumbnails in a picker.
4. Downloads the full-size image of the one you choose and stores it where it actually belongs:
   - **Album folder** (everything in it belongs to this album): written as `cover.jpg`, asking first if one is already there.
   - **Mixed folder** (the folder holds tracks from other albums too, checked against the media library): written into the tags of each selected file instead, because a `cover.jpg` there would show up on every unrelated track. You are asked before any file is rewritten, and formats that cannot hold artwork are skipped and counted.

Nothing is written if you cancel, and no tags are touched — foobar2000 (and every other player) picks up `cover.jpg` from the folder on its own.

## Batch mode notes

- "No cover art" is judged the way the display judges it: embedded artwork, folder pictures and the album art search patterns configured in the preferences all count.
- In a mixed folder, only embedded artwork counts (a folder picture there belongs to the folder, not to the album).
- Batch mode only applies a candidate whose album name matches exactly — nobody is looking at previews — so uncertain albums are left alone instead of getting the wrong picture.
- MusicBrainz is probed less deeply in batch mode, since its 1 request/second limit would otherwise dominate the run time.

## Notes

- Every source is asked with the terms it actually handles: iTunes by artist *and* by album title (a combined "<artist> <album>" term returns nothing surprisingly often), QQ Music twice with the same terms, NetEase with the combination, MusicBrainz with a `release:"…" AND artist:"…"` query.
- QQ Music's real search API is signed; this uses its key-less suggestion endpoint, which returns a couple of albums per query — enough to find the right one, not a full catalogue.
- MusicBrainz only indexes metadata, so each candidate costs one extra Cover Art Archive request. To respect the 1 request/second rule the search sleeps between probes and only probes the top three releases, which makes a search take about three seconds longer.
- Matching strips `- Single`, `- EP`, `(Cover)` and similar tails, and ignores case, so `迷星叫 - Single` still matches the tag `迷星叫`.
- The folder is inspected through the media library, so only albums that are actually in the library are considered; when nothing is known about the folder it is treated as a single album.
- If a source fails (offline, blocked, rate limited) the others are still used, and the reason is printed to the Console.
- If nothing relevant turns up, the unfiltered hits are shown rather than an empty dialog — better a wrong cover you can reject than a dead end.

## Build

Same setup as the other components here: VS2022 Build Tools with the C++ workload and ATL, plus a Windows SDK.

The SDK, WTL and cJSON are not vendored; they come from a neighbouring `foo_openlyrics` checkout:

```
repos/
  foo_openlyrics/3rdparty/{foo_SDK,WTL10_10320_Release,cJSON}
  foo_cover_finder/
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build\build.ps1
# -> build\x64\Release\foo_cover_finder.dll
```

Copy the DLL to `%APPDATA%\foobar2000-v2\user-components-x64\foo_cover_finder\` and restart foobar2000.

## Licence

MIT.

---

# 中文说明

在 foobar2000 的播放列表右键菜单里给专辑找封面，选中的图存成专辑文件夹里的 `cover.jpg`。

## 两种用法

- **单张专辑**：播放列表里右键那张专辑 → *Find cover art (online)* → 从预览里挑一张。这条路上什么都能看到，先看再决定。
- **整个媒体库**：*Library → Fill missing cover art...*，扫描媒体库、列出没有封面的专辑，把有把握的自动补齐。动手之前一定会先问你，补不了的会列出来，用单张流程手动处理。

## 用法

选中**同一张专辑**的曲目 → 右键 → **Find cover art (online)**。

1. 从选区读 `%album%` 和 `%album artist%`（没有就用 `%artist%`），并检查选中的曲子都在同一个文件夹里。
2. 同时查四个源，都不要 API key、不要账号：**iTunes**（日区）、**网易云音乐**、**QQ 音乐**、**MusicBrainz**（封面走 Cover Art Archive）。
3. 按标签给结果打分、过滤掉明显不相关的，然后以缩略图形式列出最好的 12 个让你挑。
4. 选中后下载原图，并**存到它真正该去的地方**：
   - **纯专辑文件夹**（整个文件夹都属于这张专辑）：写成 `cover.jpg`；已存在会先问要不要覆盖。
   - **混装文件夹**（文件夹里还有别的专辑的曲子，用媒体库核对）：改成**写进选中曲目自己的标签**，因为 `cover.jpg` 会让这个文件夹里所有无关曲目都显示这张封面。重写文件之前一定会先问你，不支持的格式会跳过并计数。

取消就什么都不写，也不会动标签——foobar2000 和其他播放器都会自己从文件夹读 `cover.jpg`。

## 批量模式的说明

- 「有没有封面」的判定跟界面一致：嵌入的封面、文件夹图片、偏好设置里配置的封面搜索规则，都算有。
- 混装文件夹里只认**嵌入的**封面（那里的文件夹图片属于整个文件夹，不属于某张专辑）。
- 批量模式只应用**专辑名精确匹配**的候选——没人在看预览，所以没把握的宁可放过，也不写错图。
- 批量时 MusicBrainz 探测得更浅，否则它「每秒最多 1 次」的限制会主导整个耗时。

## 说明

- **每个源用它擅长的查询方式**：iTunes 分别按艺术家、按专辑名各查一次（拼在一起的「艺术家+专辑名」经常直接返回 0 条）；QQ 音乐用同样两个词各查一次；网易云用组合查询；MusicBrainz 用 `release:"…" AND artist:"…"`。
- QQ 音乐真正的搜索接口要签名，这里用的是它免签的搜索建议接口，每次只回一两张专辑——够找到目标，但不是全量目录。
- MusicBrainz 本身只有元数据，每个候选要多查一次 Cover Art Archive。为了遵守「每秒最多 1 次」的规则，查询之间会等待，而且只探测前 3 个版本，所以整体会慢 3 秒左右。
- 匹配前会去掉 `- Single`、`- EP`、`(Cover)` 这类尾巴并忽略大小写，所以 `迷星叫 - Single` 能和标签 `迷星叫` 对上。
- 文件夹是拿**媒体库**核对的，所以只对已入库的曲目有效；查不到关于这个文件夹的信息时按「纯专辑」处理。
- 一个源失败（断网、被墙、限流）不影响其它源，失败原因会打到 Console。
- 如果一个相关的都没有，会退而显示未过滤的结果——宁可给你一张可以拒绝的错误封面，也不给一个空白对话框。

## 编译

环境与这里的其它组件相同：VS2022 Build Tools + C++ 工作负载 + ATL + Windows SDK。

SDK / WTL / cJSON 不内置，取自隔壁的 `foo_openlyrics` 仓库：

```
repos/
  foo_openlyrics/3rdparty/{foo_SDK,WTL10_10320_Release,cJSON}
  foo_cover_finder/
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build\build.ps1
# -> build\x64\Release\foo_cover_finder.dll
```

把 DLL 放进 `%APPDATA%\foobar2000-v2\user-components-x64\foo_cover_finder\`，重启 foobar2000。

## 许可

MIT。
