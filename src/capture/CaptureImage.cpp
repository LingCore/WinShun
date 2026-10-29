#include "CaptureImage.h"

#include <QQuickWindow>
#include <QPainter>
#include <QSGImageNode>
#include <QSGTexture>

namespace ws::capture {

CaptureImage::CaptureImage(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void CaptureImage::setImage(const QImage& image)
{
    if (image.isNull() && m_image.isNull())
        return;
    m_image = image;
    m_dirty = true;
    update();
    emit imageChanged();
}

QSGNode* CaptureImage::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    auto* node = static_cast<QSGImageNode*>(old);
    // A new picture gets a new node: the old one goes with its texture
    // (setOwnsTexture), nothing is left to setTexture's handling of the old.
    if (m_dirty || m_image.isNull() || width() <= 0 || height() <= 0) {
        delete node;
        node = nullptr;
    }
    m_dirty = false;
    if (m_image.isNull() || width() <= 0 || height() <= 0)
        return nullptr;
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Nearest);
        node->setMipmapFiltering(QSGTexture::None);
        node->setTexture(window()->createTextureFromImage(m_image));
    }
    node->setRect(boundingRect());
    node->setSourceRect(QRectF(QPointF(0, 0), m_image.size()));
    return node;
}

CaptureLoupe::CaptureLoupe(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setOpaquePainting(true);
    setFillColor(Qt::black);
    setSmooth(false);
}

void CaptureLoupe::setImage(const QImage& image)
{
    if (m_image.cacheKey() == image.cacheKey())
        return;
    m_image = image;
    update();
    emit imageChanged();
}

void CaptureLoupe::paint(QPainter* painter)
{
    if (!m_image.isNull()) {
        painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter->drawImage(boundingRect(), m_image);
    }
}

} // namespace ws::capture
