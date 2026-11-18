<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="en_US" sourcelanguage="zh_CN">
<context>
    <name>DevTools</name>
    <message>
        <location filename="../DevMain.cpp" line="361"/>
        <source>JSON 有错误：%1（第 %2 行第 %3 列）</source>
        <translation>JSON error: %1 (line %2, column %3)</translation>
    </message>
    <message>
        <location filename="../DevMain.cpp" line="396"/>
        <source>这段文字不是有效的 JSON</source>
        <translation>This text is not valid JSON</translation>
    </message>
</context>
<context>
    <name>Json</name>
    <message>
        <location filename="../Json.cpp" line="190"/>
        <source>嵌套太深（超过 %1 层）</source>
        <translation>Nested too deeply (more than %1 levels)</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="193"/>
        <source>这里应该有一个值，但内容已经结束</source>
        <translation>A value is expected here, but the text ends</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="210"/>
        <source>这里应该是一个值：对象、数组、字符串、数字、true、false 或 null</source>
        <translation>A value is expected here: object, array, string, number, true, false, or null</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="225"/>
        <location filename="../Json.cpp" line="253"/>
        <source>对象没有结束，缺少 }</source>
        <translation>Object is not closed: missing }</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="242"/>
        <source>这里应该是一个键，用双引号括起来</source>
        <translation>A name is expected here, in double quotes</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="246"/>
        <source>键“%1”后面缺少冒号</source>
        <translation>Missing colon after name &quot;%1&quot;</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="263"/>
        <location filename="../Json.cpp" line="299"/>
        <source>这一行末尾缺少逗号</source>
        <translation>Missing comma at the end of this line</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="264"/>
        <source>缺少逗号或 }</source>
        <translation>Missing comma or }</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="289"/>
        <source>数组没有结束，缺少 ]</source>
        <translation>Array is not closed: missing ]</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="300"/>
        <source>缺少逗号或 ]</source>
        <translation>Missing comma or ]</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="338"/>
        <source>\u 后面要有 4 位十六进制数</source>
        <translation>\u must be followed by 4 hex digits</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="346"/>
        <source>\x 后面要有 2 位十六进制数</source>
        <translation>\x must be followed by 2 hex digits</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="353"/>
        <source>转义写法不对：\%1</source>
        <translation>Invalid escape: \%1</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="360"/>
        <location filename="../Json.cpp" line="365"/>
        <source>字符串没有结束，缺少 %1</source>
        <translation>String is not closed: missing %1</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="362"/>
        <source>字符串里的控制字符要写成转义</source>
        <translation>Control characters in a string must be escaped</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="387"/>
        <location filename="../Json.cpp" line="405"/>
        <source>数字写法不对</source>
        <translation>Invalid number</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="395"/>
        <source>数字写法不对：小数点后面要有数字</source>
        <translation>Invalid number: digits are required after the decimal point</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="402"/>
        <source>数字写法不对：指数要有数字</source>
        <translation>Invalid number: the exponent needs digits</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="430"/>
        <source>“%1”不是 JSON 的值，文字要用引号括起来</source>
        <translation>&quot;%1&quot; is not a JSON value; text must be in quotes</translation>
    </message>
    <message numerus="yes">
        <location filename="../Json.cpp" line="567"/>
        <source>从 %n 行日志里各取出一段 JSON，合成数组显示</source>
        <translation>
            <numerusform>Extracted JSON from %n log line and combined it into an array</numerusform>
            <numerusform>Extracted JSON from each of %n log lines and combined them into an array</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../Json.cpp" line="568"/>
        <source>共 %n 行，每行一段 JSON，合成数组显示</source>
        <translation>
            <numerusform>%n line, one JSON per line, shown as an array</numerusform>
            <numerusform>%n lines, one JSON per line, shown as an array</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../Json.cpp" line="570"/>
        <source>%n 行不是 JSON，没有读</source>
        <translation>
            <numerusform>%n line is not JSON and was skipped</numerusform>
            <numerusform>%n lines are not JSON and were skipped</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="588"/>
        <source>去掉了外面的引号和转义</source>
        <translation>Removed the outer quotes and escaping</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="596"/>
        <source>按宽松写法读取：%1</source>
        <translation>Read with lenient syntax: %1</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="676"/>
        <source>内容太大，最多读 %1 MB</source>
        <translation>Too large: at most %1 MB can be read</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="699"/>
        <source>去掉了转义（\&quot;）</source>
        <translation>Removed escaping (\&quot;)</translation>
    </message>
    <message numerus="yes">
        <location filename="../Json.cpp" line="721"/>
        <source>跳过了前面 %n 个字符</source>
        <translation>
            <numerusform>Skipped the first %n character</numerusform>
            <numerusform>Skipped the first %n characters</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../Json.cpp" line="723"/>
        <source>后面还有 %n 个字符没有读</source>
        <translation>
            <numerusform>%n more character was not read</numerusform>
            <numerusform>%n more characters were not read</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="727"/>
        <source>值的后面还有多余的内容</source>
        <translation>There is extra content after the value</translation>
    </message>
    <message numerus="yes">
        <location filename="../Json.cpp" line="781"/>
        <source>对象 · %n 个键</source>
        <translation>
            <numerusform>object · %n key</numerusform>
            <numerusform>object · %n keys</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../Json.cpp" line="782"/>
        <source>数组 · %n 项</source>
        <translation>
            <numerusform>array · %n item</numerusform>
            <numerusform>array · %n items</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../Json.cpp" line="783"/>
        <source>字符串 · %n 个字</source>
        <translation>
            <numerusform>string · %n character</numerusform>
            <numerusform>string · %n characters</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="784"/>
        <source>数字</source>
        <translation>number</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="785"/>
        <source>布尔值</source>
        <translation>boolean</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="795"/>
        <source>注释</source>
        <translation>comments</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="797"/>
        <source>单引号</source>
        <translation>single quotes</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="799"/>
        <source>没加引号的键</source>
        <translation>unquoted names</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="801"/>
        <source>多余的逗号</source>
        <translation>trailing commas</translation>
    </message>
    <message>
        <location filename="../Json.cpp" line="806"/>
        <source>、</source>
        <translation>, </translation>
    </message>
