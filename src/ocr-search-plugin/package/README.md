# 图片文字搜索 · Image text search

官方可选 C++ 插件 `winshun.ocr-search`，按需安装，不随 Win顺 安装包启用。识别和查询在本机完成，不需要云服务、API 密钥、Python 或 Office。

## 使用

1. 在设置 → 插件 → 插件库中安装（插件包要求构建它的 Win顺 版本，1.0.6 是 0.21.0 或更新版本）并启用“图片文字搜索”，然后点击插件的“设置”。也可下载 `winshun.ocr-search-<版本号>.wsplugin` 从压缩包安装。
2. 添加工作文档、截图或扫描件所在的文件夹，按需要排除子目录。没有默认全盘扫描。
3. 如果提示缺少增强识别组件，点击安装入口，在 Win顺 设置中安装共享 OCR 组件（约 12 MB）。截图、剪贴板和本插件共用这一套模型。安装完成后插件中的更新按钮自动可用。
4. 点击“开始更新”。可随时暂停；已完成的结果保留。开启后每 5 分钟检查变化，可用底部“自动更新”开关停止；暂停也会停止自动更新，下次启动仍保持暂停。
5. 索引建好后，直接在搜索框输入图片里的文字就能搜到；输入 `ocr 关键词` 只看图片和扫描件结果。Enter 打开原文件，Ctrl+Enter 查看原图和高亮，Shift+Enter 打开所在文件夹。也可在插件窗口搜索、缩放预览、拖动放大后的图片，以及复制所选图片／页的识别文字。

包含新内容提供者接口的宿主，还会在“内容”以及开启内容搜索的“全部”中查询插件索引；无需输入 `ocr`。在“全部”里，前 3 条图片结果排在前 5 个名称结果之后，其余在最后。普通文件搜索不触发 OCR 查询。插件需要带内容提供者接口的 Win顺，插件包的最低版本即构建它的主程序版本（1.0.6 为 0.21.0）。1.0.6 识别、搜索、预览之后闲下来，会把用不着的内存还给系统；窗口跟着 Win顺 设置里的界面风格变。1.0.5 修好了从 1.0.0／1.0.1 升上来的索引打不开的问题：报“unable to open database file Unable to fetch row”，更新和重建都报这句，搜不到结果。装上 1.0.5 后第一次打开就把旧索引升级完，已识别的结果都保留，不用重新识别。1.0.4 只改了窗口命中判断的共享代码（插件窗口被裁掉的控件不再抢走标题栏），1.0.3 只改了图标的画法（Phosphor 图标对齐像素格、窗口图标）。

## 文件覆盖与边界

