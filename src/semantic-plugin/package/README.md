# 按意思找 · Search by meaning

Win顺 0.17.0 或更新版本的官方可选 C++ 常驻插件（`WinShunSemantic.exe`，源码在 `src/semantic-plugin`）。在设置 → 插件 → 插件库中“安装并启用”，确认权限后使用。输入 `? 一句话`（中文输入法下 `？` 也行），描述你要找的文档：不记得原词也能找到。比如 `? 去年报销的规定` 能找到《费用管理办法.docx》，哪怕里面写的是“差旅费用申请”。

**怎么用**
1. 设置 → 搜索范围 → 文件内容里的**文件内容搜索**要开着：插件用的是 Win顺 已经从文档里读出来的文字，不会再读一遍硬盘。
2. 设置 → 插件 → 插件库中安装并启用**按意思找**，再到“已安装”的插件设置里选一个**模型服务**：
   - **本地模型（离线，1.1.0）**：什么都不上传，也不用另装软件。在插件库装“本地模型”插件，在它的卡片上点“下载嵌入模型”（Qwen3-Embedding 0.6B，610 MB），下好就能用；还没装、没下载时，这里的提示行回车就打开它的卡片。有显卡用显卡，没有用 CPU。
   - **Ollama / LM Studio（本机）**：什么都不上传。先装 [Ollama](https://ollama.com)，再运行 `ollama pull qwen3-embedding:0.6b`（或 `bge-m3`、`embeddinggemma`）。
   - **硅基流动、阿里云百炼、OpenAI 或其他 OpenAI 兼容接口**：填 **API 密钥**（存在 Windows 凭据管理器里）。模型留空就用推荐的。
   选好之后，卡片的状态行旁有“测试连接”按钮（1.2.0）：发一小段文字给所选的模型服务，连上了写“连接正常”和用时，连不上写明原因（密钥不对、模型没下载、服务没启动）。正在建索引时不显示这个按钮，因为那时本来就在用它。
3. 输入 `?`，第一行告诉你大约有多少个文件、多少字要计算（云服务会按它的价格计费）。**回车之前什么都不发出去。**（也可以在设置里这张卡片上点“开始建立索引”，那里写着同样的数字。）
4. 建索引要一阵子，进度在两处都看得到（1.0.6，需要 Win顺 0.46.0）：设置 → 插件 → 已安装里这张卡片上有“第几个 / 共几个文件、大约还要多久”和进度条，点开卡片可以**暂停**、**继续**；输入 `?` 时第一行也是同样的进度，会自己更新，回车暂停或继续。暂停时已经算好的都留着，重启后仍是暂停的。更早的 Win顺 只有 `?` 那一行。
5. 建好以后输入 `? 一句话`，结果按意思排序，副标题是最相近的那一段（PDF 和 PPT 会注明第几页、第几张）。

**会发出去什么**：选云服务时，文档的文字分成几百字一段发给服务商计算向量（每个文件最多约 4 万字），搜索时发你输入的那句话。算出来的向量存在本机的插件数据文件夹里，比对也在本机。文件改了、删了，下一轮（约 10 分钟一次）会跟着更新。

**换模型**要重新建一遍（不同模型的向量不能比）。用本机模型时，你在用电脑它只用三分之一的时间计算，你走开了才全速。

AI 助手也能用它：设置 → AI 助手里打开“AI 可以使用插件”，AI 就多一个按意思找文件的工具。

---

An official optional resident C++ plugin requiring WinShun 0.17.0 or later. Choose **Install and enable** in Settings → Plugins → Library, review its permissions, then choose a model service in its settings under Installed. `? a sentence` finds documents by what they are about rather than by their words. It takes the text from WinShun's content index (File content search under Settings → Search scope → File contents must be on), has the model you pick turn passages into vectors — WinShun's own Local models plugin (1.1.0: offline, nothing uploaded, nothing else to install; download its embedding model on its card), Ollama or LM Studio on this PC (nothing uploaded), or SiliconFlow, Alibaba Cloud Model Studio, OpenAI or another OpenAI-compatible API with your key — and keeps and compares the vectors on this PC. Nothing is sent before you press Enter on the first row, which says how many files and about how many characters would go. Since 1.0.6 (with WinShun 0.46.0 or later) the plugin's card under Settings → Plugins → Installed shows how far the index is, with a progress bar, the time left and Pause / Resume; the row shown for `?` moves by itself too. Changing the model means building the index again.