</context>
<context>
    <name>JsonFile</name>
    <message>
        <location filename="../JsonTool.cpp" line="30"/>
        <source>打不开“%1”：%2</source>
        <translation>Cannot open &quot;%1&quot;: %2</translation>
    </message>
    <message>
        <location filename="../JsonTool.cpp" line="35"/>
        <source>“%1”有 %2，太大了：最多 32 MB</source>
        <translation>&quot;%1&quot; is %2, too large: the limit is 32 MB</translation>
    </message>
</context>
<context>
    <name>JsonRow</name>
    <message numerus="yes">
        <location filename="../qml/JsonRow.qml" line="134"/>
        <source>%n 个键</source>
        <translation>
            <numerusform>%n key</numerusform>
            <numerusform>%n keys</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../qml/JsonRow.qml" line="134"/>
        <source>%n 项</source>
        <translation>
            <numerusform>%n item</numerusform>
            <numerusform>%n items</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>JsonTextPane</name>
    <message>
        <location filename="../qml/JsonTextPane.qml" line="73"/>
        <source>JSON 原文</source>
        <translation>JSON text</translation>
    </message>
    <message>
        <location filename="../qml/JsonTextPane.qml" line="87"/>
        <source>在这里粘贴或输入 JSON</source>
        <translation>Paste or type JSON here</translation>
    </message>
    <message>
        <location filename="../qml/JsonTextPane.qml" line="120"/>
        <source>原文有 %1，太长了，这里不显示</source>
        <translation>The text is %1, too long to show here</translation>
    </message>
    <message>
        <location filename="../qml/JsonTextPane.qml" line="129"/>
        <source>右边照样能查看、搜索；“复制”菜单能复制格式化或压缩后的全部内容</source>
        <translation>You can still view and search it on the right; the &quot;Copy&quot; menu copies the whole content, formatted or minified</translation>
    </message>
