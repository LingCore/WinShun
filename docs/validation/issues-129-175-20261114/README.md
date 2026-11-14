# #129 点文字打码、#175 字母直达（2026-11-14）

分支 `features-1114`，构建目录 `build/features-1114`（Release，Qt 6.12.0）。

## #129 打码工具点一下文字就打码

- `tst_annotmodel`：50 项通过，新增 `textLineClickHidesAndShowsAgain`、`textLineReadsWhenTheToolIsChosen`（红框的位置、点一下打码、再点取消、各是一步撤销、拖动照常画框、换工具后不生效、自动识别关着时选工具才识别、选区变了重新识别）。
- Win10 22H2 虚拟机，真实鼠标：截图 → 选打码工具 → 鼠标移到一行字上出红框（`vm-redact-hover.png`）→ 点两行都盖住，已盖住的那行框变蓝（`vm-redact-hidden.png`）→ 再点恢复（`vm-redact-shown-again.png`）。

没有覆盖：编辑窗口（贴图后编辑）里的同一功能只走了模型测试；倾斜的文字；多显示器。

## #175 字母直达

- `tst_numberhints`：14 项通过，新增 `letterTap`（轻按开关、长按不算、组合键不算、没看到按下的松开不算、窗口不听时退出、26 个字母各对一行）、`letterJumpSetting`。
- Win10 22H2 虚拟机，真实按键：
  - 搜索框：轻按 Ctrl，七行前面出现 A S D F G H J（`vm-launcher-letters.png`）；再按一下退出；Esc 退出；按 G 触发第五行，搜索词没有多出字母（`vm-launcher-letter-g-opened-row5.png`）。
  - 剪贴板（开在记事本上面，按键是转发过来的）：轻按 Alt 出字母（`vm-clipboard-letters.png`），按 S 把第二条粘进记事本，再开一次按 Shift+A 粘第一条（`vm-clipboard-pasted-by-letters.png`）。
- 顺带发现并修掉一个原有问题：剪贴板开着时单按 Alt（按住看序号或轻按），前面程序的菜单栏会被激活，记事本接着把粘贴用的 Ctrl+V 当成“查看(V)”。现在松开 Alt 前补一个掩码键，长按 0.9 秒后记事本菜单栏不再被激活（`vm-alt-held-no-menu.png`）。

没有覆盖：平铺视图和搜索面板里没有字母（只做了列表）；中文输入法正在组字时轻按；Windows 11；VS Code 等其他程序对 Alt 的反应。
