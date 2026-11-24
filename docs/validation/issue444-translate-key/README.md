# #444 截图翻译：密钥不对时只显示接口原话、也换不到别的翻译插件

日期：2026-11-24　机器：本机 Windows 11（26200），本地构建 `build/issues-1124b`。

## 原因

- 截图“识别文字”窗口的“翻译”走主程序的 `translate_text`，只用清单里声明了 `translate` 的插件，官方“翻译”在前，前一个没设好或失败就换下一个。
- 用户给官方翻译填的混元密钥被拒（401），官方插件把它当普通失败：窗口里只有“翻译失败：接口返回 401: Incorrect API key …”，没有去改设置的入口。
- 第三方的“顺译”（`anghost.translate` 1.1.0）的清单没有 `"translate"`，所以不会被 `translate_text` 用到，这是插件作者那边要加的一项（`"translate": "tr"`，第一行的 `copy` 是译文）。

## 改法

- 官方翻译 2.2.5：接口返回 401、403 时给一行 `setup`：“翻译服务的密钥不对或没有权限（接口返回 401）”，Enter 打开设置 → 插件 → 翻译；副标题保留服务的原话。402、429、5xx 也换成能看懂的话（余额不足、请求太多或额度用完、服务暂时出错，稍后再试），其余仍是“接口返回 N”。
- `translationAnswer`：`setup` 行回 `state: service` 时带上 `message`（这一行的标题）。因为是 `setup`，有别的翻译插件时会接着换下一个试。
- 截图插件 1.1.33：`service` 带说明时，译文区显示这句话加“设置好后再点“翻译”。”，下面仍是“设置翻译服务…”按钮；没带说明（旧主程序）照旧。
- 鼠标手势 2.1.3：划词翻译的气泡同样显示这句话（点击去设置）。

## 验证

| 项目 | 结果 |
|---|---|
| `WinShunTranslate.exe` 单独运行，接本机假服务（固定回 401 和混元那样的报错），按 Win顺 的方式查询 `tr External files` | 回一行 `[setup] 翻译服务的密钥不对或没有权限（接口返回 401）`，副标题“Enter 打开设置 → 插件 → 翻译，重新填密钥 · Incorrect API key provided: …”，`url=winshun:settings` |
| `WinShunCapture.exe --preview-ocr <目录> --translation --scale 1.5`（中文、`--english`） | 新增的 `key` 状态：[ocr-key-zh.jpg](ocr-key-zh.jpg)、[ocr-key-en.jpg](ocr-key-en.jpg)，说明加“设置翻译服务…”按钮；其余状态不变。退出码 1 是已知的窗口句柄重建警告，原有预览也有 |
| `tst_translate`（含新的 `statusRowSaysWhatToDo`）、`tst_ocr`、`tst_gestures` | 通过 |
| `tst_plugin translateTextInTurn` | 通过：没设好的翻译插件回 `service` 并带上它的说明，装了能翻译的就换它 |
