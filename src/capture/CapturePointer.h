#pragma once

#include <QCursor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace ws::capture {

// The picture's input area inherits this cursor. The cross and the I-beam
// are alpha pictures of our own instead of Windows' theme-dependent,
// potentially inverted IDC_CROSS and IDC_IBEAM; they grow with Windows'
// pointer size as those do. The other shapes stay Windows' own.
class CapturePointer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int shape READ shape WRITE setShape NOTIFY shapeChanged FINAL)
    Q_PROPERTY(qreal dpr READ dpr WRITE setDpr NOTIFY dprChanged FINAL)
    // Windows' pointer size (Settings › Accessibility › Mouse pointer and
    // touch; [HKCU\Control Panel\Cursors] CursorBaseSize), 32 unless made
    // larger. Read when made; set only by the tests.
    Q_PROPERTY(int baseSize READ baseSize WRITE setBaseSize NOTIFY baseSizeChanged FINAL)

public:
    explicit CapturePointer(QQuickItem* parent = nullptr);
    int shape() const { return m_shape; }
    void setShape(int shape);
    qreal dpr() const { return m_dpr; }
    void setDpr(qreal dpr);
    int baseSize() const { return m_baseSize; }
    void setBaseSize(int size);

signals:
    void shapeChanged();
    void dprChanged();
    void baseSizeChanged();

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
};

} // namespace ws::capture
