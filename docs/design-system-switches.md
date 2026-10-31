# Win顺 系统开关：设计草案（2026-10-19）

> 实现更新（2026-10-31）：原生后端已移入可选 C++ 插件 `src/switches-plugin/`，新配置默认关闭，旧配置迁移一次。宿主保留查询语法和异步界面缓存，插件持有设备监听、工作线程与电源请求。下文“主程序直接持有后端”的早期设计已被替代；当前结构见 [架构文档](architecture.md#系统开关)，通信协议见 [插件文档](plugins.md#系统开关提供者)。

## 0. 目标

用户的两条要求是：代码现代化，用户体验好。落到这个功能上：

- 在搜索框里输入“蓝牙”“深色”“音量 30”，就出现一行，**看得到现在的状态**。按 Enter 就地切换，窗口不关，结果马上显示在这一行上。按 Esc 关窗口。
- 已定的九条要避开下面几类问题：
  - 开关做成一堆独立的“切换××”命令；
  - 切完就关窗口，看不到结果；
  - 失败时弹对话框；
  - 读状态拖慢搜索。

### Win顺 独有的角度

1. **开关就是系统入口里的一行**：现有的关键词、拼音、首字母、“最近打开”加分全都照用，不另起一套命令表。
2. **一个概念只占一行**：“蓝牙”只有一行，Enter 切换，Ctrl+Enter 去设置页。动词和数值写进同一行（关蓝牙、音量 30），不用记命令名。
3. **保持唤醒由 Win顺 自己持有**：不装 PowerToys Awake 也有。到点自动结束，退出 Win顺 也随之结束，开着时托盘上看得见。
4. **不拖慢搜索**：
   - 状态存在界面线程的缓存里，异步刷新。
   - 搜索线程只产出两样东西：这是哪个开关，用户想做什么。

## 1. 范围

| 档 | 开关 | 读 / 写 | 结论 |
|---|---|---|---|
| 第一批 | 深色模式（应用和系统一起） | HKCU 注册表，再广播 | 实测可用 |
| 第一批 | 显示隐藏的文件 | `SHGetSetSettings` | 实测可用 |
| 第一批 | 显示文件扩展名 | `SHGetSetSettings` | 实测可用 |
| 第一批 | 蓝牙 | `Windows.Devices.Radios` | 实测可用（开、关都翻过） |
| 第一批 | WLAN | 同上 | 读状态、同值写入实测可用；**真正的开关没翻**（原因见 5.3）。阶段 3 真机验收时补测，过了才算第一批 |
| 第一批 | 音量、静音（默认输出设备） | Core Audio `IAudioEndpointVolume` | 实测可用 |
| 第一批 | 保持唤醒 | 电源请求（`PowerCreateRequest`） | 实测可用 |
| 第二批（需实测） | 亮度、蓝牙耳机连接和电量、切换默认输出、夜间模式（夜灯）、飞行模式、HDR | 见 5.6 | 每项先用探针量出数字，过了闸门再做 |

**闸门**（已定第 5 条）：在普通权限的界面进程里能**可靠地读和写**，才上线。缺硬件或权限被拒时，这一行退回成设置页的链接，并附一句原因。

不在这两批里的（移动热点、节电模式、专注、麦克风）照旧是设置页，以后按同一个闸门评估。

## 2. 交互

### 2.1 行的样子

```
输入：蓝牙
 ▍[图标] 蓝牙                                                开 (━━━●)
         Windows 设置                  （第二行：没有要说的就照旧写来源）
 Enter 关闭蓝牙    Ctrl+Enter 打开设置                    Tab 切换范围

输入：关蓝牙
 ▍[图标] 关闭蓝牙                                            开 (━━━●)   ← 标题写意图，开关写现状
         Windows 设置
 Enter 关闭蓝牙    Ctrl+Enter 打开设置
   按下后开关在 120 ms 内滑到“关”；再按一次，底栏闪“蓝牙已经是关的”

输入：音量 30（这台现在是 24%、静音）
 ▍[图标] 音量设为 30%                           24 ▬▬▭▭▭▭▭▭   关 (●───)
         扬声器 (3- Realtek(R) Audio) · 已静音，会同时取消静音
 Enter 音量设为 30    Ctrl+Enter 打开设置              Alt+←→ 调音量

输入：深色模式（应用浅色、系统深色）
 ▍[图标] 深色模式                                        自定义 (──●──)
         自定义：应用浅色，系统深色
 Enter 全部切到深色    Ctrl+Enter 打开设置

空搜索框，保持唤醒开着时
 ▍[图标] 保持唤醒                                            开 (━━━●)
         到 16:42 · 还剩 42 分钟
  [图标] （下面照旧是最近打开）
 Enter 结束保持唤醒    Ctrl+Enter 电源设置

降级（缺硬件或没有权限）
 ▍[图标] 蓝牙                                                     系统
         这台电脑没有蓝牙
 Enter 打开设置

失败
 ▍[图标] 蓝牙                                                关 (●───)
         没能打开蓝牙：Windows 没有回应                   （Theme.danger 字色）
```

**行尾的开关**
- 开关常显，不只在悬停时出现。它取代现在地点行的“系统”标签（`ResultRow.qml` 的 badge）。
- 宽度固定，状态读到之前和之后都一样宽，标题不会被挤得跳一下。
- 开关就是现有的 `ToggleSwitch.qml`：“开 / 关”二字加 40×20 的轨道。
  - 开：`Theme.accentFill` 填底，白色的钮（`Theme.accentText`）。
  - 关：1 px 的 `Theme.subtext` 描边。
  - 放在行里时设 `activeFocusOnTab: false`，不画焦点环。
- 两态之外，新增三种样子：
  - **混合**：深色模式里应用和系统不一样。字写“自定义”，钮放在中间。
  - **不知道**：状态还没读到。只画描边轨道，没有钮，不写字。
  - **进行中**：150 ms 内还没确认，才淡到 50%。
- 音量行的行尾依次是：数字 `24`，64×4 的音量条（用 `Theme.accent` 画线），开关（开 = 有声，关 = 静音）。
- 悬停或当前行时，“打开设置”按钮（RowAction，齿轮图标）出现在开关左边，提示写“打开设置（Ctrl+Enter）”。

**第二行**（原来显示文件夹的那一行）
- 写状态说明：设备名、“自定义：应用浅色，系统深色”、“到 16:42 · 还剩 42 分钟”。
- 没有要说的，就照旧写来源（“Windows 设置”）。
- 失败时同一位置改成红字。按 colors.md，状态色只能用在字上。

**命令行**（带动词或数值的行）：标题写意图（“关闭蓝牙”“音量设为 30%”“保持唤醒 1 小时”），行尾照样显示**现在的**状态。

### 2.2 按键

| 键 | 开关行上 | 说明 |
|---|---|---|
| Enter | 普通行：切换；命令行：执行 | 窗口不关，焦点留在搜索框。搜索框里的字全部选中，接着打字就是新命令 |
| Ctrl+Enter | 打开这个开关的设置页，关窗口 | 今天地点行上的 Ctrl+Enter 只闪一句“没有可以打开的位置”（`Launcher::performPlace`），这个键正好空着 |
| Shift+Enter、Ctrl+Shift+Enter | 同 Enter | 开关都不需要管理员权限 |
| 数字键（Ctrl+1…9） | 同 Enter，窗口不关 | 照旧走 `pressNumber` → `holdUntilShown` |
| 单击行、单击开关 | 同 Enter | 走现在的 `rowClicked` → `trigger(Open)`。双击不能变成翻两次：同一行在系统的双击间隔（`styleHints.mouseDoubleClickInterval`）内的第二下不算。照 pitfalls“快速连点，第二下没有 `clicked`”的做法，在处理函数里判断，不靠 Qt 漏不漏事件 |
| 按住 Enter（自动重复） | 只认第一下 | `Main.qml` 的 `handleKey` 里，`event.isAutoRepeat` 时跳过 |
| Alt+← / Alt+→ | 音量行：减 / 加 5 | 对齐到 5 的倍数。加的时候顺带取消静音。普通的 ←/→ 是输入框的光标键，不能占。别的程序可能把这两个组合注册成全局热键，那时 Win顺 收不到（同 pitfalls“剪贴板页里 Alt+1、Alt+2 没反应”），“音量 30”这种打字写法照样能用 |
| Tab | 不变（补全 / 切换范围） | |
| Esc | 不变：立刻关窗口 | 正在进行的写入照常做完，结果只进缓存 |
| Ctrl+点击、Shift+↓ 多选 | 开关行不进选择 | `performMany` 跳过开关行 |
| 右键、Menu 键 | 菜单：设为开 / 设为关（写明目标状态）、打开设置、复制名称、从最近打开中删除 | |

### 2.3 底栏和提示

- `Launcher::refreshKeyHints` 给开关行单独加一个分支。
  - Enter 的提示**写目标状态，不单写“打开”“关闭”**：“Enter 关闭蓝牙”“Enter 切换到浅色”“Enter 静音”“Enter 取消静音”“Enter 音量设为 30”“Enter 保持唤醒到 16:42”“Enter 结束保持唤醒”。
  - 原因：Win顺 里的“打开”本来指“打开文件”，底栏右边还有“Esc 关闭”。只写“关闭”会被看成关窗口。
- 第二项写“Ctrl+Enter 打开设置”。保持唤醒写“Ctrl+Enter 电源设置”。
- “蓝牙设置”这种命令行（3.1 第 7 条）：Enter 那一项写“Enter 打开蓝牙设置”，不再列 Ctrl+Enter。
- 音量行上，Tab 那一格换成“Alt+←→ 调音量”。Tab 照样能用。
- 命令要的状态已经是了：不写入，底栏闪“蓝牙已经是关的”，开关的轨道亮一下（160 ms）。
- **第一次**在开关行上翻转之后，底栏闪一次“要打开设置页：Ctrl+Enter”，记进 state.ini，以后不再提示。这是照顾以前“蓝牙 Enter = 打开设置页”的习惯，属于一次性的小提示。
- 当前行的开关状态变了，不管是自己改的还是别人改的，底栏都跟着变（`Switches::changed` → `refreshKeyHints`）。

### 2.4 动画

全部不慢于 Windows 自己的（约 200 ms）。

- 钮滑动：120 ms，OutCubic（`ToggleSwitch` 现有的）。轨道换色现在是一下子换的，加一个同样 120 ms 的颜色动画。
- 音量条变宽变窄：120 ms，OutCubic。
- “进行中”变淡：等 150 ms 还没确认才开始，用 80 ms 淡到 50%。实测蓝牙打开要 324 ms，关闭只要 18 ms，所以关闭时不会闪一下淡色。
- 失败：钮用 120 ms 滑回原处，第二行换成红字（字不做动画）。
- 已经是那个状态：轨道亮一下，160 ms。
- 窗口收起时动画停在半路：再显示时直接跳到终值（见 pitfalls“没出帧，动画就停在半路”）。
- Esc 关窗口：照旧立刻开始。

### 2.5 不知道、进行中、失败、降级

| 情形 | 行 | Enter |
|---|---|---|
| 还没读到状态（刚启动） | 开关画成“不知道” | 不带动词：等状态，最多 500 ms（和 `holdUntilShown` 一个思路），到了再切；还没到就闪“还没读到蓝牙的状态”。带动词：直接做（动词本身是幂等的） |
| 进行中 | 钮已经在目标位置，150 ms 后变淡 | 同一个开关再按：忽略；别的行照常 |
| 失败 | 钮滑回去，第二行红字：“没能打开蓝牙：Windows 没有回应” | 再按就是重试 |
| 失败时窗口已经收起 | 不打扰。30 秒内再打开、这一行还在时显示红字 | |
| 降级（缺硬件；权限被拒；被策略或对比度主题挡住） | 没有开关，仍显示“系统”标签。第二行灰字说原因：“这台电脑没有蓝牙”“‘让应用控制设备无线电’已关闭”“没有声音输出设备”“对比度主题开着时，深浅色不起作用” | Enter 打开设置页并关窗口（就是今天的行为）。权限被拒时打开 `ms-settings:privacy-radios` |

- 按 Enter 时，“切换”在界面线程上就按**当前显示的状态**换成明确的“开”或“关”，再交给后台。后台不再自己判断方向，所以底栏写什么就做什么。
- 错误文字一律是我们自己写的中文，HRESULT 只进日志（见 pitfalls“结束不了提权的进程：插件以用户身份跑，别把英文异常原样丢到底栏”）。

### 2.6 空搜索框

- **不列开关**。开始列表照旧是“最近打开 / 最近修改”，开关行不进去。
  - 原因：打开窗口时第 0 行就是当前行（`Main.qml` 的 `selectFirst`），很多人打开窗口顺手按 Enter，重开上一个东西。刚切过的开关排在那里，这一下就把它切了回去。
  - 以前系统入口行上误按 Enter，最多打开一个设置页；现在会改系统状态，所以误按的路要堵上。
  - 用过的次数照样记下，给打字搜索时的排序加分。
- 唯一的例外：**保持唤醒开着时**，开始列表最上面多一行“保持唤醒 · 还剩 42 分钟”。
  - 它**不做默认选中行**：打开窗口时高亮落在它下面那一行（最近打开的第一条），Enter 照旧重开上一个东西。
  - 要结束：按 ↑ 到它上面再按 Enter，或者点它的开关。
  - 这是 Win顺 自己持有的状态，必须看得见，关掉也只要一步。

### 2.7 怎么看出保持唤醒开着

- **启动器**：空搜索框时它在第一行（但不是默认选中行，见 2.6）；搜“唤醒”时也在第一行。第二行写“到 16:42 · 还剩 42 分钟”，或者“直到手动结束或退出 Win顺”。
- **托盘提示**：`Win顺 · 保持唤醒到 16:42`（用 `MessageWindow::setTrayTooltip`）。
- **托盘菜单**：开着时最上面多一项打勾的“保持唤醒（到 16:42）”，点一下就结束。没开时不出现：菜单保持精简，开启从启动器里做。
- **托盘图标**：第一版不加小点（见待定问题）。
- 到点结束时不弹通知。

## 3. 查询语法

### 3.1 规则

记号：
- S：开关的主语词（名字和关键词）。
- W：状态词，自带目标状态。
- V+、V-、V~：开、关、切换三类动词。
- N：数。
- D：时长。

规则：
1. **先规整输入**：做 NFKC 规范化（全角数字和字母变半角），拉丁字母转小写，连续空格并成一个，去掉末尾的 `%`。
2. **整句就是一个 S**：不是命令。交给普通的系统入口搜索，出普通开关行（Enter 切换）。
3. **整句就是一个 W**：设成 W 带的状态（深色、静音、mute）。
4. **动词加主语**：
   - `V S` 或 `S V`，中文可以不留空格：开蓝牙、蓝牙关、bluetooth off、turn off wifi。
   - `V W`：关闭深色 = 设为浅色。
5. **主语加数**：`S N`、`S +N`、`S -N`，如“音量 30”“音量30”“vol 30”“音量 +10”。
6. **主语加时长**：`S D` 或 `S 到 时间`，如“保持唤醒 1小时”“awake 90m”“保持唤醒 到 18:00”。
7. **主语加“设置”**：`S 设置`、`S 选项`、`S settings`，如“蓝牙设置”“深色模式设置”“bluetooth settings”。这是在找设置页：出一行命令行“打开蓝牙设置”，Enter 打开设置页（和 Ctrl+Enter 一样）。行尾照样显示开关，单击开关照样切换。只认放在后面的“设置”。
8. **闸门**：去掉动词、数值、时长、“设置”后，剩下的部分必须**整词**命中**恰好一个**开关的 S 或 W。不满足就把整句原样交给普通搜索，一个字都不吃掉。所以“关于”“关机”“开始菜单”“打开方式”“关闭防火墙”“开热点”“禁用网卡”都照旧。
9. **一次只认一个开关**：“关蓝牙和 wifi”不认。

### 3.2 词从哪来

**动词、数、时长**：写在代码里的常量表（`constexpr` 数组，按长度从长到短试）。不在每次按键时现建正则（见 pitfalls“循环里现建 QRegularExpression 很慢”）。
- V+：开、打开、开启、启用、on、turn on、enable
- V-：关、关闭、关掉、关上、停用、禁用、取消、off、turn off、disable
- V~：切换、toggle、switch；还有后缀“开关”（如“蓝牙开关”）
- 时长单位：
  - 小时：小时、个小时、h、hr、hour、hours
  - 分钟：分钟、分、m、min、minute、minutes
- 时长的数：1–999、小数（1.5）、一到十、两、半（半小时、一个半小时）
- 时间点：`到 18:00`、`到18点`、`until 18:00`。时间已过，就到明天的这个时间。

**S 和 W**：写在 places.txt 每个开关条目里（`keywords`、`on`、`off` 三个键），和系统入口用同一份数据。拼音照样自动生效（“ly”能找到“蓝牙”那一行），但拼音不进命令语法。

### 3.3 例子

| 输入 | 结果 | 说明 |
|---|---|---|
| `蓝牙`、`bluetooth`、`bt`、`ly` | 普通开关行，Enter 切换 | 整句是 S |
| `开蓝牙`、`打开蓝牙`、`开启蓝牙`、`启用蓝牙`、`蓝牙开`、`蓝牙 开`、`bluetooth on`、`turn on bluetooth`、`enable bt` | 命令行“打开蓝牙” | 已经开着：闪“蓝牙已经是开的” |
| `关蓝牙`、`关闭蓝牙`、`关掉蓝牙`、`蓝牙关`、`bluetooth off`、`disable bt` | 命令行“关闭蓝牙” | |
| `切换蓝牙`、`蓝牙开关`、`toggle bluetooth` | 命令行“切换蓝牙” | 效果和普通行的 Enter 一样 |
| `wifi off`、`关 WiFi`、`关闭无线网`、`关wlan` | 命令行“关闭 WLAN” | |
| `蓝牙设置`、`深色模式设置`、`隐藏文件设置`、`bluetooth settings` | 命令行“打开蓝牙设置”等，Enter 打开设置页 | 点名要设置页；行尾照样能切换 |
| `深色`、`暗色`、`dark` | 设为深色 | 单个状态词：按关键词分数排，不强行置顶 |
| `浅色`、`亮色`、`light` | 设为浅色 | 同上 |
| `关闭深色`、`关深色模式` | 设为浅色 | `V- W` 和 `V- S` |
| `打开深色模式`、`dark mode on` | 设为深色 | |
| `深色模式`、`dark mode`、`深浅色` | 普通开关行 | |
| `夜间模式` | 不变：仍是夜灯的设置页 | 这是 Windows 对夜灯的叫法，不是深色模式 |
| `静音`、`mute` | 静音 | |
| `取消静音`、`解除静音`、`unmute` | 取消静音 | |
| `音量 20`、`音量20`、`音量 20%`、`vol 20`、`volume 20`、`音量 ２０` | 音量设为 20，并取消静音 | |
| `音量 0` | 音量设为 0 | 不等于静音 |
| `音量 +10`、`音量 -5` | 在当前值上加减 | |
| `音量 150` | 设为 100，底栏说明“最大 100” | |
| `音量` | 普通行，Enter 切换静音 | |
| `隐藏文件`、`hidden files` | 普通行“显示隐藏的文件” | |
| `显示隐藏文件`、`show hidden files` | 设为显示 | 整句是 W |
| `不显示隐藏文件`、`hide hidden files` | 设为不显示 | |
| `扩展名`、`后缀名`、`file extensions` | 普通行“显示文件扩展名” | |
| `显示扩展名`、`隐藏扩展名`、`不显示后缀名` | 设为显示 / 隐藏 | |
| `保持唤醒`、`唤醒`、`不睡眠`、`防止睡眠`、`awake`、`caffeine` | 普通开关行 | |
| `保持唤醒 1小时`、`保持唤醒 一小时`、`唤醒 60分钟`、`awake 1h`、`keep awake 90m` | 保持唤醒到某个时刻（hh:mm） | |
| `保持唤醒 半小时`、`保持唤醒 一个半小时` | 30 分钟、90 分钟 | |
| `保持唤醒 到 18:00`、`保持唤醒到18点`、`awake until 18:00` | 到 18:00 | 时间已过就到明天的 18:00 |
| `关闭保持唤醒`、`取消唤醒`、`awake off` | 结束 | |
| `关于`、`关机`、`开始菜单`、`打开方式` | 原样搜索 | 去掉“关”“开”“打开”后，剩下的不是开关 |
| `关闭防火墙`、`关闭实时保护`、`开热点`、`关闭更新`、`启用网卡`、`禁用网卡` | 原样搜索，照旧落到各自的设置页 | 同上 |
| `开蓝牙耳机` | 原样搜索 | “蓝牙耳机”是蓝牙设置页的关键词，不是开关 |
| `关蓝牙和wifi` | 原样搜索 | 一次只认一个开关 |

### 3.4 怎么接进系统入口的匹配和排序

**每个开关在 places.txt 里是一个新条目**，不是给已有的页面补关键词：

```
[switch-bluetooth]
name = 蓝牙 | Bluetooth
switch = bluetooth
open = ms-settings:bluetooth
kind = settings
keywords = 蓝牙; bluetooth; bt

[switch-dark-mode]
name = 深色模式 | Dark mode
switch = theme
open = ms-settings:colors
kind = settings
keywords = 深色模式; 暗色模式; 黑暗模式; 夜间主题; 深浅色; 浅色模式; dark mode; light mode
on = 深色; 暗色; 黑色主题; dark
off = 浅色; 亮色; 白色主题; light
```

**一共七个条目**：switch-dark-mode、switch-hidden-files、switch-file-extensions、switch-bluetooth、switch-wlan、switch-volume、switch-keep-awake。方括号里的 id 会记进“最近使用”，定了就不改。

**从页面条目上挪走的关键词**（页面保留其余的，如配对、蓝牙耳机、已知网络、扬声器）：

| 页面条目 | 挪走的关键词 |
|---|---|
| `[ms-settings:colors]` | 深色模式、浅色模式、暗色模式、黑暗模式、夜间主题 |
| `[…Microsoft.FolderOptions]` | 显示隐藏文件、隐藏文件、扩展名、显示扩展名、文件扩展名、后缀名 |
| `[ms-settings:network-wifi]` | wifi、wi-fi、无线、无线网、无线网络、wlan |
| `[ms-settings:bluetooth]` | 蓝牙、bluetooth |
| `[ms-settings:sound]` | 音量 |

**打分**
- S 整词命中照旧打分（自己的关键词 50 + 6），和应用、别的入口一起排。
- **弱命中封顶**：只靠拼音首字母、词中或模糊命中时（如“ly”“yl”），开关行的分数不超过同样命中的应用和页面，排在它们后面。误按 Enter 打开一个应用没有害处，误切一个开关就改了系统状态。没有同样命中的应用时，开关行照样排第一，“ly”回车照样能切蓝牙。
- **带动词或数值的命令行**才置顶：分数用 `kTypedKeywordScore`，排在“输入的网页关键字”那一行之后（用户自己定的关键字优先）。
- **单个状态词**（深色、静音、dark、mute）不置顶，按关键词整词的分数排，免得“dark”压住名叫 Darktable 的应用。

**去重**
- 现在 `searchPlaces` 按命令去重（每个命令只出一行），改成：
  - 开关条目按 switch id 去重。
  - 某个开关行已经出现时，**和它同命令的页面行不再出**。例如搜“蓝牙”只出开关行，蓝牙设置页用 Ctrl+Enter 打开，或者打“蓝牙设置”（3.1 第 7 条）。
  - 页面行只在得分不高于开关行时才让位；查询把页面的名字打得更全、页面得分更高时，两行都出，按分数排。
- 隐藏文件和扩展名两个开关的 Ctrl+Enter 目标相同，但按 id 去重，所以两行都在。
- 命令行出现时，同一个开关的普通行不再出（一个概念一行）。

**只在“全部”里出**：文件、内容范围里没有开关行，插件的关键字模式里也没有。

**已核对的影响**（用 `wsbench --places`）

| 词 | 今天第一行 | 改后 |
|---|---|---|
| 深色模式 | 名叫“浅色主题设置”的那一条 | 先出开关 |
| 隐藏文件、扩展名 | 都落到“文件资源管理器选项” | 先出开关 |
| 静音 | “更改屏幕指示器的位置”（30 分） | 先出开关 |
| 夜间模式 | 夜灯（显示设置，115 分） | 不动 |

## 4. 架构

### 4.1 新文件和改动

| 文件 | 改什么 |
|---|---|
| `src/core/places.txt` | 七个开关条目（新键 `switch`、`on`、`off`）；从五个页面条目挪走关键词；文件头的说明补上新键 |
| `src/core/SystemCatalog.{h,cpp}` | `PlaceInfo` 加 `switchId`、`onWords`、`offWords`；`applyPlaceExtras` 认新键；`searchPlaces` 的去重键改成“开关 id 或命令”，同命令的页面行让位 |
| `src/core/SwitchCommand.{h,cpp}`（新） | `SwitchIntent`；`parseSwitchCommand(QStringView, const PlaceList&)`，返回 `std::optional<SwitchCommand>`；纯函数 |
| `src/core/SearchTypes.h` | `SearchResult` 加 `switchId`、`intent` |
| `src/core/SearchEngine.{h,cpp}` | 全部范围：加命令行（排在输入的网页关键字之后）；`fromPlace` 带上 `switchId`；`SearchEngine::Request` 加 `pinnedPlaces`（Launcher 发请求时填），开始列表把它们置顶 |
| `src/app/Switches.{h,cpp}`（新） | 界面线程上的缓存和调度；后端接口；一次性提示 |
| `src/app/platform/SwitchTheme.{h,cpp}`（新） | 深浅色 |
| `src/app/platform/SwitchExplorer.{h,cpp}`（新） | 隐藏文件、扩展名 |
| `src/app/platform/SwitchRadios.{h,cpp}`（新） | 蓝牙、WLAN |
| `src/app/platform/SwitchAudio.{h,cpp}`（新） | 音量、静音。不叫 SwitchVolume：项目里的 Volume 指盘（`VolumeNotifier`、`volproto`） |
| `src/app/platform/KeepAwake.{h,cpp}`（新） | 电源请求、到点结束、睡眠恢复后核对 |
| `src/app/platform/SettingBroadcast.{h,cpp}`（新） | 先通知本进程的顶层窗口，再在广播线程上发全局消息 |
| `src/app/platform/ComWorker.{h,cpp}` | 构造参数可选 STA 或 MTA |
| `src/app/platform/MessageWindow.*` | 不改。它已有 `resumed`（`PBT_APMRESUMEAUTOMATIC`）和 `deviceChange`（`WM_DEVICECHANGE`，现在交给盘的 `VolumeNotifier`）两个回调，App 接线时顺带通知 `Switches` |
| `src/app/ResultModel.{h,cpp}` | 开关的几个角色；命令行的标题；收到 `changed` 就发 `dataChanged` |
| `src/app/Launcher.{h,cpp}` | `performPlace` 的开关分支；`refreshKeyHints`；`handleShown` 时刷新；`stepSwitch(row, delta)`；多选和 `performMany` 跳过开关行 |
| `src/app/qml/ResultRow.qml`、`SwitchControl.qml`（新）、`ToggleSwitch.qml` | 行尾的开关和音量条；混合、不知道、进行中三种样子 |
| `src/app/qml/Main.qml` | Enter 的自动重复；Alt+←/→；`selectFirst` 和 `onBetterFirstRow` 跳过置顶的状态行（保持唤醒） |
| `src/app/App.{h,cpp}` | 建 `Switches`；托盘菜单和提示；`dumpLauncher` 输出开关字段和底栏提示 |
| `src/app/i18n/winshun_en.ts` | 英文 |
| `tools/wsbench.cpp` | `--switches`（只读） |
| `tests/tst_core.cpp`、`tests/tst_switches.cpp`（新）、`tests/CMakeLists.txt` | 见 4.6 |

### 4.2 按一次 Enter 经过哪些地方

1. **搜索线程**：`parseSwitchCommand` 或 `searchPlaces` 产出一行，带上 `SearchResult::switchId = "bluetooth"` 和 `intent = Off`（普通行是 `Toggle`）。这里不读任何状态。
2. **界面线程**：`ResultModel::data` 拿 `switchId` 去查 `Switches` 的缓存，给出 `switchOn`、`switchBusy` 等角色，QML 据此画行尾。
3. **Enter**：`Launcher::trigger` → `performPlace` 的开关分支 → `Switches::apply(id, intent)`。
   - 把“切换”换成明确的开或关。
   - 已经是目标状态：只闪提示。
   - 否则：缓存标成“进行中”，先把开关翻过去显示，任务投给开关线程。
   - 这一行记进历史，但不重排眼前的列表。
4. **开关线程（MTA）**：写入，读回，再把结果投回界面线程（`QMetaObject::invokeMethod(switches, …, Qt::QueuedConnection)`）。
5. **界面线程**：更新缓存，发 `changed(id)`。`ResultModel` 对这些行发 `dataChanged`，只带开关的几个角色。底栏跟着变。

### 4.3 线程

| 线程 | 做什么 | 不做什么 |
|---|---|---|
| 界面线程（STA） | 缓存；读注册表（微秒级）；注册表变更通知（`RegNotifyChangeKeyValue` + `QWinEventNotifier`，照 `SettingsEditor::TaskbarWatch` 的写法）；保持唤醒的电源请求（微秒级）；切完深浅色后先同步通知自己的顶层窗口 | 不调 WinRT 的 `.get()` 或 `wait_for`；不做全局广播 |
| 开关线程（`ComWorker` 加 MTA 选项，一条 `std::jthread`） | 无线电对象和音频端点都在这里；除保持唤醒外，**写入**都在这里做；等 WinRT 的结果用 `wait_for(5 s)`，按 stop token 取消 | 不等广播 |
| 广播线程（按需起一条 detach 的 `std::thread`，同一时刻最多一条） | 发 `SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, "ImmersiveColorSet", SMTO_ABORTIFHUNG, 1000)`；发的过程中又来请求，只记一个原子标志，发完再补发一次 | 退出时**不等**它（原因见表下）；不用 `std::jthread`，它的析构会 join |

- **广播线程退出时为什么不等**：
  - 广播也会发给 Win顺 自己的窗口。界面线程在 join 时不处理消息，广播就卡在发给自己窗口的那一步，一直等到超时。1000 ms 按每个窗口算，Win顺 有好几个顶层窗口，退出会卡好几秒。
  - 所以它是 §6“用 `std::jthread`”的例外：detach，只按值捕获（要发的字符串是静态的），不碰任何会在退出时析构的对象。
  - 进程退出时它若还在发，就随进程结束，没有害处。
- **回调线程**：无线电和音频的回调**不在调用的线程上**，落在哪个线程也不固定（实测）。一律带上下文对象转回界面线程（见 pitfalls“不带上下文对象 connect 引擎的信号，槽在引擎的工作线程上跑”）。
- **`IMMNotificationClient`**：它的回调里不调 MMDevice API。把事情投给开关线程，在那里重新绑定。
- **写入的顺序**：
  - 同一个开关的写入一个接一个做。
  - 一个写入还在排队时又来一个，用新的换掉旧的，只做最后一个。
  - 每个结果带序号，过时的结果只用来更新真实状态。
- **闲着时**：
  - 开关线程睡在条件变量上。所有通知都是事件，**没有任何轮询定时器**。
  - 保持唤醒的“还剩几分钟”每 30 秒刷新一次，只在启动器可见、列表里有保持唤醒行时才刷（见 pitfalls“什么都不做时也有线程在醒”）。
- **退出**：
  - 开关线程的等待切成 100 ms 一段，每段查一次 stop token，需要时 `IAsyncOperation::Cancel()`。
  - 关掉保持唤醒的请求句柄（进程退出时系统也会收回）。

### 4.4 状态缓存和行模型

```cpp
struct SwitchIntent {
    enum class Kind : quint8 { Toggle, On, Off, SetLevel, StepLevel, AwakeFor, AwakeUntil, OpenSettings };
    Kind kind = Kind::Toggle;
    int value = 0; // 音量 0–100 或 ±步长；分钟数；从午夜算起的分钟数
    bool operator==(const SwitchIntent&) const = default;
};

struct SwitchState {
    enum class Availability : quint8 { Unknown, Ready, NoDevice, Denied, Blocked };
    Availability availability = Availability::Unknown;
    qint8 on = -1;    // -1 不知道，0 关，1 开，2 混合（深色：应用和系统不同）
    qint8 level = -1; // 音量 0–100；别的开关 -1
    bool busy = false;
    QString note;     // 第二行：设备名、结束时刻、降级原因
    QString error;    // 第二行的红字
    QDateTime until;  // 保持唤醒的结束时刻（墙上时间）
    bool operator==(const SwitchState&) const = default;
};
```

- **`Switches`**：QObject，住在界面线程。接口有 `state(id)`、`apply(id, intent)`、`step(id, delta)`、`refresh()`，信号是 `changed(const QString& id)`。
- **`SearchResult`** 只加 `QString switchId` 和 `SwitchIntent intent`。两者都能比较，默认的 `operator==` 照常工作。
  - 状态**不进** `SearchResult`：结果行要做 diff，状态一变，这一行就会被当成新行。
- **`ResultModel` 的新角色**：`switchId`、`switchOn`、`switchLevel`、`switchBusy`、`switchNote`、`switchError`、`switchReady`，以及 `statusRow`（置顶的状态行，不做默认选中行）。
  - 名字避开 Item 自己的属性，如 `state`、`enabled`、`visible`（见 pitfalls“`required property bool enabled` 遮住了 Item 自己的”）。
  - 命令行的标题（“关闭蓝牙”）在 `ResultModel` 里用 `tr()` 生成，core 不管文案。
- **`Switches::changed(id)`**：`ResultModel` 找出这个 id 的行，发 `dataChanged(i, i, {开关角色})`，行不重建。做法和 `colorSchemeChanged`、网站图标到达时一样。
- **`--dump`**（`App::dumpLauncher`）：每行加 `switchId`、`switchOn`、`switchLevel`、`switchBusy`、`switchNote`、`switchError`，再加底栏的 `keyHints`。真机测试直接读这些，不靠截图。

### 4.5 通知来源

| 开关 | 平时靠什么知道状态变了 | 打开启动器时 |
|---|---|---|
| 深色模式 | `SystemTheme` 已有的 WM_SETTINGCHANGE（"ImmersiveColorSet"）过滤；加上 Personalize 键的 `RegNotifyChangeKeyValue`（别的程序只写注册表、不广播时也能知道） | 读两个值（微秒级） |
| 隐藏文件、扩展名 | Explorer\Advanced 键的 `RegNotifyChangeKeyValue`（一个等待管两项） | 读两个值 |
| 蓝牙、WLAN | `Radio.StateChanged`。网卡来去：MessageWindow 已有的 `deviceChange` 回调（`DBT_DEVNODES_CHANGED`）触发重新枚举，1 秒内的合成一次 | 距上次枚举超过 30 秒才重新 `GetRadiosAsync`（热的时候 0.2–0.4 ms） |
| 音量、静音 | `IAudioEndpointVolumeCallback::OnNotify`（带 event context，认得出自己写的）；`IMMNotificationClient::OnDefaultDeviceChanged`（eRender、eConsole）时换端点、重新注册 | 读音量和静音（0.03 ms） |
| 保持唤醒 | 自己持有。到点用一个单次定时器；从睡眠恢复时（MessageWindow 已有的 `resumed` 回调）按墙上时间再核对一次 | — |

`Launcher::handleShown` 里调 `Switches::refresh()`。上面这些都很便宜，统一在开关线程上做，结果回来再刷新行。搜索照旧立刻开始，不等它。

### 4.6 测试（都不改系统状态）

**`tst_core`**：加在已有的 `placeExtras`、`placeSearch` 旁边。
- 语法表：3.3 的每一行都是一条用例，包括所有“原样搜索”的例子和全角数字。
- places.txt 的新键：`switch`、`on`、`off` 读得进来；英文系统下用英文名。
- 排序和去重：
  - “蓝牙”的第一行是开关行，蓝牙设置页不出。
  - “深色”的第一行是开关。
  - “夜间模式”仍是夜灯的设置页。
  - “文件夹选项”仍出页面。
  - “dark”不压过名字就叫 dark 的应用（测试里造一个假应用）。
  - 弱命中封顶：造一个首字母也是“ly”的假应用，“ly”时它排在蓝牙开关前面；去掉这个应用，蓝牙开关排第一。
  - “蓝牙设置”“bluetooth settings”的第一行是“打开蓝牙设置”的命令行（`intent` 是 `OpenSettings`）。
- 引擎：
  - 全部范围里，“关蓝牙”的第一行带 `switchId` 和 `intent`。
  - 文件和内容范围里没有开关行。
  - 开始列表里没有开关行，即使历史里有。
  - 开始列表的 `pinnedPlaces` 生效，置顶行带着 `statusRow` 标记。

**`tst_switches`**（新）：用假后端，`Switches` 接收一个后端工厂。
- 用例：
  - 切换；已经是目标状态时不写入。
  - 失败后回滚，并给出错误文字。
  - 状态未知时按 Enter 会等。
  - 连按合并；过时的结果被丢弃。
  - `changed` 引起的 `dataChanged` 带对的角色列表。
  - 保持唤醒到点结束（时钟注入）和手动结束。
  - 电源请求走假接口，不真去挡睡眠。
- 如果链接 Qt6::Qml，加 `QT_QML_MODULE_NO_IMPORT_SCAN`（见 pitfalls“测试程序链接 Qt6::Qml，ctest 构建失败在 qml_imports”）。

**`wsbench --switches`**：只读，打印每个开关的状态和读取耗时，供真机核对。

**翻转测试不进 CTest**：只在真机验收脚本里做，并且像这次的探针一样，在同一个进程里改回。

## 5. 每个开关

### 5.1 深色模式（应用和系统一起）

**为什么一起切**：Windows 设置“选择模式”里的“深色 / 浅色”就是同时改这两个值，只改一个的叫“自定义”。用户说“深色模式”，指的是整台电脑。

**读**
- 位置：`HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` 的 `AppsUseLightTheme` 和 `SystemUsesLightTheme`（DWORD；没有这个值按 1，即浅色）。
- 两个都是 0：开（深色）。两个都是 1：关。不一样：混合，第二行写“自定义：应用浅色，系统深色”。

**写**（在开关线程上）
1. 两个值一起写（`RegSetKeyValueW`）。
2. 在界面线程上**先同步通知 Win顺 自己的顶层窗口**：按本进程枚举，`SendMessageTimeoutW(hwnd, WM_SETTINGCHANGE, 0, L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 200)`。Qt、托盘的单色图标（MessageWindow）、任务栏框（`TaskbarSearchBox::readTheme`）马上换。实测靠全局广播的话，自己的窗口要等 1.5–2.8 秒才轮到。
3. 广播线程发全局的 `WM_SETTINGCHANGE`（"ImmersiveColorSet"）。实测：
   - 只写注册表：约 1.6–2 秒内，本进程的窗口没收到 WM_SETTINGCHANGE，Qt 不变，任务栏时钟也不变（只有输入法的“中”变了色）。
   - 广播开始后 1.4 秒内的一张截图里，任务栏上的字已经变了；本进程的 Qt 窗口却到约 1.5 秒才收到消息。别的程序先变，自己最后，所以要有第 2 步。
   - 广播调用本身 2.5–3.8 秒才返回。

**混合时按 Enter**：以“应用”的值为准切到相反，两个值都写（应用窗口面积大，最显眼）。底栏写“Enter 全部切到深色”。

**已经是目标状态**：两个值都等于目标。

**Win顺 自己跟不跟着变**
- Win顺 的主题设成“跟随系统”时，`App::applyAppearance` 把 Qt 设成 `Qt::ColorScheme::Unknown`，Qt 就跟着系统走。
  - 探针实测：Qt 收到消息后 3–4 ms 发出 `colorSchemeChanged`；在 Win顺 里，这个信号接着就进 `App::applyTheme`。
  - App.cpp:700 只接这个信号，所以同一主题的消息再来一次，不会重跑 Mica 的修正。
- Win顺 自己的主题固定为深色或浅色时，它不跟着变（这是设计如此）。第一次切换时底栏提示一次：“Win顺 自己的主题是固定的，可在设置里改成跟随系统”。
- 整条链在 Win顺 里的端到端效果**待阶段 2 真机测**：这次探针运行时 Win顺 的界面没开。

**降级**
- 对比度主题开着（`SPI_GETHIGHCONTRAST` 的 `HCF_HIGHCONTRASTON`）：深浅色不起作用，Enter 打开 `ms-settings:easeofaccess-highcontrast`。
- 写完读回不是目标（被策略或别的程序改回去了）：报错“被系统改回去了”。

**权限**：HKCU，由界面进程（不提权）来写。**绝不经过 LocalSystem 服务**：服务看到的 HKCU 是 SYSTEM 账户的。

**启动器开着时整个主题翻转**：走的就是 pitfalls“切换深浅色后，Mica 还是旧主题的颜色”那条路。真机验收照那一条的测试步骤读 `DWMWA_USE_IMMERSIVE_DARK_MODE`，并截图核对。

### 5.2 显示隐藏的文件、显示文件扩展名

**读**：位置是 `HKCU\…\Explorer\Advanced`。

| 值 | 含义 | 没有这个值时 |
|---|---|---|
| `Hidden` | 1 = 显示隐藏的文件，2 = 不显示 | 按 2 |
| `HideFileExt` | 1 = 隐藏扩展名，0 = 显示 | 按 1 |

`ShowSuperHidden`（受保护的系统文件）不碰。

**写**（在开关线程上）：`SHGetSetSettings(&ss, SSF_SHOWALLOBJECTS 或 SSF_SHOWEXTENSIONS, TRUE)`。调用只要 1.6–1.8 ms。实测它一次做完三件事：
1. 写注册表：读回核对过 Hidden 1→2、HideFileExt 0→1。
2. 带出一条 WM_SETTINGCHANGE（"ShellState"）广播：调用后约 4 ms，本进程的窗口就收到了，不用自己再广播。
3. 打开的资源管理器窗口自己刷新：隐藏文件 358 ms，扩展名 742 ms。

**不用的办法**（都实测过）

| 办法 | 结果 |
|---|---|
| 只写注册表 | 3 秒内窗口不变 |
| 写注册表 + `SHChangeNotify(SHCNE_ASSOCCHANGED)` | 调用 163 ms，窗口照样不变 |
| 写注册表 + `IShellWindows` 逐个 `Refresh` | 能刷（62 ms），但要碰用户的每个窗口，没必要 |
| 写注册表 + 在工作线程上自己广播 "ShellState" | 也行：95 ms 刷新，广播 0.8–3.7 秒才返回。留作 `SHGetSetSettings` 失败时的备用 |

**其他细节**
- 读回 `SHELLSTATE` 的位域时，“真”是 -1（有符号的 1 位），判断要用 `!= 0`。
- **已经是目标状态**：注册表的值等于目标。
- **Ctrl+Enter**：`rundll32.exe shell32.dll,Options_RunDLL 7`，打开“文件资源管理器选项”的“查看”页。实测选中的是 3 页里的第 1 页（从 0 数），即“查看”；对话框开在 explorer.exe 里，rundll32 随即退出。
- 桌面上的隐藏文件（如 desktop.ini）会跟着出现或消失，这是 Windows 本身的行为。

### 5.3 蓝牙、WLAN

**沿用项目里现有的 C++/WinRT**：项目已经在用 SDK 自带的 C++/WinRT（`<winrt/…>`，`windowsapp` 已链接，见 src/app/CMakeLists.txt 里给 TextRecognizer 链接的那一行）。`TextRecognizer.cpp:176` 和 `Grabber.cpp:183` 都是在自己的线程上 `init_apartment(multi_threaded)`，再等结果。开关照这个做。

**准备**（在开关线程上，第一次用到时）
- `Radio::RequestAccessAsync()`：实测返回 Allowed，1.3–2.4 ms。
- `Radio::GetRadiosAsync()`：冷 8.8–17.7 ms，热 0.2–0.4 ms。
- 按 `Kind()` 分出 Bluetooth 和 WiFi。这台电脑上有两个：蓝牙（关）、WLAN（开，未连接）。

**读**：`radio.State()`，可能是 On、Off、Disabled、Unknown。
- Disabled 表示被硬件开关或固件关掉，降级为“被硬件开关关闭”。
- 同类有几个（比如内置的加一个 USB 蓝牙）：一起开关，任何一个开着就算开。

**写**：`radio.SetStateAsync(On 或 Off)`，用 `wait_for(5 s)` 等，返回 `RadioAccessStatus`。
- 实测蓝牙关→开 323.6 ms，开→关 18.0 ms。
- 同值写入也返回 Allowed（1.8–12.5 ms），不报错。所以要判断“已经是这个状态”，得自己先比较。

**通知**：`radio.StateChanged`。实测它比 `SetStateAsync` 完成早约 1 ms，不在调用的线程上（两次落在两个不同的线程上）。

**权限**：普通权限就能写。如果“让应用控制设备无线电”被关掉（`DeniedByUser`），就降级，Enter 打开 `ms-settings:privacy-radios`。

**WLAN 和蓝牙不同的地方**
- **不显示网络名**：24H2 起读 SSID 要位置权限。第二行只写“已连接”或“未连接”（用 `GetAdaptersAddresses` 看 802.11 网卡的 `OperStatus`，**待实测**）。
- **关掉会断网**：当前上网靠 WLAN 时，底栏写“Enter 关闭 WLAN（会断网）”。判断用 `NetworkInformation::GetInternetConnectionProfile().IsWlanConnectionProfile()`，它要不要位置权限**待实测**。不弹确认，再按一次就开回来。
- **打开时可能自动连网**：Windows 可能自动连上设为“自动连接”的已存网络，包括用户之前手动断开的那个。这是 Windows 本身的行为，和在快速设置里点一下一样。
- **写入这次没实测**：这台电脑的 WLAN 开着但没连，附近有一个设为自动连接的已存网络。关了再开可能把它连上，改变用户的网络，所以只做了同值写入。WLAN 和蓝牙用的是同一个 API、同一条路径。阶段 3 真机验收时，在用户在场的情况下翻一次再定。

**飞行模式开着时**：也能单独打开蓝牙或 WLAN（Windows 允许），飞行模式本身不变。

### 5.4 音量、静音（默认输出设备）

**端点**
- `IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)`。实测它和 eMultimedia 是同一个设备。
- `Activate(IAudioEndpointVolume)`：0.9 ms。
- 第二行写设备的友好名（`PKEY_Device_FriendlyName`，这台是“扬声器 (3- Realtek(R) Audio)”）。默认设备可能是 ToDesk、VB-Cable 这类虚拟设备（这台就装着），写出来才知道改的是哪个。

**读**：`GetMasterVolumeLevelScalar` 加 `GetMute`，一共 0.03 ms。显示 `qRound(level × 100)`，和 Windows 音量浮窗上的数一致。这台现在是 24%、静音。

**写**（在开关线程上，带 Win顺 自己的 event context GUID）
- 行上的开关表示“有声 / 静音”。不带动词的 Enter 切换静音。
- “音量 N”：`SetMasterVolumeLevelScalar(N / 100)`。正静音着的话同时取消静音：给了一个数，就是想听到这个音量。例：这台现在 24%、静音，输入“音量 20”后变成 20%、有声。
- “静音”“取消静音”：`SetMute`。
- Alt+←/→：每次 ±5，对齐到 5 的倍数（24 → 25 或 20）；往上加时取消静音。超出 0–100 就夹住，“音量 150”在底栏说明“最大 100”。

**已经是目标状态**
- 同值 `SetMute` 实测返回 **S_FALSE**（这是成功码），而且没有回调。判断用 `SUCCEEDED`，S_FALSE 就表示“已经是了”。
- 同值 `SetMasterVolumeLevelScalar` 返回 S_OK，同样没有回调。所以写之前自己比较，四舍五入后的整数相等就算已经是。

**通知**
- `OnNotify` 实测不在调用的线程上，也不固定（4 次回调落在 2 个线程上）。`guidEventContext` 认得出自己写的，用来跳过自己引起的回调。
- 收到 `OnDefaultDeviceChanged` 时，换端点、重新注册。

**降级**：没有输出设备（`E_NOTFOUND`），第二行写“没有声音输出设备”，Enter 打开 `ms-settings:sound`。

**音量浮窗**：程序改音量时，Windows 一般不弹音量浮窗。阶段 3 真机验收时确认。

### 5.5 保持唤醒

**由 Win顺 持有**：调用如下。
- `PowerCreateRequest`，`REASON_CONTEXT` 用简单字符串“Win顺：保持唤醒”。
- `PowerSetRequest(PowerRequestSystemRequired)`。
- `PowerSetRequest(PowerRequestDisplayRequired)`。

实测 `CallNtPowerInformation(SystemExecutionState)` 的变化：从 0 变成 0x3（系统 0x1、屏幕 0x2，两位互相独立）；清掉屏幕那项变成 0x1；全清回到 0。

**不用 `SetThreadExecutionState`**
- 它跟着调用的线程走，线程结束就失效，也带不了原因字符串。
- 电源请求是一个句柄。在 `powercfg /requests`（要管理员）里看得到“Win顺：保持唤醒”，进程退出时系统自动收回。

**默认让系统不睡、屏幕也不关**：说“保持唤醒”的人，多半是在等下载、做演示或开着远程连接；屏幕关了接着锁屏，同样碍事。只要求系统不睡的用法（“屏幕可以关”）先不做，见待定问题。

**时长**
- 不带时长：一直开着，直到手动结束、Win顺 退出或关机。
- 带时长或“到 18:00”：到点结束。时长可以是 1 分钟到 99 小时。
- 已经开着时再给一个时长：改结束时刻（底栏写“改为到 16:42”），不算“已经是了”。

**到点**
- 用一个单次定时器。
- 从睡眠恢复时，按墙上时间再核对一次，时间已过就立即结束。

**退出即结束**：析构时清掉请求、关掉句柄。进程意外结束时，系统也会收回。不跨重启记住。

**挡不住的**：合盖、按电源键照样睡眠；组策略的“计算机不活动限制”，以及设了“恢复时显示登录屏幕”的屏幕保护程序，可能照样锁屏（阶段 4 实测）。这些是 Windows 的规矩，不该去挡。没开时第二行写“电脑不睡眠、屏幕不关；合盖仍会睡眠”。

**Ctrl+Enter**：打开电源设置页。地址用索引里“电源、睡眠和电池设置”那一条的命令。这台电脑上 `wsbench --places` 给出的是 `ms-settings:batterysaver`；Win11 上 `ms-settings:powersleep` 是不是更准，阶段 4 核对（见 pitfalls“设置页面的 `ms-settings:` 地址不一定是 `PolicyIds` 的第一个”）。

### 5.6 第二批（需实测：每项先用探针出数字）

**亮度**
- 已知：
  - 内置屏用 WMI（`root\wmi`）。`WmiMonitorBrightness` 只读查过：读到 80，101 档。
  - 写用 `WmiMonitorBrightnessMethods.WmiSetBrightness`。
  - 外接屏要走 DDC/CI（dxva2 的 `SetMonitorBrightness`），慢，也不是每台都支持。
- 要实测：
  - 普通权限能不能写。
  - 外接屏的延迟。
  - 多屏时“亮度 50”改哪一块。推荐改启动器所在的屏。

**蓝牙耳机连接、电量**
- 已知：
  - 连接、断开可以用音频端点 KS 过滤器的 `KSPROPERTY_ONESHOT_RECONNECT` / `KSPROPERTY_ONESHOT_DISCONNECT`（ToothTray 就是这么做的）。
  - 电量在设备节点的属性 `{104EA319-6EE2-4701-BD47-8DDBF425BBE5},2` 上。
- 要实测：
  - 需要一副连着的耳机。这台配对了 HUAWEI FreeBuds 5，探针运行时没连着。
  - 普通权限是否可行。

**切换默认输出**
- 已知：要用 `IPolicyConfig`。这是未公开的接口，EarTrumpet、SoundSwitch 在用。
- 要实测：
  - 未公开的接口能不能过闸门。
  - 这台有三个输出可试：扬声器、ToDesk、VB-Cable。

**夜间模式（夜灯）**
- 已知：状态存在 CloudStore 的一个二进制块里。这台的块是 41 字节，第 18 字节是 0x13，表示关；网上的说法是 0x15 表示开。
- 要实测：块的格式未公开，要看改写它可不可靠。过不了闸门，就一直保持为设置页链接。

**飞行模式**
- 已知：公开 API 只能开关单个无线电，不能设飞行模式本身；能设它的是未公开的 `IRadioManager`。
- 要实测：大概率过不了闸门，保持为设置页链接。

**HDR**
- 已知：**有公开 API**，在 SDK 10.0.26100 里。
  - 读：`DisplayConfigGetDeviceInfo` + `DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2`。这台实测：支持 HDR、用户没开、当前是 SDR，0.01 ms。
  - 写：`DisplayConfigSetDeviceInfo` + `DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE`（24H2 起；更早的版本用 `SET_ADVANCED_COLOR_STATE`）。
  - 内置屏的友好名是空的，行上要写成“内置显示器”。
- 要实测：
  - 写入时屏幕会黑一下，要用户在场测。
  - 多屏时，是每块屏一行，还是只管启动器所在的屏。

## 6. 现代代码的规矩

**C++20 写法**
- `std::jthread` 配 `std::stop_token`。唯一的例外是广播线程，见 4.3。
- 时长和时刻用 `std::chrono`。
- “没有”用 `std::optional` 表示。
- `enum class`；结构体用指定初始化；用默认的 `operator==`。
- 解析和写入的返回值标 `[[nodiscard]]`。

**资源**
- 所有句柄都用 RAII 管：`winrt::com_ptr`、`winrt::handle`、`win32::UniqueKey`、`win32::UniqueHandle`。
- 不写裸的 `new`、`delete`、`Release()`。

**WinRT 和 COM**
- WinRT 一律用 SDK 自带的 C++/WinRT，不用 WRL 或 C++/CX。
- `winrt::hresult_error` 在后端的边界上接住，换成中文说明再交给界面。
- 经典 COM 回调（`IAudioEndpointVolumeCallback`、`IMMNotificationClient`）用 `winrt::implements<…>` 写（先包含 `unknwn.h`），不手写引用计数。

**线程**
- 界面线程上不阻塞：没有 `.get()`、`wait_for`、同步广播、`Sleep`。
- 跨线程一律用 `QMetaObject::invokeMethod(上下文对象, …, Qt::QueuedConnection)`。
- 事件驱动，不轮询：注册表、无线电、音频都有通知可用，不设周期定时器。

**字符串**
- 项目开着 `QT_NO_CAST_FROM_ASCII`，字面量写 `u"…"_s`。
- 解析时用 `QStringView` 切片，不拷贝。
- 常量词表用 `constexpr`。

**结构**
- 解析放在 core，是纯函数，用表驱动测试。
- 后端放在 platform，经一个窄接口交给 `Switches`；测试时换成假的。
- 头文件里不写 `using namespace`。
- 小心 Windows 头文件的宏：`small`、`near`、`far`（pitfalls 里已经记过）。

**文字**
- 新的界面文字都过 `tr()` 或 `qsTr()`，英文写进 winshun_en.ts，复数用 `%n`，英文写短。

## 7. 实施顺序（按子代理切分）

每个阶段做完，程序都是能用的。改同一批文件的阶段要串行，不能并行。

**阶段 0：探针**：本次已做完，数字见下表。

**阶段 1：core**（sonnet）
- places.txt：加七个条目，挪关键词。
- `SystemCatalog`：认 `switch`、`on`、`off`；按开关去重，同命令的页面行让位。
- 新文件 `SwitchCommand`；`SearchResult` 加两个字段。
- `SearchEngine`：命令行和 `pinnedPlaces`。
- `tst_core` 的用例。
- 做完时开关行已经出现，Enter 仍像今天一样打开设置页。

**阶段 2：骨架 + 三个注册表开关**（深色、隐藏文件、扩展名）
- 后台：`ComWorker` 的 MTA 选项、`Switches`、`SettingBroadcast`、`SwitchTheme`、`SwitchExplorer`。
- `ResultModel` 的角色。
- `Launcher`：开关分支、底栏、`handleShown` 时刷新、多选跳过开关行、Enter 的自动重复。
- `ResultRow` 的行尾；`ToggleSwitch` 的混合、不知道、进行中三种样子。
- `--dump` 的字段；`tst_switches`；英文。
- 分工：线程、广播、退出这部分用 opus（max），其余用 sonnet。Launcher、ResultModel、ResultRow、Main.qml 这几个文件会被多个任务改到，要串行。
- 做完后真机验收（见下）。

**阶段 3：蓝牙、WLAN、音量**（opus，max）
- `SwitchRadios`、`SwitchAudio`；通知、设备变化；Alt+←/→；音量行的行尾。
- 真机验收。WLAN 的写入要在用户在场时测。

**阶段 4：保持唤醒**（sonnet）
- `KeepAwake`：到点结束、睡眠恢复后核对。
- 托盘；空搜索框的置顶行。
- 真机验收。

**阶段 5：文档**（sonnet）
- architecture.md 加“系统开关”一节；更新使用说明.md。
- pitfalls.md 补上 10.2 的新坑。
- README 的功能列表。

**阶段 6：第二批的探针**（opus，max）：先出一张数字表，再决定做哪些。

### 真机验收（每个阶段）

照 pitfalls“界面实测（模拟真实输入）”的规矩做：

- **部署**：从 dist 测。旧版留着，新版起不来就自动换回去。测完让新版留着运行。
- **发键**：用提权的 SendInput。每次发键前确认前台是 Win顺。不发 Ctrl+1/2/3。
- **填搜索框**：用 `--query "关蓝牙"`，绕开输入法。
- **读结果**：
  - 用 `--dump` 读每行的开关状态和底栏提示。
  - 系统状态另用只读的方式核对：注册表、`wsbench --switches`、`CallNtPowerInformation`。
- **还原**：改过的系统状态和设置，在同一个脚本进程里改回。

测试矩阵：

| 维度 | 要测的 |
|---|---|
| 每个开关 × 操作 | 普通 Enter；带动词；动词要的状态已经是了；Ctrl+Enter；单击开关；双击开关；进行中按 Esc；按住 Enter |
| 误按 | 历史里有用过的开关、保持唤醒开着时，打开窗口直接按 Enter：哪个开关都不变，打开的是最近打开的第一条 |
| 深浅色 | 启动器开着时切换，按 Mica 那条坑的步骤测 |
| 缩放 | 100% 和 150%；行尾对齐到看得见的边 |
| 界面语言 | 英文界面 |

### 阶段 0 实测结果（2026-10-19）

**环境**
- Windows 10.0.26200；单屏 2560×1600，缩放 150%；普通权限。
- MSVC 2022 + SDK 10.0.26100 + Qt 6.12.0。
- TranslucentTB 开着。用户的 Win顺 界面没在运行，只有 WinShunService。

**探针源码**：在会话临时目录 `C:\Users\young\AppData\Local\Temp\claude\f--------\77112d2e-05b6-4f68-8680-01d3a161404a\scratchpad\switch-probe\` 里（`switchprobe.cpp`、`themeprobe.cpp`），不进仓库。

**系统入口的现状**（`wsbench --places`）：一共 1231 条，读取 481 ms。

| 词 | 今天的第一行 |
|---|---|
| 深色模式 | “浅色主题设置”（50 分） |
| 深色 | “颜色滤镜设置”（30 分） |
| 隐藏文件、扩展名 | 都是“文件资源管理器选项”（50 分） |
| 静音 | “更改屏幕指示器的位置”（30 分） |
| 夜间模式 | 夜灯（显示设置，115 分） |
| 蓝牙、wifi、音量、睡眠 | 各自对应的设置页 |

**深浅色**

| 项目 | 结果 |
|---|---|
| 只写注册表 | 约 1.6–2 秒内，本进程的窗口一条 WM_SETTINGCHANGE 都没收到，Qt 不变；任务栏时钟和大部分托盘图标不变（输入法的“中”变灰了） |
| 广播调用的耗时 | `SendMessageTimeout(HWND_BROADCAST, SMTO_ABORTIFHUNG, 1000)` 四次分别用了 2764、3821、2501、3108 ms 才返回 |
| 自己的窗口何时收到 | 隐藏的 Qt 窗口在广播开始后 1.5–2.8 秒才收到 |
| Qt 的反应 | 收到后 3–4 ms 发出 `colorSchemeChanged`，Light、Dark 两个方向都对 |
| 任务栏 | 广播开始后 1.4 秒内的截图里，任务栏时钟已经是浅色主题的深色字；那时本进程的 Qt 窗口还没收到消息（约 1.5 秒才收到） |

**无线电**

| 项目 | 结果 |
|---|---|
| 权限 | `RequestAccessAsync` 返回 Allowed，1.3–2.4 ms；HKCU 的 `ConsentStore\radios` = Allow（没有 NonPackaged 子键），HKLM 也是 Allow |
| 枚举 | `GetRadiosAsync` 冷 8.8–17.7 ms（三次），热 0.2–0.4 ms；`FindAllAsync(GetDeviceSelector())` 14–15 ms；共两个：蓝牙（关）、WLAN（开，未连接） |
| 同值写入 | Allowed，1.8–12.5 ms |
| 蓝牙开关 | 关→开 323.6 ms，开→关 18.0 ms |
| StateChanged | 比写入完成早约 1 ms；不在调用的线程上，两次落在两个不同的线程上 |

**音量**

| 项目 | 结果 |
|---|---|
| 默认输出 | eConsole 和 eMultimedia 是同一个：“扬声器 (3- Realtek(R) Audio)” |
| 初始化耗时 | 枚举器 1.8 ms，默认端点 1.9 ms，`Activate` 0.9 ms |
| 读 | 音量加静音一共 0.03 ms |
| 当前状态 | 24%、静音；51 档，-65.2 到 0 dB；硬件支持标志 0x7 |
| 同值写音量 | S_OK，没有回调 |
| 音量 +1% | 1.3 ms，回调带着自己的 GUID；改回 0.6 ms |
| 同值写静音 | **S_FALSE**，没有回调 |
| 取消静音 | 1.9 ms，有回调 |
| 回调线程 | 不在调用的线程上，也不固定：4 次回调落在 2 个线程上 |

**隐藏文件、扩展名**

| 项目 | 结果 |
|---|---|
| 只写注册表 | 3 秒内窗口不变 |
| + `SHChangeNotify(ASSOCCHANGED)` | 调用 163 ms，3 秒内窗口不变 |
| + 在工作线程上自己广播 "ShellState" | 95 ms 后窗口变了；广播 782 ms 才返回。在界面线程上同步发的那次要 3709 ms，测量循环一次没跑，那次不算 |
| + 只刷自己窗口的 `IWebBrowser2::Refresh` | 62 ms |
| `SHGetSetSettings`，隐藏文件 | 调用 1.8 ms；注册表 Hidden 1→2；约 4 ms 后本进程的窗口收到 "ShellState"（我们没广播）；窗口 358 ms 后刷新 |
| `SHGetSetSettings`，扩展名 | 1.6 ms；HideFileExt 0→1；同样约 4 ms 后收到 "ShellState"；742 ms 后窗口里的名字不带扩展名了 |
| 测试用的窗口 | 用 `ShellExecuteEx("opennewwindow")` 打开，490–545 ms 后出现在 ShellWindows 里，是一个新框架 |
| 文件夹选项 | `rundll32 shell32.dll,Options_RunDLL 7`：121 ms 后出现“文件资源管理器选项”，选中的是第 1 页（共 3 页，从 0 数，即“查看”）；窗口属于 explorer.exe |

**保持唤醒**

| 项目 | 结果 |
|---|---|
| 系统 + 屏幕两项请求 | SystemExecutionState = 0x3 |
| 清掉屏幕那项 | 0x1 |
| 全清 | 0 |
| `SetThreadExecutionState(连续\|系统\|屏幕)` | 同样是 0x3 |

**只读的几项**

| 项目 | 结果 |
|---|---|
| HDR | `GET_ADVANCED_COLOR_INFO`：支持、未开。`_INFO_2`：hdrSupported=1、hdrUserEnabled=0、activeColorMode=0（SDR），0.01 ms。内置屏的友好名为空 |
| 亮度 | `WmiMonitorBrightness`：80，101 档，Active；`WmiMonitorBrightnessMethods` 存在 |
| 夜灯 | CloudStore 的 `bluelightreductionstate`，41 字节，第 18 字节 0x13（关） |

### 探针改过的系统状态（都在探针进程里改回，最终值已核对）

| 改了什么 | 用户可能看到的 | 最终值（读回核对） |
|---|---|---|
| 蓝牙：关 → 开 → 关 | 蓝牙开了约 0.5 秒 | 关（另起一个进程复读确认） |
| 音量：24% → 25% → 24% | 每步停 0.4 秒 | 24% |
| 静音：开 → 关 → 开（只在没有声音在放时做） | 取消静音约 0.3 秒 | 静音 |
| “隐藏的项目”：显示 ↔ 不显示，一共 6 次 | 所有打开的资源管理器窗口和桌面会闪一下 | Hidden=1（原值） |
| “文件扩展名”：显示 ↔ 隐藏，1 次 | 同上 | HideFileExt=0（原值） |
| 深浅色：两次把应用和系统都切到浅色，再切回深色 | 第一次浅色约 7 秒，第二次约 5 秒 | 两个值都是 0（原值）。第二次拍了几张整屏截图看任务栏，看完已删除 |
| 打开又关掉一个“文件资源管理器选项”对话框；开过、关掉两个测试用的资源管理器窗口（会话临时目录里的 `exptest`） | 窗口闪过 | 已关 |
| `CoCreateInstance(ShellBrowserWindow)` 失败，但在 explorer 里留下两个看不见的“库”窗口 | 看不见 | 已用 `Quit()` 关掉；之后核对过，只剩用户原来的三个资源管理器窗口 |
| 电源请求 | 只存在了几毫秒 | 已清掉，SystemExecutionState 回到 0 |

WLAN 只做了同值写入，状态没变。亮度、夜灯、HDR 只读，没有改。

## 8. 对已定方案的修正

1. **“升级现有的页面行”行不通，因为页面是共用的，有的名字也不对。**
   - “文件资源管理器选项”一页同时管隐藏文件和扩展名。
   - “颜色”页还管强调色和透明效果；“声音”“显示”页也都是好几样东西的合集。
   - “深色模式”这个词今天落在名叫“浅色主题设置”的那一条上。
   - 改成：每个开关在 places.txt 里是一个新条目，有自己的概念名和关键词，Ctrl+Enter 打开页面。开关行出现时，同命令的页面行让位。“一个概念一行”照样成立。
2. **普通的 ←/→ 不能用来调音量。** 输入框要用它们挪光标（`Main.qml` 只在常用应用那一排时才接 ←/→）。改用 Alt+←/→。
3. **“夜间模式”在中文 Windows 里指夜灯**（显示设置里的那一项，`wsbench` 给了 115 分），不是深色模式。深色的词表不收它；夜灯放在第二批。
4. **HDR 有公开 API**（SDK 10.0.26100 的 DisplayConfig 高级颜色接口，以及 `SET_HDR_STATE`）。第二批里这一项要做的只剩“实测写入”，不用再查 API 是否公开。
5. **底栏不能只写“打开”或“关闭”。** Win顺 里的“打开”本来指打开文件，右边还有“Esc 关闭”。Enter 的提示要写目标状态，如“关闭蓝牙”“切换到浅色”。
6. **静音、深色、浅色、mute 是“设成”词，不是“切换”词。** 输入“静音”再按 Enter，不该把声音打开；“关闭深色”= 设为浅色。
7. **单个状态词不能强行置顶。** “dark”“light”“mute”可能是应用名的一部分。只有带动词或数值的命令才置顶。
8. **资源管理器的刷新不用另找办法。** `SHGetSetSettings` 一次就写好注册表、发出广播、让窗口刷新。实测 `SHChangeNotify` 不起作用；用 `IShellWindows` 逐个刷新也没必要。
9. **广播 ImmersiveColorSet 要两三秒。** 实测 2.5–3.8 秒才返回，自己的窗口 1.5–2.8 秒后才收到。所以必须放在单独的线程上发，退出时不等它；Win顺 自己的窗口先同步通知。
10. **保持唤醒用电源请求**，不用 `SetThreadExecutionState`，理由见 5.5。
11. **WLAN 的写入还没实测**，在第一批里先挂着，阶段 3 用户在场时翻一次再定。另外，WLAN 显示不了网络名（读 SSID 要位置权限）。

## 9. 待定问题（附推荐）

1. **以前“蓝牙 + Enter”打开设置页，现在会把蓝牙关掉。**
   推荐：接受这个变化。底栏事先写清“Enter 关闭蓝牙”；第一次翻转后提示一次“要打开设置页：Ctrl+Enter”；再按一次 Enter 就能切回来。打“蓝牙设置”也直接出设置页那一行（3.1 第 7 条）。
2. **深色模式处于混合（自定义）状态时，Enter 做什么？**
   推荐：按“应用”的值切到相反，两个值都写。底栏写“Enter 全部切到深色”。
3. **保持唤醒要不要让屏幕也常亮？**
   推荐：默认两样都要（系统和屏幕）。“屏幕可以关”等有人要了再加，做成命令后缀（“保持唤醒 屏幕可关”），不做成设置项。
4. **保持唤醒的默认时长？**
   推荐：不带时长就一直开着，直到手动结束或 Win顺 退出。
5. **托盘图标要不要加一个小点，表示保持唤醒开着？**
   推荐：第一版不加，只用托盘提示文字和打勾的菜单项。单色图标的墨色另有一套规则，改图标要单独评估。
6. **托盘菜单要不要常驻一个“保持唤醒”入口？**
   推荐：不常驻，只在开着时出现，用来结束它。
7. **数字键设成 Alt 时，Alt+←/→ 会不会冲突？** 按住 Alt 会显示数字角标。
   推荐：照用 Alt+←/→。实现时核对 `NumberHints.qml` 的触发条件；如果会闪出角标，就在方向键按下时收起角标。
8. **音量每步调多少？**
   推荐：Alt+←/→ 固定每步 5，不加 Shift 细调。滚轮不接管，滚轮是滚列表用的。
9. **音量行上的 Enter 做什么？**
   推荐：切换静音。行上的开关表示的就是“有声 / 静音”。
10. **关 WLAN 会断网时，要不要先确认？**
    推荐：不确认，底栏写“（会断网）”；再按一次就开回来。
11. **同类有几个无线电时怎么办？**
    推荐：一起开关，任何一个开着就算开。
12. **Win顺 自己的主题不是“跟随系统”时怎么办？**
    推荐：照常切换 Windows 的主题；第一次切换时提示一句怎么改成跟随。
13. **开关行进不进“最近打开”？**
    推荐：不进开始列表。打开窗口顺手按 Enter 会作用在第 0 行，刚切过的开关排在那里就会被切回去（见 2.6）。用过的次数照样记下，给打字搜索时的排序加分。保持唤醒开着时的置顶行是例外，但它不做默认选中行。
14. **按 Enter 之后，要不要把搜索框里的字全部选中？**
    推荐：全选。接着打字就是下一条命令，Enter 仍作用在当前行。
15. **AI（MCP）能不能开关这些？**
    推荐：第一版不开放。以后可以加一个只读的 `switch_states`；要写入的话，每次都要用户确认。
16. **插件能不能注册开关？**
    推荐：以后再说，先把后端接口用稳。
17. **“到 18:00”这种写法第一版就支持吗？**
    推荐：支持。解析成本低，“下班前别让电脑睡”是常见用法。
18. **WLAN 第二行要不要显示“已连接 / 未连接”？**
    推荐：显示，用 `GetAdaptersAddresses` 判断，不显示网络名，也不去碰位置权限。

## 10. 查过的坑

### 10.1 docs/pitfalls.md 里相关的

行号按 2026-10-19 写这份草案时的文件。别的会话还在往里加条目，行号会挪，以标题为准。

**缩放、主题、外观**
- “先想清楚：屏幕缩放和分辨率”（L7）：开关的尺寸用逻辑像素，位置落到整物理像素上（ResultRow 的 `onPixel`），按看得见的边对齐。
- “切换深浅色后，Mica 还是旧主题的颜色”（L250）：启动器开着时会遇到整个主题翻转。`applyTheme` 只接 `colorSchemeChanged`，同一主题的消息再来不会重跑修正（已核实，App.cpp:700）。真机验收照这一条的测试步骤做。
- “名为 `onXxx` 的属性不随主题变化”（L1081）：新的 QML 属性别以 `on` 开头命名（比如开关的“开”别叫 `onState`）。

**系统入口和排序**
- “设置页面的 `ms-settings:` 地址不一定是 `PolicyIds` 的第一个”（L300）：Ctrl+Enter 的地址用 `wsbench --places` 核对。
- “中文 Windows 上搜‘计算器’找不到计算器”（L312）：开关条目自带中英两种名字和关键词，不依赖系统语言的清单。
- “补充的关键词要加在页面本身那一条上”（L319）：开关是新条目。挪走关键词时，核对页面本身那一条仍能搜到。
- “循环里现建 `QRegularExpression` 很慢”（L325）：语法用常量表，不现建正则。
- “搜 ‘vscode’ 找不到 Visual Studio Code”（L2716）、“搜‘微信’‘ps’‘cmd’，要的应用不在第一”（L2721）：应用本来就容易被挤下去，所以单个状态词和主语词不置顶，只有带动词或数值的命令才置顶。

**写注册表后要通知**
- “改 StartAllBack 的注册表，任务栏不跟着变”（L1073）和“改了 PrtScn 的注册表值不广播…”（L2595）：同一个教训，写完注册表要通知。这次深浅色的实测也是这样。

**托盘**
- “托盘菜单第一次打开慢”（L91）：新加的菜单项走同一条预热路径。
- “测试脚本：托盘菜单不认发来的方向键，通知盖住菜单”（L2681）：测托盘里的“保持唤醒”一项时，按文字找菜单项、用鼠标点。

**按键**
- “剪贴板页里 Alt+1、Alt+2 没反应”（L694）：Alt+←/→ 也可能被别的程序注册成全局热键，测试前先用 `RegisterHotKey` 试一下。
- “转给 Qt 的 Alt 按下，`event.modifiers` 里没有 Alt”（L739）：启动器不激活、键盘经 `KeyRouter` 转来时，Alt+←/→ 的修饰键照那一条处理。

**列表和行**
- “快速连点，第二下没有 `clicked`”（L1143）：双击不能变成翻两次，在处理函数里按时间去重。
- “在当前行上面插入一行，ListView 的当前行跟着往下走”（L1172）、“在第 0 行插入提示行，ListView 的光标跟着原来的行下移”（L1826）：命令行置顶、保持唤醒的置顶行出现时，当前行要停在第 0 行。
- “行里的 `HoverHandler` 永远是 `hovered: false`”（L1196）：行尾开关的悬停，用 `area` 的坐标来算。
- “没出帧，动画就停在半路”（L1755）：再显示时跳到终值。
- “插件行的 Enter 不一定是‘打开’，底栏提示要跟着当前行”（L1838）：开关行也一样。
- “结束不了提权的进程：插件以用户身份跑，别把英文异常原样丢到底栏”（L1850）：错误一律写成中文，HRESULT 只进日志。

**线程和后台开销**
- “什么都不做时也有线程在醒”（L1496）、“主线程在常用操作里卡不卡”（L1503）：不轮询；改完后用那里的探针把主线程量一遍。
- “不带上下文对象 connect 引擎的信号，槽在引擎的工作线程上跑”（L1733）：所有回调都带上下文对象。
- “`Qt::UniqueConnection` 连 lambda 不生效”（L2424）：`Switches::changed` 接 lambda 时不加它。

**QML 和 C++ 接口**
- “qmltypes 里指针属性的类型名要带 `ws::`”（L1767）：把 `Switches*` 暴露给 QML 时注意。
- “`required property bool enabled` 遮住了 Item 自己的”（L1773）：角色名避开 `state`、`enabled`、`visible`。

**设置**
- “设置窗口的副本把接管前的 PrtScn 值冲掉”（L2629）：开关的状态不存进 Settings，没有副本问题。

**测试**
- “界面实测（模拟真实输入）”（L1510 起）：真机验收的规矩全照这一节。
- “测不提权的 Win顺：配置在 `<profile>\WinShun\WinShun.ini`”（L2075）：无线电、HKCU 都要在普通权限下测。
- “测试：量悬停时间别忘了 `--dump` 本身要 270 ms”（L2549）：用 `--dump` 量开关多快变过去时，扣掉它自己的耗时。
- “测试程序链接 Qt6::Qml，ctest 构建失败在 `qml_imports`”（L2634）、“新加的测试程序，ctest 报 `0xc0000135`”（L2525）：`tst_switches` 是新测试程序，两条都要照做。
- “Qt 测试程序在 PowerShell 工具里一行输出都没有”（L2229）：跑测试时加 `-o 文件,txt`。

**编译和翻译**
- “`small` 是 Windows 头文件里的宏”（L2223）、“Windows 头文件把参数名 small 展开成 char”（L2573）、“`windows.h` 把 `near`、`far` 定义成了空宏”（L1359）：新文件包含 mmdeviceapi、endpointvolume、WinRT 的头文件时要注意。
- “lupdate 把 QML 字符串里的反斜杠吃掉”（L2690）、“lupdate 读错 QML 字符串里的反斜杠”（L1456）：文案里的路径层级用“›”，不用反斜杠。
- “从 Git Bash 调 `powershell.exe` 编译，构建目录被弄坏”（L1280）、“从 Bash 里编译，新头文件的路径变成问号”（L1304）、“从 bash 起的 pwsh 里跑 ninja…”（L2129）：在 PowerShell 工具里编译。

### 10.2 这次新发现的（阶段 5 补进 pitfalls.md）

1. **只写 Personalize 注册表，几乎没人知道**：本进程收不到 WM_SETTINGCHANGE，Qt 不变，任务栏时钟也不变（只有输入法指示器变了色）。要自己广播 "ImmersiveColorSet"。
2. **这个广播同步发要 2.5–3.8 秒**（SMTO_ABORTIFHUNG，超时 1000 ms）：
   - 在界面线程上发，就会卡住几秒。
   - 自己的窗口 1.5–2.8 秒后才轮到，所以要先直接通知自己的窗口。
3. **在 STA 线程上同步广播，会让测量落空**：`SendMessageTimeout` 返回之前，测量循环一次都没跑，当时得出了“刷新不了”的假结论（M4）。要量广播的效果，就在别的线程上发。
4. **同值写入静默成功**：同值 `SetMute` 返回 S_FALSE（成功码），没有回调；同值 `SetMasterVolumeLevelScalar` 返回 S_OK，也没有回调。
5. **只写 Explorer\Advanced 不会刷新窗口**，再加 `SHChangeNotify(SHCNE_ASSOCCHANGED)` 也不会。改用 `SHGetSetSettings`：它写注册表，也带出 "ShellState" 广播，窗口自己刷新。
6. **`SHELLSTATE` 的位域读回来，“真”是 -1。**
7. **`CoCreateInstance(ShellBrowserWindow)` 会留下隐形窗口**：在这台电脑上它返回 E_FAIL，却在 explorer 里留下两个看不见的“库”窗口，要用 `Quit()` 关掉。
   - 测试时开自己的资源管理器窗口，用 `ShellExecuteEx` 的 "opennewwindow" 动词。
   - 按路径找到窗口后，核对它的 HWND 不是用户原有的框架（Win11 的标签页共用一个 HWND）。
8. **TranslucentTB 开着时，任务栏取色量不出深浅色的变化**：取到的是透明背后的壁纸。要看时钟和图标的字色。
9. **`Options_RunDLL 7` 的对话框开在 explorer.exe 里**，rundll32 马上就退出了。按 rundll32 的进程号找不到这个窗口。
10. **重开 WLAN 的无线电可能自动连网**：会连上设为自动连接的已存网络，包括用户手动断开的那个。所以测试要用户在场。
11. **无线电和音频的回调不在调用的线程上，落在哪个线程也不固定**：一律带上下文对象转回界面线程。
