# #171 搜索框打开网址、填入刚复制的文字；#191 截图翻译（2026-11-14）

分支 `features-1114`，构建目录 `build/features-1114`（Release，Qt 6.12.0）。

## #171

- `tst_core typedWebAddresses`：通过（带协议头、纯域名、带路径和端口、localhost、IP；文件名、版本号、路径、`main.py` 这类不算）。
- `tst_clipboard`：31 项通过，新增 `searchFill`（一行、去掉首尾空白、多行和超过 200 字不填）。
- `tst_aisettings` 160 项、`tst_settings` 59 项通过。第一次跑 `tst_aisettings` 时失败过一项：设置只认 3/5/10 秒，AI 设置却能写 4；改成 0–60 秒都认后通过。
- Win10 22H2 虚拟机，真实的 Win顺：
  - 另一个程序复制 `github.com/LingCore/WinShun`，2.5 秒后打开搜索框：文字已填入并全选（`vm-fill-just-copied.png`）；再打开一次不再填；复制后过 9 秒再打开也不填。
  - 开着“网页搜索”插件：`baidu.com` 第一行“打开网址”，第二行百度搜索（`vm-address-bare-domain.png`）；`github.com/LingCore/WinShun` 带着 GitHub 的图标（`vm-address-with-path.png`）；`report_01.txt` 只有本地文件，没有网址行（`vm-file-name-not-an-address.png`）。

没有覆盖：按 Enter 后浏览器真的打开（虚拟机里没验证到这一步）；本地有同名文件时网址排第二（只按代码逻辑，没有实际造同名文件）；用真实快捷键而不是第二次启动来打开搜索框。

过程中发现虚拟机的设置文件被以前的测试脚本写坏过（网页搜索插件的一行把后面的内容吞进去了），导致插件不生效，和这次的代码无关，已在虚拟机里修好。

## #191

- `tst_capture ocrWordsBecomeOneLine`：通过（翻译用的文字最多 800 字）。
- Win10 22H2 虚拟机：截图 → 识别文字 → 小窗口里有“翻译”按钮（`vm-ocr-window-translate-button.png`）→ 点击后搜索框带着识别出的文字进入翻译插件（`vm-translate-in-search-box.png`）。

没有覆盖：真实的翻译结果（虚拟机里没有翻译服务的密钥，显示的是“还没有设置翻译服务”）；没装翻译插件时的提示通知；旧版 Win顺 配新版截图插件时的退路（改用 `tr 文字` 打开搜索框）。