- 图片：PNG、JPEG（含 .jfif、.jpe）、BMP、GIF（只读第一帧）、TIFF；多页 TIFF 按页处理。使用 Windows 自带图片解码器，并处理相机方向、透明底图。WebP、HEIC／HEIF、AVIF 要装对应的 Windows 扩展（Microsoft Store 里的“WebP 图像扩展”，“HEIF 图像扩展”加“HEVC 视频扩展”，“AV1 视频扩展”）；没装时这些文件列入未完成项，并提示要装哪个。普通照片按长边最多 4096 像素识别；长图（长边超过短边 3 倍，如整段聊天记录、网页长截图）保持原宽度，最多 4000 万像素。
- PDF：逐页识别。有图片的页（纯扫描页、已有文字层的混排页）都识别；没有图片的页，只有文字层里几乎没有字、页面却画了东西时才识别，例如字被转成曲线的设计图和 CAD 图纸、印章等批注。普通带文字层的页面直接跳过，这些字由 Win顺 的内容搜索负责。页面按长边 2600 像素、最高 200 dpi 渲染；页面上的图片更清晰时（如 A3、A1 扫描件），按图片自身的清晰度渲染，同样最高 200 dpi，最多 4000 万像素。使用宿主的 PDFium；预览定位具体页面。
- Office：DOCX / DOCM、PPTX / PPTM、XLSX / XLSM 中通过常规图片关联引用的位图。包含 Word 正文、页眉页脚及相关 XML 部件中的图片、PPT 幻灯片和 Excel 绘图图片。记录可用的段落、幻灯片或工作表信息，预览原始内嵌图；不重排 Office 版式，也不计算 Word 打印页码。图表、OLE 对象和未支持的图片编码不保证覆盖。
- 旧 DOC / XLS / PPT、WPS 专有格式不支持图片提取，明确列入未完成项。加密、损坏、仅在云端、超时或部分处理的文件也有原因，不计为完整识别。插件窗口还列出没有识别到文字的文件和原因（如 PDF 的字都在文字层里、文档里没有图片）；没有字的图片多是照片，只显示数量。
- 单文件最多 100 MB、500 页／张，文档解包预算 256 MB；单张图片超过 1 亿像素不解码，列入未完成项。超过页数的文件显示部分完成；读取部分图片失败时保留该文件中成功的图片并显示原因。
- 中英文和数字由共享 PP-OCR 模型识别；上下颠倒的页面认得不清时，会转过来再认一次。识别准确率取决于图像质量，不能保证手写、模糊和特殊排版全部正确。
- 空格分隔的词按 AND 搜索，可以分布在同一文件不同页／图中；引号保留短语。支持中文单字、双字，大小写和全角字符归一化。繁体和简体互相能搜到；中文字旁边有没有空格不影响匹配（识别结果常在中文之间、中英之间加空格）；A4、Q3、12 这类一两个字母或数字的词按整词匹配。结果按文件去重并显示一处命中位置；不会自动控制外部阅读器跳页。

## 资源、数据与权限

默认索引容量上限 512 MB，可调整。仅存压缩文字、识别框、图片指纹、位置及文件状态，不持久缓存原图、整页位图或缩略图。相同图片共享识别数据；文件内容副本／重命名尽量复用结果。索引文件的容量限制不包含共享 OCR 组件和 SQLite 短期日志；界面显示索引与日志的实际占用。

开发版存储优化使用压缩倒排表，省去逐条片段关系的记录开销；删除识别数据后会回收空闲页并清理临时日志。旧索引自动迁移，大多数文件无需重新识别：从 1.0.1 及以前的版本升级时，只补认以前跳过的 PDF 页、被缩小的长图和大页面、超时的文件和可能放反的页面，以及提示“缺少 PDF 读取组件”的 PDF（1.0.0 装好后找不到这个组件，已修好），插件窗口显示还剩多少个文件，期间照常可以搜索。迁移失败会保留原索引。容量按实际数据库页大小计算，界面占用包含数据库、WAL 和 SHM。512 MB 是默认上限，不是预先分配的空间；实际占用取决于识别文字量和重复率。

数据放在宿主指定的 `<插件数据根目录>/winshun.ocr-search.data`，包含 `ocr-search.ini` 与 `ocr-index.sqlite`。不写原文件。移除目录或清空索引只删除识别数据。暂时不可用或遍历失败的目录保留其未检查索引，某个子目录读不了时只保留这个子目录的索引；搜索会核对文件大小和毫秒修改时间，过期结果不会继续显示。修改时间和大小均未变化的原地改写，需手动“重新检查所有文件”。

工作进程复用宿主 Qt、PDFium 和 OCR 组件，以 Windows 后台优先级运行：CPU、磁盘和内存优先级都低，但去掉了后台模式附带的 32 MB 内存上限，字多的大页面因此快约 2.5 倍。一个文件连续 60 秒没有进展即跳过；识别一张图时每认完一段都算进展，字很多的大页面不会因为整页认得慢被跳过。预览限时 30 秒。失败的文件在大小或修改时间变化前不自动重试，可用“重新检查所有文件”重试。工作进程崩溃或超时会被结束并重建，主界面继续工作。查询、预览和后台识别各自异步，旧查询／旧预览不会覆盖当前选择。SQLite 按文件事务更新，取消会回滚未完成的文件。

