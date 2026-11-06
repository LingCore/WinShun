pragma Singleton
import QtQuick

QtObject {
    // Normalize aliases before the existing list/grid navigation handles
    // selection, scrolling and boundaries. IME and AltGr keep their keys.
    function navigationKey(key: int, modifiers: int, composing: bool): int {
        if (composing || (modifiers & ~Qt.KeypadModifier) !== Qt.ControlModifier)
            return key
        switch (key) {
        case Qt.Key_J:
        case Qt.Key_N: return Qt.Key_Down
        case Qt.Key_K:
        case Qt.Key_P: return Qt.Key_Up
        case Qt.Key_U: return Qt.Key_PageUp
        case Qt.Key_D: return Qt.Key_PageDown
        default: return key
        }
    }

    // A file's kind for a style's own icons (Theme.typeIcon): doc xls ppt pdf
    // image md zip code audio video ebook text other; "" for one that keeps
    // the system's icon (a program, a link: their own pictures).
    readonly property var kinds: ({
        doc: "doc", docx: "doc", docm: "doc", dot: "doc", dotx: "doc", rtf: "doc", odt: "doc", wps: "doc", wpd: "doc", pages: "doc",
        xls: "xls", xlsx: "xls", xlsm: "xls", xlsb: "xls", csv: "xls", ods: "xls", et: "xls", numbers: "xls",
        ppt: "ppt", pptx: "ppt", pptm: "ppt", pps: "ppt", ppsx: "ppt", odp: "ppt", dps: "ppt", key: "ppt",
        pdf: "pdf",
        jpg: "image", jpeg: "image", jfif: "image", png: "image", gif: "image", bmp: "image", webp: "image", heic: "image",
        heif: "image", avif: "image", tif: "image", tiff: "image", ico: "image", svg: "image", psd: "image", raw: "image",
        jxr: "image", dng: "image",
        md: "md", markdown: "md",
        zip: "zip", rar: "zip", "7z": "zip", tar: "zip", gz: "zip", tgz: "zip", bz2: "zip", xz: "zip", zst: "zip",
        iso: "zip", cab: "zip",
        c: "code", h: "code", cpp: "code", hpp: "code", cc: "code", cxx: "code", cs: "code", java: "code", kt: "code",
        py: "code", js: "code", mjs: "code", ts: "code", jsx: "code", tsx: "code", go: "code", rs: "code", swift: "code",
        rb: "code", php: "code", lua: "code", html: "code", htm: "code", css: "code", scss: "code", json: "code",
        xml: "code", yaml: "code", yml: "code", toml: "code", qml: "code", sh: "code", sql: "code",
        mp3: "audio", wav: "audio", flac: "audio", aac: "audio", m4a: "audio", ogg: "audio", opus: "audio", wma: "audio",
        ape: "audio", aiff: "audio", mid: "audio", midi: "audio", amr: "audio",
        mp4: "video", mkv: "video", avi: "video", mov: "video", wmv: "video", flv: "video", webm: "video", m4v: "video",
        mpg: "video", mpeg: "video", m2ts: "video", "3gp": "video", rm: "video", rmvb: "video", vob: "video",
        epub: "ebook", mobi: "ebook", azw: "ebook", azw3: "ebook", fb2: "ebook", djvu: "ebook", chm: "ebook",
        txt: "text", log: "text", ini: "text", cfg: "text", conf: "text",
        exe: "", msi: "", msix: "", appx: "", lnk: "", url: "", bat: "", cmd: "", com: "", scr: "", ps1: "", vbs: ""
    })
    function fileKind(path: string): string {
        const name = path.substring(Math.max(path.lastIndexOf("\\"), path.lastIndexOf("/")) + 1)
        const dot = name.lastIndexOf(".")
        if (dot <= 0)
            return "other"
        const extension = name.substring(dot + 1).toLowerCase()
        return Object.prototype.hasOwnProperty.call(kinds, extension) ? kinds[extension] : "other"
    }
}