</context>
<context>
    <name>JsonToolbar</name>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="78"/>
        <source>粘贴</source>
        <translation>Paste</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="79"/>
        <source>换成剪贴板里的内容（Ctrl+V）</source>
        <translation>Replace with the clipboard contents (Ctrl+V)</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="84"/>
        <source>格式化</source>
        <translation>Format</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="86"/>
        <source>原文换成缩进排好的 JSON</source>
        <translation>Replace the text with indented JSON</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="91"/>
        <source>压缩</source>
        <translation>Minify</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="93"/>
        <source>原文换成一行，去掉空白</source>
        <translation>Replace the text with a single line, without whitespace</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="106"/>
        <source>全部展开</source>
        <translation>Expand all</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="112"/>
        <source>全部折叠</source>
        <translation>Collapse all</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="118"/>
        <source>隐藏原文</source>
        <translation>Hide text</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="118"/>
        <source>显示原文</source>
        <translation>Show text</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="167"/>
        <location filename="../qml/JsonToolbar.qml" line="190"/>
        <source>搜索键名和值</source>
        <translation>Search names and values</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="204"/>
        <source>无结果</source>
        <translation>No results</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="216"/>
        <source>上一个（Shift+Enter）</source>
        <translation>Previous (Shift+Enter)</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="217"/>
        <source>下一个（Enter）</source>
        <translation>Next (Enter)</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="242"/>
        <source>复制</source>
        <translation>Copy</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="252"/>
        <source>复制格式化的 JSON</source>
        <translation>Copy formatted JSON</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="253"/>
        <source>复制压缩的 JSON</source>
        <translation>Copy minified JSON</translation>
    </message>
    <message>
        <location filename="../qml/JsonToolbar.qml" line="254"/>
        <source>复制转义后的 JSON</source>
        <translation>Copy escaped JSON</translation>
    </message>
</context>
<context>
    <name>JsonTreePane</name>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="84"/>
        <source>复制值（格式化的 JSON）</source>
        <translation>Copy value (formatted JSON)</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="84"/>
        <source>复制值</source>
        <translation>Copy value</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="86"/>
        <source>复制键名</source>
        <translation>Copy name</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="87"/>
        <source>复制路径</source>
        <translation>Copy path</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="89"/>
        <source>复制压缩的 JSON</source>
        <translation>Copy minified JSON</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="90"/>
        <source>展开所有子项</source>
        <translation>Expand all children</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="91"/>
        <source>折叠所有子项</source>
        <translation>Collapse all children</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="116"/>
        <source>JSON 树</source>
        <translation>JSON tree</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="192"/>
        <source>粘贴 JSON 就能看</source>
        <translation>Paste JSON to view it</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="202"/>
        <source>日志里夹着的、转义过的、一行一段的 JSON 都能认出来。
也可以把 .json 文件拖到窗口里。</source>
        <translation>JSON embedded in logs, escaped JSON, and one JSON per line are all recognized.
You can also drag a .json file into the window.</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="212"/>
        <source>粘贴 (Ctrl+V)</source>
        <translation>Paste (Ctrl+V)</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="232"/>
        <source>不是有效的 JSON</source>
        <translation>Not valid JSON</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="242"/>
        <source>第 %1 行第 %2 列：%3</source>
        <translation>Line %1, column %2: %3</translation>
    </message>
    <message>
        <location filename="../qml/JsonTreePane.qml" line="253"/>
        <source>在原文中定位</source>
        <translation>Show in text</translation>
    </message>
