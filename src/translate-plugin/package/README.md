# 翻译 · Translate

在 设置 → 插件 → 插件库 里安装。一个小程序（C++，不需要 Python）。输入 `tr 句子`（或 `fy 句子`）：中文译成英文，其他译成中文（设置里可以固定译成某种语言）。Enter 复制译文。

**默认不联网。** 要用它，先在设置 → 插件 → 翻译里：
1. **翻译服务**选 DeepL 或 OpenAI 兼容；
2. 填 **API 密钥**——它存在 Windows 凭据管理器里，不写进 `WinShun.ini`，也不进插件日志；
3. OpenAI 兼容的再填**接口地址**（如 `https://api.openai.com/v1`，或任何兼容服务）和**模型**。

没设置时输入 `tr 任何东西` 只会看到一行提示，不会发出任何请求。请求走系统代理；整个请求最多等 9 秒，超时会给出一行说明。同一句 5 分钟内不再请求。

---

Install it in Settings → Plugins → Plugin library. A small program (C++, no Python needed). `tr sentence` (or `fy sentence`) translates Chinese to English and anything else to Chinese, or into the language fixed in the settings, with **your own** DeepL or OpenAI-compatible API. Enter copies the translation.

Nothing goes online until a service and a key are set in Settings → Plugins → Translate (the key lives in Windows Credential Manager, not in the settings file). Requests use the system proxy and wait 9 seconds at most; the same sentence is not asked again for 5 minutes.
