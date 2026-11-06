import QtQuick

// A file's thumbnail in its row's icon slot (ResultRow, DialogBarRow): a
// picture, a video, a PDF as Explorer shows it (ThumbnailProvider), all of
// it, as large as the slot holds. Put over the icon, which stays until this
// has loaded (`ready`), so the row shows something at once and never jumps:
// a delegate reused for another file shows its icon again until its picture
// is there. A file without one stays Ready with no size: never shown.
Image {
    id: thumbnail

    // The window's, passed by a host that reads it once for all its rows (GitHub #111).
    property real dpr: Screen.devicePixelRatio
    readonly property bool ready: status === Image.Ready && implicitWidth > 0

    visible: ready
    // Made once at the slot's size in device pixels (sourceSize: an image
    // provider, so logical pixels, both set: the aspect kept), then drawn
    // pixel for pixel (implicit size: its pixels over the ratio), unsmoothed,
    // centred on whole device pixels and nudged off the half pixel, as the
    // icon under it is.
    x: Math.round((parent.width - width) / 2 * dpr) / dpr + 1 / 64
    y: Math.round((parent.height - height) / 2 * dpr) / dpr + 1 / 64
    width: implicitWidth
    height: implicitHeight
    sourceSize.width: parent.width
    sourceSize.height: parent.height
    asynchronous: true
    smooth: false
}