</context>
<context>
    <name>JsonWindow</name>
    <message>
        <location filename="../qml/JsonWindow.qml" line="29"/>
        <location filename="../qml/JsonWindow.qml" line="29"/>
        <source>JSON 工具</source>
        <translation>JSON tool</translation>
    </message>
    <message>
        <location filename="../qml/JsonWindow.qml" line="34"/>
        <source>已复制格式化的 JSON</source>
        <translation>Copied formatted JSON</translation>
    </message>
    <message>
        <location filename="../qml/JsonWindow.qml" line="35"/>
        <source>已复制压缩的 JSON</source>
        <translation>Copied minified JSON</translation>
    </message>
    <message>
        <location filename="../qml/JsonWindow.qml" line="36"/>
        <source>已复制转义后的 JSON</source>
        <translation>Copied escaped JSON</translation>
    </message>
    <message>
        <location filename="../qml/JsonWindow.qml" line="37"/>
        <source>已复制键名</source>
        <translation>Copied name</translation>
    </message>
    <message>
        <location filename="../qml/JsonWindow.qml" line="38"/>
        <source>已复制路径</source>
        <translation>Copied path</translation>
    </message>
    <message>
        <location filename="../qml/JsonWindow.qml" line="39"/>
        <source>已复制值</source>
        <translation>Copied value</translation>
    </message>
    <message>
        <location filename="../qml/JsonWindow.qml" line="185"/>
        <source>Ctrl+V 粘贴 · Ctrl+F 搜索 · ← → 折叠和展开 · Ctrl+C 复制值 · Ctrl+Shift+C 复制路径</source>
        <translation>Ctrl+V paste · Ctrl+F search · ← → collapse and expand · Ctrl+C copy value · Ctrl+Shift+C copy path</translation>
    </message>
    <message>
        <location filename="../qml/JsonWindow.qml" line="187"/>
        <source>第 %1 行第 %2 列：%3</source>
        <translation>Line %1, column %2: %3</translation>
    </message>
</context>
<context>
    <name>Launcher</name>
    <message>
        <location filename="../Launcher.cpp" line="21"/>
        <source>输入的内容</source>
        <translation>Input</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="22"/>
        <source>剪贴板</source>
        <translation>Clipboard</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="23"/>
        <source>粘贴的内容</source>
        <translation>Pasted text</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="24"/>
        <source>文件</source>
        <translation>File</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="59"/>
        <source>打开 JSON 工具</source>
        <translation>Open JSON tool</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="60"/>
        <source>剪贴板里没有文字，打开后粘贴或输入 JSON</source>
        <translation>The clipboard has no text; open the tool, then paste or type JSON</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="60"/>
        <source>在 json 后面粘贴 JSON，或者先复制再输入 json</source>
        <translation>Paste JSON after json, or copy it first and then type json</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="68"/>
        <source>%1里不是 JSON</source>
        <translation>%1 contains no JSON</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="68"/>
        <source>Enter 打开 JSON 工具，粘贴或输入 JSON</source>
        <translation>Press Enter to open the JSON tool, then paste or type JSON</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="72"/>
        <source>JSON 有错误：%1</source>
        <translation>JSON error: %1</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="73"/>
        <source>%1 · 第 %2 行第 %3 列 · Enter 打开窗口查看和修改</source>
        <translation>%1 · Line %2, column %3 · Press Enter to view and edit in the window</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="81"/>
        <source>查看 JSON</source>
        <translation>View JSON</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="94"/>
        <source>Tab 缩进</source>
        <translation>Tab indent</translation>
    </message>
    <message numerus="yes">
        <location filename="../Launcher.cpp" line="94"/>
        <source>%n 个空格缩进</source>
        <translation>
            <numerusform>%n-space indent</numerusform>
            <numerusform>%n-space indent</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="95"/>
        <source>复制格式化后的 JSON</source>
        <translation>Copy formatted JSON</translation>
    </message>
    <message numerus="yes">
        <location filename="../Launcher.cpp" line="95"/>
        <source>%n 行</source>
        <translation>
            <numerusform>%n line</numerusform>
            <numerusform>%n lines</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="96"/>
        <source>复制压缩后的 JSON</source>
        <translation>Copy minified JSON</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="97"/>
        <source>复制转义后的 JSON</source>
        <translation>Copy escaped JSON</translation>
    </message>
    <message>
        <location filename="../Launcher.cpp" line="97"/>
        <source>放进字符串里用：%1</source>
        <translation>For use inside a string: %1</translation>
    </message>
</context>
<context>
    <name>ws::devtools::JsonTool</name>
    <message>
        <location filename="../JsonTool.cpp" line="92"/>
        <source>只能打开本机的文件</source>
        <translation>Only local files can be opened</translation>
    </message>
</context>
</TS>
