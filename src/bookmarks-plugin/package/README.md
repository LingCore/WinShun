# 浏览器书签 · Browser bookmarks

插件库里的 C++ 一次性插件（设置 → 插件 → 插件库 里安装），不需要装 Python。输入 `bm 词`（或 `sq 词`）搜 Chrome 和 Edge 所有配置文件里的书签：名字、所在文件夹或网址里含每个词的都列出来，名字里全有的排在前面；Enter 打开网址，Ctrl+C 复制网址。

名字里有汉字的书签可以用拼音找：首字母、全拼、或者混着打都行，`xjtu`、`xian`、`xianjiaotong`、`xajt` 都能找到"西安交通大学…"。名字直接写着这个词的排在只会拼音的上面（`xian` 先给"西安…"，再给"…在线…"）。拼音只对含汉字的名字生效，英文书签照旧按字面找。

登录 Chrome / Edge 账户同步的书签存在同目录的 `AccountBookmarks` 里（有些配置甚至只有它，没有 `Bookmarks`），两个文件一起读；同一个网址在两处都有时只列一次。`EncryptedAccountBookmarks2` 是加密的，不读。

- **一次性**：每次查询起一个小程序（几毫秒），不常驻；结果缓存 60 秒（manifest 的 `cache`），同一个词再打不重跑。
- **files:read**：manifest 里写明它读哪些文件（`%LOCALAPPDATA%\Google\Chrome\User Data\*\Bookmarks`、`*\AccountBookmarks` 和 Edge 的同名文件）。这一条 Win顺 挡不住，只是让你知道——"仅声明"。
- **设置项**：另外的书签文件——Brave、Vivaldi 这类 Chromium 系浏览器的 `Bookmarks` 文件路径；同一个文件夹里的 `AccountBookmarks` 也会一起读，所以指向 Brave 的 `Bookmarks` 同样能搜到它的账户书签。注意 Opera 的书签在 `%APPDATA%\Opera Software\Opera Stable`（Roaming，且没有 `User Data` 这一层），只能这样手动指定。
- 不会改动书签文件。

---

A once C++ plugin from the plugin library (Settings → Plugins → Plugin library); no Python needed. `bm words` searches every Chrome and Edge profile's bookmarks by title, folder and address; Enter opens the address (a declarative `url`). A tiny process per query, results cached for 60 s. Bookmarks synced to a signed-in account live in `AccountBookmarks` beside the local list — sometimes as the only file — and both are read; a URL in both shows once. `EncryptedAccountBookmarks2` is encrypted and is not read. Its `files:read` lists the files it reads (a declaration Win顺 cannot enforce). The setting names one more Chromium browser's `Bookmarks` file, and the `AccountBookmarks` in that same folder is read with it; Opera keeps its bookmarks in `%APPDATA%\Opera Software\Opera Stable` (Roaming, no `User Data` level), so it is named there too. Nothing is written.

A Chinese title can be found by pinyin: initials, whole syllables, or a mix — `xjtu`, `xian`, `xianjiaotong` and `xajt` all find 西安交通大学…. A title that says the word outranks one that only spells it (`xian` gives 西安… before …在线…). Pinyin applies to Chinese titles only; English bookmarks keep matching literally.
