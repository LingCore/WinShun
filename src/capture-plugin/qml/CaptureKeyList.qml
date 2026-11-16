import QtQuick
import WinShun

// Every key of a capture and of the editing window, as [keys, what they do]
// flattened: the toolbar's 全部按键 list (AnnotateToolbar) and the settings'
// 按键 tab (CaptureOptions, GitHub #342) read the same rows. The texts keep
// the toolbar's translation context, where they were first.
QtObject {
    id: list

    property bool editor: false // the editing window's keys, else the capture's
    // The keys the user set (#187), shown with the rest when given; the
    // settings list them as boxes of their own and leave this unset.
    property AnnotationModel annot: null

    readonly property var commonRows: [
        editor ? qsTranslate("AnnotateToolbar", "R E A L P M T N B 或 1–9") : qsTranslate("AnnotateToolbar", "1 E 3 L P M T N B 或 1–9"),
        qsTranslate("AnnotateToolbar", "选工具：矩形、椭圆、箭头、直线、画笔、荧光笔、文字、序号、打码"),
        qsTranslate("AnnotateToolbar", "工具键再按一次"), qsTranslate("AnnotateToolbar", "关掉工具"),
        "Ctrl+Z", qsTranslate("AnnotateToolbar", "撤销"),
        "Ctrl+Y、Ctrl+Shift+Z", qsTranslate("AnnotateToolbar", "重做"),
        "Delete、Backspace", qsTranslate("AnnotateToolbar", "删除选中的对象"),
        "[  ]", qsTranslate("AnnotateToolbar", "粗细、字号、格子小一档 / 大一档"),
        "Shift+B", qsTranslate("AnnotateToolbar", "智能打码：自动找出手机号、证件号、卡号、邮箱、密钥，一次全部打码"),
        "Ctrl+[  Ctrl+]", qsTranslate("AnnotateToolbar", "下移一层 / 上移一层，加 Shift 到底 / 到顶"),
        qsTranslate("AnnotateToolbar", "画的时候按住 Shift"), qsTranslate("AnnotateToolbar", "正方形、正圆；线吸附 15°；画笔、荧光笔画直线"),
        qsTranslate("AnnotateToolbar", "双击"), qsTranslate("AnnotateToolbar", "文字：改文字；序号：改号，序号旁的字：改字"),
        "Ctrl+Enter、Esc", qsTranslate("AnnotateToolbar", "写完文字（Enter 换行；序号旁的字 Enter 就写完）"),
        "Ctrl+C、Enter", qsTranslate("AnnotateToolbar", "复制")
    ].concat(annot && annot.saveKeyText.length > 0
                 ? [annot.saveKeyText, annot.saveDirect ? qsTranslate("AnnotateToolbar", "保存到设置的保存位置") : qsTranslate("AnnotateToolbar", "另存为：选择保存位置")] : [],
             annot && annot.pinKeyText.length > 0 ? [annot.pinKeyText, qsTranslate("AnnotateToolbar", "贴图")] : [])
    readonly property var overlayRows: [
        "WASD", qsTranslate("AnnotateToolbar", "微调鼠标光标，每次 1 个物理像素；按住左键拖选区、拖手柄或画标注时也有效"),
        "Shift+WASD", qsTranslate("AnnotateToolbar", "微调鼠标光标，每次 10 个物理像素"),
        qsTranslate("AnnotateToolbar", "滚轮"), qsTr("框选前扩大 / 缩小框选范围；有工具或选中对象时改粗细"),
        qsTranslate("AnnotateToolbar", "方向键"), qsTranslate("AnnotateToolbar", "挪动选中的对象；否则移动选区 1 个物理像素，Shift 调整选区大小；未框选时微调光标"),
        "F", qsTr("框选前按预设尺寸或比例框选；框选后输入尺寸、锁定比例、选预设"),
        "H", qsTranslate("AnnotateToolbar", "隐藏 / 显示工具条"),
        "?、F1", qsTranslate("AnnotateToolbar", "显示 / 隐藏屏幕角落的快捷键提示（只管这次截图）"),
        "C、Shift", qsTranslate("AnnotateToolbar", "复制颜色并结束截图；单按 Shift 换颜色格式（HEX、RGB、HSL）"),
        "Shift+C", qsTranslate("AnnotateToolbar", "识别选区里的文字并复制"),
        "R / Shift+R", qsTranslate("AnnotateToolbar", "恢复最近选区：向前 / 向后循环，最多 10 个；使用当前画面"),
        ",  .", qsTranslate("AnnotateToolbar", "翻看之前截图时的画面（整屏、选区和标注），. 往回翻"),
        "Q", qsTranslate("AnnotateToolbar", "识别选区里的二维码"),
        "Ctrl+Shift+D", qsTranslate("AnnotateToolbar", "延时截图：关掉这次，倒计时后再截"),
        "Ctrl+L", qsTranslate("AnnotateToolbar", "长截图：选区里的内容一边滚动一边截"),
        "Ctrl+R", qsTranslate("AnnotateToolbar", "区域录屏：把选区里的操作录成 MP4 或 GIF"),
        qsTranslate("AnnotateToolbar", "Esc、右键"), qsTranslate("AnnotateToolbar", "退一步：写完文字、放弃标注、关闭截图")
    ]
    readonly property var editorRows: [
        qsTranslate("AnnotateToolbar", "方向键"), qsTranslate("AnnotateToolbar", "挪动选中的对象，按住 Shift 一次 10 像素；没有选中时滚动图片"),
        qsTranslate("AnnotateToolbar", "Shift+点击、拖空白处"), qsTranslate("AnnotateToolbar", "多选；Ctrl+A 全选，Ctrl+D 复制一份"),
        "I", qsTranslate("AnnotateToolbar", "吸管：点图上的一点，取它的颜色"),
        qsTranslate("AnnotateToolbar", "拖图片的边"), qsTranslate("AnnotateToolbar", "裁剪（没有工具时；有工具时从图外拖）"),
        qsTranslate("AnnotateToolbar", "滚轮"), qsTranslate("AnnotateToolbar", "上下滚动，Shift 左右；Alt+滚轮改粗细"),
        qsTranslate("AnnotateToolbar", "Ctrl+滚轮、Ctrl+加号 / 减号"), qsTranslate("AnnotateToolbar", "缩放"),
        "Z", qsTranslate("AnnotateToolbar", "适合窗口 / 100%"),
        qsTranslate("AnnotateToolbar", "空格 + 拖动，或中键拖动"), qsTranslate("AnnotateToolbar", "平移"),
        qsTranslate("AnnotateToolbar", "Esc"), qsTranslate("AnnotateToolbar", "写完文字；有改动时放弃全部（Ctrl+Z 恢复）；没有改动时关闭")
    ]
    readonly property var rows: commonRows.concat(editor ? editorRows : overlayRows)
}
