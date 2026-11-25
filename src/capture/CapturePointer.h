#pragma once

#include <QColor>
#include <QCursor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace ws::capture {

// A round tip as the pointer: a disc of pixels across (screen pixels) in
// color, what a click with the pen leaves, ringed white and dark so it
// shows on any picture. A small one gets four ticks around it, to be found
// by. The hotspot is its middle. aseSize: Windows' pointer size, which
// the ring and the ticks grow with (the disc is the tip's size, always).
QCursor brushCursor(QColor color, qreal pixels, qreal dpr, int baseSize = 32);

// The picture's input area inherits this cursor. The cross and the I-beam
// are alpha pictures of our own instead of Windows' theme-dependent,
// potentially inverted IDC_CROSS and IDC_IBEAM; they grow with Windows'
// pointer size as those do. The other shapes stay Windows' own.
//
// With a brush set, the cross is the brush's tip instead (GitHub #461): the
// pen, the marker and the brush redaction show how wide they draw, and in
// what colour, before the press.
class CapturePointer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int shape READ shape WRITE setShape NOTIFY shapeChanged FINAL)
    Q_PROPERTY(qreal dpr READ dpr WRITE setDpr NOTIFY dprChanged FINAL)
    // Windows' pointer size (Settings › Accessibility › Mouse pointer and
    // touch; [HKCU\Control Panel\Cursors] CursorBaseSize), 32 unless made
    // larger. Read when made; set only by the tests.
    Q_PROPERTY(int baseSize READ baseSize WRITE setBaseSize NOTIFY baseSizeChanged FINAL)
    // The tool's round tip, screen pixels across; 0: none, the cross.
    Q_PROPERTY(qreal brushSize READ brushSize WRITE setBrushSize NOTIFY brushChanged FINAL)
    Q_PROPERTY(QColor brushColor READ brushColor WRITE setBrushColor NOTIFY brushChanged FINAL)

public:
    explicit CapturePointer(QQuickItem* parent = nullptr);
    int shape() const { return m_shape; }
    void setShape(int shape);
    qreal dpr() const { return m_dpr; }
    void setDpr(qreal dpr);
    int baseSize() const { return m_baseSize; }
    void setBaseSize(int size);
    qreal brushSize() const { return m_brushSize; }
    void setBrushSize(qreal pixels);
    QColor brushColor() const { return m_brushColor; }
    void setBrushColor(const QColor& color);

signals:
    void shapeChanged();
    void dprChanged();
    void baseSizeChanged();
    void brushChanged();

private:
    void refreshCursor();
    int m_shape = Qt::CrossCursor;
    qreal m_dpr = 1;
    int m_baseSize = 32;
    // Made for this dpr and base size; made again when either changes.
    qreal m_drawnDpr = 0;
    int m_drawnSize = 0;
    QCursor m_cross;
    QCursor m_beam;
    qreal m_brushSize = 0;
    QColor m_brushColor;
    // The tip as last drawn: made again when it, the dpr or the base size changes.
    struct Tip {
        qreal size = 0;
        QRgb color = 0;
        qreal dpr = 0;
        int baseSize = 0;
        bool operator==(const Tip&) const = default;
    };
    Tip m_drawnTip;
    QCursor m_tip;
};

} // namespace ws::capture
