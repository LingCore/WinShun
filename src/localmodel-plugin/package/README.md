# 本地模型 · Local models

在这台电脑上运行 AI 模型，断网也能用，要翻译的文字不出本机。在设置 → 插件 → 插件库安装。

**怎么用**

1. 装好后打开“本地模型”的卡片，模型默认是 Qwen3.5 2B（1.2 GB），点卡片上的“下载”。从 ModelScope 下载（国内快），断了下次接着下，下完核对 SHA-256 才用。
2. 到“翻译”插件的设置里，把“翻译服务”选成“本地模型（离线）”。之后 `tr 句子` 就用本机的模型翻译，截图翻译、AI 调用的 translate_text 也一样。
3. 搜索框输入 `llm `（或 `本地模型 `）可以看状态、下载、释放内存，回车打开这张卡片。

**选哪个模型**

| 模型 | 大小 | 适合 |
|---|---|---|
| Qwen3.5 2B（默认） | 1.2 GB | 翻译、问答都行 |
| 腾讯混元 HY-MT1.5 1.8B | 1.1 GB | 只做翻译，33 种语言 |
| 自己的模型文件 | — | 任何 llama.cpp 能用的对话模型（.gguf） |

自己的模型：模型选“自己的模型文件”，再在“模型文件”里选 .gguf。LM Studio 下好的模型在它的模型文件夹里（默认 `%USERPROFILE%\.lmstudio\models`）。要对话模型（文件名常带 Instruct、Chat 或 it），2B–8B、Q4_K_M 的最合适；mmproj（多模态的视觉部分）和 LoRA 适配器不行，嵌入模型要选到“嵌入模型文件”里，选错了卡片会直接说明。已经在用 Ollama 的话，也可以不装这个插件：在“翻译”里选“OpenAI 兼容”，接口地址填 `http://127.0.0.1:11434/v1`，模型填 Ollama 里的名字，不用密钥。

**按意思找也能离线**

“按意思找”插件（1.1.0）把“模型服务”选成“本地模型（离线）”，建索引和查找就都在本机算，文档的文字不上传。它用的是另一个模型，嵌入模型（把文字变成向量）：默认 Qwen3-Embedding 0.6B（610 MB，多语言）。“按意思找”第一次要用时，这张卡片上会多出“下载嵌入模型”；也可以输入 `llm ` 直接下载。只用翻译的话看不到它，也不用下载。

- 对话模型和嵌入模型各用各的：谁被用到才加载谁，各自空闲后释放。Qwen3-Embedding 0.6B 加载后约占 1.4 GB。
- 自己的嵌入模型：设置最下面“嵌入模型”选“自己的模型文件”，在“嵌入模型文件”里选 .gguf（比如 bge-m3）。换了嵌入模型，“按意思找”会重新建索引（不同模型的向量不能比）。
- 接口是 `/v1/embeddings`，同一个地址和端口。

**显卡和内存**

- “显卡”默认“有显卡就用”：NVIDIA、AMD、Intel 显卡都行（Vulkan，随显卡驱动带的）。显存不够时自动少放几层到显卡上，还不行就改用 CPU，卡片上会写明。CPU 也够用：默认模型在近几年的 CPU 上一句话约半秒。
- 模型在第一次用到时才加载（几秒），这么久没用就释放（默认 5 分钟），卡片上的“释放内存”可以立即释放。加载后占 1–2 GB 内存；用显卡时，显存里的模型在 Windows 上也算进进程的内存，所以卡片上的数字和用 CPU 时差不多。
- 一个进程最多用 16 GB（插件的内存上限），模型文件大约 12 GB 以内都行。

**本机接口**

模型在 `http://127.0.0.1:39281/v1` 提供 OpenAI 兼容的接口（`/v1/chat/completions` 和 `/v1/embeddings`，模型名随意），端口可以在设置里改。只有本机的程序能连：只监听 127.0.0.1，Host 必须是本机地址，别的网站的网页不能调用。浏览器打开 `http://127.0.0.1:39281/` 是 llama.cpp 自带的对话页面。

**隐私和网络**

只有点“下载”时才联网，从 ModelScope 下载所选模型；翻译和对话都在本机完成。日志（数据文件夹里的 `server.log`、`embed.log` 和 Win顺 日志）只记加载、设备、耗时和错误，不记要翻译的文字、文档的文字和回答。

**许可**

- llama.cpp：MIT，见 `licenses\llama.cpp.txt`；LLVM OpenMP：Apache 2.0 with LLVM exception，见 `licenses\llvm-openmp.txt`。
- Qwen3.5 2B、Qwen3-Embedding 0.6B：Apache 2.0。
- HY-MT1.5 1.8B：腾讯混元社区许可协议，不适用于欧盟、英国和韩国境内。
- 自己导入的模型按它自己的许可使用。

## English

Runs an AI model on this computer: no network needed, and the text you translate stays on this PC. Install it from Settings → Plugins → Plugin library.

1. On the Local models card the model is Qwen3.5 2B (1.2 GB) by default: click Download. It comes from ModelScope, resumes where it stopped, and is checked by SHA-256 before use.
2. In the Translate plugin's settings, set the service to Local model (offline). `tr sentence`, screenshot translation and the AI's translate_text then use it.
3. Type `llm ` in the search box for its state, Download and Free memory; Enter opens the card.

Models: Qwen3.5 2B (default, translates and answers), Tencent HY-MT1.5 1.8B (1.1 GB, translation only, 33 languages), or a .gguf chat model of your own (LM Studio keeps them in `%USERPROFILE%\.lmstudio\models`; 2B–8B at Q4_K_M suits most PCs; mmproj files and LoRA adapters are refused with a reason). Search by meaning (1.1.0) works offline too: set its model service to Local model, and indexing and searching are computed here by a second model, an embedding model: Qwen3-Embedding 0.6B (610 MB, multilingual; about 1.4 GB loaded), offered as Download embedding model on the card once Search by meaning first asks for it (or under `llm `), or a .gguf of your own (bge-m3…) under Embedding model at the bottom of the settings. Each model loads when it is used and is let go of when idle; a change of embedding model makes Search by meaning build its index again. Ollama users can skip this plugin: set Translate to OpenAI-compatible with `http://127.0.0.1:11434/v1`, the Ollama model's name and no key.

GPU: by default any NVIDIA, AMD or Intel GPU (Vulkan, part of the graphics driver); fewer layers on the GPU when its memory is short, the CPU when that fails, and the card says which. The CPU is fine too (about half a second a sentence with the default model). The model loads when first needed (a few seconds) and is let go of after 5 idle minutes (Free memory does it at once); loaded it takes 1–2 GB. A model in video memory counts towards the process's memory on Windows too. The plugin may use up to 16 GB, so model files up to about 12 GB.

Local API: `http://127.0.0.1:39281/v1`, OpenAI-compatible (chat completions and embeddings, any model name), port changeable in the settings. Programs on this computer only (127.0.0.1, a loopback Host, no other site's web pages). `http://127.0.0.1:39281/` is llama.cpp's chat page.

Only Download goes online (ModelScope). Logs hold loading, device, timing and errors, never the text or the answers.

Licenses: llama.cpp MIT (`licenses\llama.cpp.txt`), LLVM OpenMP Apache 2.0 with LLVM exception (`licenses\llvm-openmp.txt`), Qwen3.5 and Qwen3-Embedding Apache 2.0, HY-MT1.5 under the Tencent Hunyuan Community License (not for use in the EU, the UK or South Korea); your own models under their own licenses.
