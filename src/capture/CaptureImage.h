#pragma once

#include <QImage>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace ws::capture {

// A frozen picture drawn unsmoothed over the item's whole size (1:1, sized
// by CaptureScreen's scale). The live magnifier uses CaptureLoupe below.
// Not Image with a provider: the
// texture here exists exactly as long as the picture is set. Setting a null
// picture removes it with the next frame, which is how the capture gives
// the memory back (Capture::finish renders that frame cloaked).
class CaptureImage : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged FINAL)

public:
    explicit CaptureImage(QQuickItem* parent = nullptr);

    QImage image() const { return m_image; }
    void setImage(const QImage& image);

signals:
    void imageChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;

private:
    QImage m_image;
    bool m_dirty = false;
};

// A small image that changes with every pointer move. PaintedItem keeps its
// backing texture and updates it; CaptureImage suits the large frozen frame.
class CaptureLoupe : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged FINAL)

public:
    explicit CaptureLoupe(QQuickItem* parent = nullptr);
    QImage image() const { return m_image; }
    void setImage(const QImage& image);
    void paint(QPainter* painter) override;

signals:
    void imageChanged();

private:
    QImage m_image;
};

} // namespace ws::capture
