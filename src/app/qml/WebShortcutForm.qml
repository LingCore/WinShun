pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Adding or changing a web shortcut in the settings (网页搜索): its keyword,
// its name, the address it searches with %s where the words go, and the page
// the keyword alone opens, where its search suggestions come from, and a
// picture of the user's for its icon (a file, or SVG code pasted).
// SettingsEditor checks and saves it, or says what is wrong. Enter saves,
// Esc puts it away. The overlay keeps it in view; only the fields scroll,
// leaving validation and actions visible on short screens.
FocusScope {
    id: form

    required property SettingsEditor editor
    property int index: -1 // the one being changed; -1: a new one
    property var shortcut: ({ keyword: "", name: "", url: "", home: "", icon: "", homeUrl: "", suggest: "" })
    property string error
    property string icon: form.shortcut.icon || "" // a picture chosen (SettingsEditor::pickWebIcon); empty: the site's
    property string fallbackIcon: form.shortcut.icon || ""
    property string appliedSvg
    property string svgError
    property bool svgTouched: false
    readonly property bool compact: height < 520
    // 84, or the widest label if a language needs more ("Search address").
    readonly property int labelWidth: Math.ceil(Math.max(84, keywordMetrics.width, urlMetrics.width,
                                                         homeMetrics.width, suggestMetrics.width, iconMetrics.width))
    readonly property int inset: form.compact ? 12 : 16
    readonly property int fieldGap: form.compact ? 6 : 8
    readonly property int sectionGap: form.compact ? 10 : 12
    // The suggestion address filled in for the search address typed (a new
    // one only, SettingsEditor::knownSuggestUrl): replaced as the address
    // changes, unless its user wrote another.
    property string offeredSuggest

    TextMetrics { id: keywordMetrics; text: qsTr("关键词"); font.pixelSize: Theme.fontCaption }
    TextMetrics { id: urlMetrics; text: qsTr("搜索网址"); font.pixelSize: Theme.fontCaption }
    TextMetrics { id: homeMetrics; text: qsTr("主页"); font.pixelSize: Theme.fontCaption }
    TextMetrics { id: suggestMetrics; text: qsTr("搜索建议"); font.pixelSize: Theme.fontCaption }
    TextMetrics { id: iconMetrics; text: qsTr("图标"); font.pixelSize: Theme.fontCaption }

    signal finished()

    property bool closing: false
    property Item lastFocus: null

    function close() {
        closing = true
        finished()
    }

    function save() {
        if (!updateSvgIcon()) {
            svgInput.takeFocus()
            return
        }
        form.error = form.editor.saveWebShortcut(form.index, keywordField.text, nameField.text, urlField.text,
                                                 homeField.text, form.icon, suggestField.text)
        if (form.error.length === 0)
            form.close()
    }

    function takeIcon(picked) {
        if (picked.error) {
            form.error = picked.error
        } else if (picked.icon) {
            // A .svg picked shows its code, to be changed like code pasted.
            const svg = (picked.svg || "").trim()
            form.appliedSvg = svg // first: the text set below is then no change to draw
            svgInput.text = svg
            svgRefresh.stop()
            form.svgTouched = true
            form.svgError = ""
            form.fallbackIcon = svg ? "" : picked.icon
            form.icon = picked.icon
            form.error = ""
        }
    }

    // The SVG code the saved icon was drawn from, shown again. The picture
    // is that code, so clearing it goes back to the site's icon.
    function restoreSvg() {
        const svg = form.icon ? form.editor.webIconSvg(form.icon).trim() : ""
        if (svg.length === 0)
            return
        form.appliedSvg = svg
        svgInput.text = svg
        svgRefresh.stop()
        form.fallbackIcon = ""
    }

    function updateSvgIcon() {
        svgRefresh.stop()
        const text = svgInput.text.trim()
        if (text.length === 0) {
            form.icon = form.fallbackIcon
            form.appliedSvg = ""
            form.svgError = ""
            return true
        }
        if (text === form.appliedSvg)
            return true
        const picked = form.editor.webIconFromSvg(text)
        form.svgError = picked.error || ""
        form.appliedSvg = form.svgError ? "" : text
        form.icon = picked.icon || form.fallbackIcon
        return form.svgError.length === 0
    }

    function fillSvgFromClipboard() {
        if (form.index < 0 && !form.svgTouched && svgInput.text.length === 0)
            svgInput.text = form.editor.webIconClipboardText()
    }

    function inside(item, ancestor) {
        for (let current = item; current; current = current.parent) {
            if (current === ancestor)
                return true
        }
        return false
    }

    // TextInput handles Tab before ancestor Keys handlers see it. Wrap
    // focus when Qt reaches the surrounding page, in either direction.
    function keepFocusInside() {
        const current = form.Window.window?.activeFocusItem
        if (!current)
            return
        if (inside(current, form)) {
            lastFocus = current
            revealFocus()
        } else if (inside(lastFocus, keywordField)) {
            saveButton.forceActiveFocus(Qt.BacktabFocusReason)
        } else {
            keywordField.takeFocus()
        }
    }

    function revealFocus() {
        const current = form.Window.window.activeFocusItem
        if (!inside(current, fields))
            return
        const top = current.mapToItem(fields, 0, 0).y
        const bottom = top + current.height
        const maximum = Math.max(0, viewport.contentHeight - viewport.height)
        if (top < viewport.contentY + 8)
            viewport.contentY = Math.max(0, top - 8)
        else if (bottom > viewport.contentY + viewport.height - 8)
            viewport.contentY = Math.min(maximum, bottom + 8 - viewport.height)
    }

    focus: true
    Connections {
        target: form.Window.window
        enabled: !form.closing && form.visible
        function onActiveFocusItemChanged() { form.keepFocusInside() }
    }
    Keys.onEscapePressed: form.close()
    Component.onCompleted: {
        form.restoreSvg()
        form.fillSvgFromClipboard()
        const first = form.index < 0 ? keywordField : urlField
        Qt.callLater(() => first.takeFocus())
    }

    Timer {
        id: svgRefresh
        interval: 300
        onTriggered: form.updateSvgIcon()
    }

    // Inline labels leave room for every field, even in a short window.
    component Field: Column {
        id: field

        property string label
        property string hint
        property real labelWidth: form.labelWidth
        property alias text: box.text
        property alias placeholder: box.placeholder

        signal accepted()

        function takeFocus() { box.takeFocus() }

        spacing: 4

        Row {
            width: field.width
            spacing: 8

            Text {
                id: fieldLabel
                width: field.labelWidth
                height: box.height
                verticalAlignment: Text.AlignVCenter
                text: field.label
                color: Theme.text
                font.pixelSize: Theme.fontCaption
                HoverHandler { id: labelHover }
            }
            InputBox {
                id: box
                width: field.width - field.labelWidth - parent.spacing
                onAccepted: field.accepted()
                onTextChanged: form.error = "" // an old complaint is stale once the field changes
            }
        }
        Text {
            visible: field.hint.length > 0 && !form.compact
            x: field.labelWidth + 8
            width: field.width - x
            text: field.hint
            color: Theme.subtext
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
            lineHeight: 1.15
        }
        HoverTip {
            target: form.compact && labelHover.hovered && field.hint.length > 0 ? fieldLabel : null
            text: field.hint
        }
    }

    Rectangle { // modal backdrop: page clicks and wheel events stay here
        anchors.fill: parent
        color: Theme.tok("scrim", Qt.rgba(0, 0, 0, Theme.dark ? 0.5 : 0.3))
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Item {
        id: card
        objectName: "webFormCard"
        anchors.centerIn: parent
        width: Math.min(680, parent.width - 32)
        height: Math.min(parent.height - 24,
                         heading.height + fields.implicitHeight + footer.height + 2 * (form.inset + form.sectionGap))

        Hook { // the style's floating box (Theme.menuFrame)
            anchors.fill: parent
            part: Theme.menuFrame
            dpr: Screen.devicePixelRatio
        }

        Text {
            id: heading
            x: 20
            y: form.inset
            width: parent.width - 40
            text: form.index < 0 ? qsTr("添加网页搜索") : qsTr("编辑网页搜索")
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Theme.fontStrong
        }

        Flickable {
            id: viewport
            objectName: "webFormViewport"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: heading.bottom
            anchors.bottom: footer.top
            anchors.margins: form.sectionGap
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            contentWidth: width
            contentHeight: fields.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            onHeightChanged: Qt.callLater(form.revealFocus)

            Column {
                id: fields
                width: viewport.width - 8
                spacing: form.fieldGap

                Row {
                    width: parent.width
                    spacing: 12

                    // Its box starts where the boxes below do; the name's is narrower for it.
                    Field {
                        id: keywordField
                        objectName: "keywordField"
                        width: form.labelWidth + 8 + 124
                        label: qsTr("关键词")
                        placeholder: qsTr("例如 gh")
                        text: form.shortcut.keyword
                        onAccepted: form.save()
                    }
                    Field {
                        id: nameField
                        objectName: "nameField"
                        width: parent.width - keywordField.width - parent.spacing
                        labelWidth: 42
                        label: qsTr("名称")
                        placeholder: qsTr("例如 GitHub；留空用网站域名")
                        text: form.shortcut.name
                        onAccepted: form.save()
                    }
                }

                Field {
                    id: urlField
                    objectName: "urlField"
                    width: parent.width
                    label: qsTr("搜索网址")
                    placeholder: "https://github.com/search?q=%s"
                    hint: qsTr("搜索词用 %s 代替，可直接粘贴搜索结果页的网址")
                    text: form.shortcut.url
                    onAccepted: form.save()
                    onTextChanged: {
                        if (form.index >= 0 || !suggestField)
                            return
                        const known = form.editor.knownSuggestUrl(text)
                        if (suggestField.text.length === 0 || suggestField.text === form.offeredSuggest) {
                            suggestField.text = known
                            form.offeredSuggest = known
                        }
                    }
                }

                Field {
                    id: homeField
                    objectName: "homeField"
                    width: parent.width
                    label: qsTr("主页")
                    placeholder: qsTr("可不填，默认打开搜索网站的首页")
                    text: form.shortcut.home
                    onAccepted: form.save()
                }

                Field {
                    id: suggestField
                    objectName: "suggestField"
                    width: parent.width
                    label: qsTr("搜索建议")
                    placeholder: qsTr("建议接口网址（可不填）")
                    hint: qsTr("搜索词同样用 %s 代替，支持 OpenSearch、百度 JSONP 和 JSON 列表")
                    text: form.shortcut.suggest || ""
                    onAccepted: form.save()
                }

                Row {
                    id: iconControls
                    width: parent.width
                    spacing: 8

                    Text {
                        width: form.labelWidth
                        height: svgInput.height
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("图标")
                        color: Theme.text
                        font.pixelSize: Theme.fontCaption
                    }
                    Item { // as its rows will show it
                        id: iconPreview
                        anchors.verticalCenter: parent.verticalCenter
                        width: 24
                        height: 24

                        Image {
                            readonly property real dpr: Screen.devicePixelRatio
                            readonly property int pixels: Math.round(iconPreview.width * dpr)

                            // Exactly its device pixels, unsmoothed (as in ResultRow).
                            x: 1 / 64
                            y: 1 / 64
                            width: pixels / dpr
                            height: pixels / dpr
                            sourceSize.width: pixels / dpr
                            sourceSize.height: pixels / dpr
                            smooth: false
                            asynchronous: true
                            // A new one has no site yet: the browser's icon ("https:" has no host).
                            source: form.editor.siteIconsArrived >= 0
                                    ? form.editor.webIconUrl(form.icon.length > 0 ? "custom:" + form.icon
                                                                                 : (form.shortcut.homeUrl || "https:"),
                                                             Theme.dark)
                                    : ""
                        }
                    }
                    FlatButton {
                        id: pickIconButton
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("选择图片")
                        onClicked: form.takeIcon(form.editor.pickWebIcon())
                        HoverHandler { id: pickIconHover }
                        HoverTip {
                            target: pickIconHover.hovered ? pickIconButton : null
                            text: qsTr("支持 PNG、ICO、SVG；留空使用网站图标")
                        }
                    }
                    FlatButton {
                        id: siteIconButton
                        objectName: "webFormSiteIcon"
                        visible: form.icon.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        flat: true
                        text: qsTr("用网站的图标")
                        onClicked: {
                            form.fallbackIcon = ""
                            form.icon = ""
                            form.svgTouched = true
                            svgInput.text = ""
                            form.updateSvgIcon()
                        }
                    }
                    // One line, as high as the buttons; the code pasted, or the
                    // saved icon's, is all there to be changed.
                    InputBox {
                        id: svgInput
                        objectName: "webFormSvgInput"
                        width: iconControls.width - form.labelWidth - iconPreview.width - pickIconButton.width
                               - (siteIconButton.visible ? siteIconButton.width : 0)
                               - iconControls.spacing * (siteIconButton.visible ? 4 : 3)
                        maximumLength: 1 << 20 // never cut: an SVG too large is refused with a reason
                        placeholder: qsTr("粘贴 SVG 代码（可不填）")
                        error: form.svgError.length > 0 // the style's error mark (Theme.inputField)
                        onAccepted: form.save()
                        onInputFocusedChanged: if (inputFocused) form.fillSvgFromClipboard()
                        onTextChanged: {
                            if (text.length > 0)
                                form.svgTouched = true
                            form.svgError = ""
                            svgRefresh.restart()
                        }

                        HoverHandler { id: svgHover }
                        HoverTip {
                            target: svgHover.hovered ? svgInput : null
                            text: qsTr("添加时自动填入剪贴板中的 SVG，可直接粘贴或编辑")
                        }
                    }
                }
            }
        }

        ListScrollBar {
            flickable: viewport
            anchors.top: viewport.top
            anchors.bottom: viewport.bottom
            anchors.right: parent.right
            anchors.rightMargin: 8
        }

        Column {
            id: footer
            x: 20
            width: parent.width - 40
            anchors.bottom: parent.bottom
            anchors.bottomMargin: form.inset
            spacing: 6

            Text {
                objectName: "webFormError"
                visible: form.svgError.length > 0 || form.error.length > 0
                width: parent.width
                text: form.svgError || form.error
                textFormat: Text.PlainText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Row {
                anchors.right: parent.right
                spacing: 8

                FlatButton {
                    objectName: "webFormCancel"
                    text: qsTr("取消")
                    onClicked: form.close()
                }
                FlatButton {
                    id: saveButton
                    objectName: "webFormSave"
                    highlighted: true
                    text: form.index < 0 ? qsTr("添加") : qsTr("保存")
                    onClicked: form.save()
                }
            }
        }
    }
}