权限 `ui` 用于插件窗口和组件安装入口，`files:read` 用于所选目录，`clipboard:write` 用于复制识别文字，`processes` 用于启动本插件自己的隔离工作进程。没有网络权限，外部图片链接不读取。启用时仍遵循 Win顺 原有权限确认流程。

## 开发与验证

源码：`src/ocr-search-plugin`。`OcrSource` 提取图片，`OcrWorker` 负责可结束的进程边界，`OcrStore` 管理去重、压缩和文件级查询，`OcrController` 管理后台任务与界面状态。复用 `grams` 中文／ASCII 索引、WIC 图片解码、共享 OCR、公共 QML 控件与现有 JSON-RPC 客户端。

构建 `WinShunOcrSearch`、`WinShunOcrSearchApp` 和 `library_plugins`，生成 `.wsplugin` 的现有发布脚本会自动包含此插件。自动测试为 `tst_ocrsearch` 和 `tst_plugin`；真实窗口验证工具为 `tools/verify_ocr_search.py`，仅使用其输出目录内生成的样本与独立配置。

---

An optional native C++ plugin. Add selected folders, install the shared enhanced OCR component if needed, and choose **Update index**. Nothing is uploaded and original files are read-only. Once the index is built, type the words from a picture straight into the search box; `ocr words` lists only picture and scan results. Enter opens the original, Ctrl+Enter opens a highlighted preview, and Shift+Enter reveals its folder. It needs WinShun 0.18.0 or later with content providers, which merges results into Content and content-enabled All searches. Install it from the plugin library, or download the .wsplugin package and install from archive.

Supports PNG/JPEG (including .jfif and .jpe), BMP, GIF (first frame), multi-page TIFF, scanned and mixed PDFs, and ordinary embedded bitmap relationships in modern Word, PowerPoint and Excel documents. WebP, HEIC/HEIF and AVIF need the matching Windows extensions from the Microsoft Store; without them those files are listed as unfinished, naming the extension. Legacy Office/WPS formats, encryption, unsupported encodings and failures are reported. Office previews show the embedded original image, without reproducing the Office page layout. A PDF page without pictures is read only when its text layer holds almost nothing while the page draws something (lettering turned into outlines, CAD drawings, stamps); ordinary text pages are left to WinShun's content search. Photos are read at up to 4096 px; long pictures (more than 3:1) keep their width, and big PDF pages render at their pictures' own resolution, up to 200 dpi and 40 megapixels. Pages that read poorly are read once more turned upside down. The plugin window lists unfinished files and files where no text was found, each with the reason. Limits: 100 MB and 500 pages/images per file, 256 MB archive expansion, 100 megapixels per picture, 60 seconds without progress per file (progress within a page counts), 30 seconds per preview. Failed files are retried only after they change or with Recheck all files. Multiple words may match different pages of one file; one matching location is shown per file. Traditional and simplified Chinese find each other, spaces next to Chinese characters do not matter, and one- or two-character words such as A4 or 12 match as whole words.

The default 512 MB index stores compressed text, recognition coordinates and deduplicated fingerprints, without a persistent image cache. File-level transactions preserve completed work on pause or interruption. Background changes are checked every five minutes while enabled. Removing folders or clearing the index preserves original files. Qt, PDFium and OCR models are shared with the host; Python and Office are not runtime dependencies. The worker runs at background priority, without background mode's 32 MB working-set cap, which made dense pages about 2.5 times slower.

The optimized development format uses compact contentless FTS5 postings instead of one database record per gram/image relationship. Existing indexes migrate transactionally using their stored text. After an upgrade from 1.0.1 or earlier, only what the old version skipped or shrank is read again (PDF pages, long pictures, big pages, timed-out files and pages that may be upside down), plus the PDFs that failed with "PDF component is missing" (once installed, 1.0.0 did not find WinShun's PDF reader; fixed); the plugin window shows how many files are left, and search works meanwhile. Removing indexed data reclaims database pages and checkpoints the WAL. The configured capacity is a limit, not a preallocation; shared models and temporary SQLite journals are outside that limit. Reported disk usage includes the database, WAL and SHM.
