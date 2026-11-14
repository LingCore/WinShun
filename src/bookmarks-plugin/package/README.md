# 浏览器书签 · Browser bookmarks

插件库里的 C++ 一次性插件（设置 → 插件 → 插件库 里安装），不需要装 Python。输入 `bm 词`（或 `sq 词`）搜 Chrome 和 Edge 所有配置文件里的书签：名字、所在文件夹或网址里含每个词的都列出来，名字里全有的排在前面；Enter 打开网址，Ctrl+C 复制网址。

名字或所在文件夹里有汉字的书签可以用拼音找：首字母、全拼、或者混着打都行，`xjtu`、`xian`、`xianjiaotong` 都能找到"西安交通大学…"，`wa` 能找到放在"网安"文件夹里的书签。名字直接写着这个词的排在只会拼音的上面（`xian` 先给"西安…"，再给"…在线…"）。拼音只对含汉字的字生效，英文书签照旧按字面找；网址不读拼音（几乎都是英文，读了也白读），只按字面比。

登录 Chrome / Edge 账户同步的书签存在同目录的 `AccountBookmarks` 里（有些配置甚至只有它，没有 `Bookmarks`），两个文件一起读；同一个网址在两处都有时只列一次。`EncryptedAccountBookmarks2` 是加密的，不读。

- **一次性**：每次查询起一个小程序（几毫秒），不常驻；结果缓存 60 秒（manifest 的 `cache`），同一个词再打不重跑。
- **files:read**：manifest 里写明它读哪些文件（`%LOCALAPPDATA%\Google\Chrome\User Data\*\Bookmarks`、`*\AccountBookmarks` 和 Edge 的同名文件）。这一条 Win顺 挡不住，只是让你知道——"仅声明"。
- **设置项**：另外的书签文件——Brave、Vivaldi 这类 Chromium 系浏览器的 `Bookmarks` 文件路径；同一个文件夹里的 `AccountBookmarks` 也会一起读，所以指向 Brave 的 `Bookmarks` 同样能搜到它的账户书签。注意 Opera 的书签在 `%APPDATA%\Opera Software\Opera Stable`（Roaming，且没有 `User Data` 这一层），只能这样手动指定。
- 不会改动书签文件。

## 浏览历史（2.1.0）

在插件的设置里打开“搜索浏览历史”后，输入 `bh 词`（或 `ls 词`）搜 Chrome 和 Edge 所有配置文件里打开过的网页：标题或网址里含每个词的都列出来，标题是汉字的也可以打拼音；标题里有这个词的排在只有网址里有的前面，同一档里最近打开的在前。不打字就是最近打开的 50 个网页。每行写着浏览器、打开的时间和网址，Enter 再打开，Ctrl+C 复制网址。

- **默认关着**：没打开这个开关时不读历史，`bh` 只会提示去哪里打开。开了“AI 可以使用插件”的话，打开后 AI 也能查历史，介意就别开。
- **怎么读的**：读各配置文件夹里的 `History`（manifest 的 files:read 里写着），用 Windows 自带的 SQLite，不随插件多带东西。浏览器开着时这个文件是锁住的，插件不加锁、只读，不会影响浏览器，也不会改它；刚打开的网页可能要过一会儿才搜得到（浏览器还没写进文件）。
- 设置里指定了另外的书签文件时，同一个文件夹里的 `History` 也一起读。
- 无痕窗口的网页浏览器本来就不记，这里也没有。

---

A once C++ plugin from the plugin library (Settings → Plugins → Plugin library); no Python needed. `bm words` searches every Chrome and Edge profile's bookmarks by title, folder and address; Enter opens the address (a declarative `url`). A tiny process per query, results cached for 60 s. Bookmarks synced to a signed-in account live in `AccountBookmarks` beside the local list — sometimes as the only file — and both are read; a URL in both shows once. `EncryptedAccountBookmarks2` is encrypted and is not read. Its `files:read` lists the files it reads (a declaration Win顺 cannot enforce). The setting names one more Chromium browser's `Bookmarks` file, and the `AccountBookmarks` in that same folder is read with it; Opera keeps its bookmarks in `%APPDATA%\Opera Software\Opera Stable` (Roaming, no `User Data` level), so it is named there too. Nothing is written.

A Chinese title or folder can be found by pinyin: initials, whole syllables, or a mix — `xjtu`, `xian`, `xianjiaotong` all find 西安交通大学…, and `wa` finds the bookmarks filed under 网安. A title that says the word outranks one that only spells it (`xian` gives 西安… before …在线…). Pinyin applies to Chinese text only; English bookmarks keep matching literally. The address is matched literally and never as pinyin: it is nearly always English, so reading it as pinyin would find nothing.

**Browsing history (2.1.0).** With "Search the browsing history" turned on in the plugin's settings, `bh words` (or `ls words`) searches the pages opened in every Chrome and Edge profile by title or address, pinyin for Chinese titles too; a title that says the words comes before an address that does, the last opened first. Nothing typed lists the 50 pages last opened. Off by default: until then nothing of the history is read, and `bh` only says where to turn it on. With "AI may use plugins" on, an AI can search it too. The profiles' `History` files are read with Windows' own SQLite, with no lock taken and nothing written, so a running browser is not disturbed; a page opened a moment ago may take a while to show up. The `History` beside the settings' other Bookmarks file is read as well.
