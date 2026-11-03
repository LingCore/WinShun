#include "AnnotationCanvas.h"

#include "EditView.h"
#include "LiveStroke.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGVertexColorMaterial>
#include <QSGNode>
#include <QSGTexture>

#include <algorithm>
#include <cstring>
#include <numeric>

using namespace Qt::StringLiterals;

namespace ws::capture {

AnnotationCanvas::AnnotationCanvas(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_viewMoving.setSingleShot(true);
    m_viewMoving.setInterval(400);
    connect(&m_viewMoving, &QTimer::timeout, this, [this] {
        if (m_meter.on && m_model)
            reportMeter(m_model->compositor().patchesMade());
    });
}

// The nodes are the scene graph's (it deletes them with their textures);
// m_nodes only points at them.
AnnotationCanvas::~AnnotationCanvas() = default;

void AnnotationCanvas::setModel(AnnotationModel* model)
{
    if (m_model == model)
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    if (model) {
        connect(model, &AnnotationModel::changed, this, &AnnotationCanvas::onChanged);
        // The QPointer is null by then: the next polish lets the tiles go.
        connect(model, &QObject::destroyed, this, &AnnotationCanvas::markAll);
        // A gesture's cost is logged when it ends; the next one starts afresh.
        connect(model, &AnnotationModel::gestureChanged, this, [this] {
            if (m_live)
                polish(); // a stroke let go of: the tiles take it from the GPU
            if (m_meter.on && m_model && !m_model->gestureActive())
                reportMeter(m_model->compositor().patchesMade());
        });
    }
    markAll();
    emit modelChanged();
}

void AnnotationCanvas::setScaleX(qreal scale)
{
    if (scale == m_scaleX)
        return;
    m_scaleX = scale;
    m_rectsStale = true;
    update();
    emit scaleChanged();
}

void AnnotationCanvas::setScaleY(qreal scale)
{
    if (scale == m_scaleY)
        return;
    m_scaleY = scale;
    m_rectsStale = true;
    update();
    emit scaleChanged();
}

void AnnotationCanvas::setSparse(bool sparse)
{
    if (sparse == m_sparse)
        return;
    m_sparse = sparse;
    markAll(); // which tiles have a picture is decided again
    emit sparseChanged();
}

void AnnotationCanvas::setLimited(bool limited)
{
    if (limited == m_limited)
        return;
    m_limited = limited;
    markAll();
    emit limitedChanged();
}

void AnnotationCanvas::setViewport(const QRectF& viewport)
{
    if (viewport == m_viewport)
        return;
    m_viewport = viewport;
    if (m_limited) {
        m_viewMoving.start();
        polish(); // tiles come into view or go out of it
    }
    emit viewportChanged();
}

void AnnotationCanvas::setLinear(bool linear)
{
    if (linear == m_linear)
        return;
    m_linear = linear;
    m_filterStale = true;
    update();
    emit linearChanged();
}

void AnnotationCanvas::setLive(bool live)
{
    if (live == m_live)
        return;
    m_live = live;
    polish();
    emit liveChanged();
}

int AnnotationCanvas::tilesShown() const
{
    return int(std::count_if(m_tiles.cbegin(), m_tiles.cend(), [](const Tile& t) { return t.shown; }));
}

// An empty region is all of it (a new picture, the capture's end).
void AnnotationCanvas::onChanged(const QRectF& dirty)
{
    if (dirty.isEmpty())
        markAll();
    else
        markTiles(dirty.toAlignedRect());
}

void AnnotationCanvas::markAll()
{
    for (Tile& tile : m_tiles)
        tile.dirty = true;
    polish(); // a new size makes new tiles there, all of them dirty
}

void AnnotationCanvas::markTiles(const QRect& area)
{
    const QRect r = area & QRect(QPoint(0, 0), m_size);
    if (r.isEmpty() || m_columns <= 0)
        return;
    for (int row = r.top() / kTile; row <= r.bottom() / kTile; ++row) {
        for (int column = r.left() / kTile; column <= r.right() / kTile; ++column)
            m_tiles[std::size_t(row * m_columns + column)].dirty = true;
    }
    polish();
}

void AnnotationCanvas::dropTiles()
{
    if (m_tiles.empty() && m_size.isEmpty())
        return;
    m_tiles.clear();
    m_tiles.shrink_to_fit();
    m_size = QSize();
    m_columns = 0;
    m_rows = 0;
    m_dropNodes = true;
}

QRect AnnotationCanvas::tileRect(int index) const
{
    const int x = (index % m_columns) * kTile;
    const int y = (index / m_columns) * kTile;
    return QRect(x, y, std::min(kTile, m_size.width() - x), std::min(kTile, m_size.height() - y));
}

QRectF AnnotationCanvas::nodeRect(int index) const
{
    const QRect r = tileRect(index);
    // Each edge from its own image px, so neighbours meet without a seam.
    const qreal x0 = r.left() * m_scaleX;
    const qreal y0 = r.top() * m_scaleY;
    const qreal x1 = (r.left() + r.width()) * m_scaleX;
    const qreal y1 = (r.top() + r.height()) * m_scaleY;
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

// The GUI thread, before the frame is synchronized: the dirty tiles drawn
// by the model's compositor, which is not thread safe and lives here.
void AnnotationCanvas::updatePolish()
{
    AnnotationModel* model = m_model;
    if (!model || !model->active()) {
        if (m_limited && m_mostShown > 0) {
            qInfo().noquote() << u"Annotation canvas: a view of a %1 x %2 picture kept at most %3 tiles (the view and a ring need %4; the picture has %5)"_s
                                     .arg(m_size.width())
                                     .arg(m_size.height())
                                     .arg(m_mostShown)
                                     .arg(m_mostWanted)
                                     .arg(m_tiles.size());
        }
        m_mostShown = m_mostWanted = 0;
        dropTiles();
        update();
        return;
    }
    QElapsedTimer clock;
    clock.start();
    const bool gesture = model->gestureActive() || (m_limited && m_viewMoving.isActive());
    if (gesture && !m_meter.on) {
        m_meter = Meter {};
        m_meter.on = true;
        m_meter.patchesBefore = model->compositor().patchesMade();
    }
    const QSize size = model->base().size();
    if (size != m_size) {
        dropTiles();
        m_size = size;
        m_columns = (size.width() + kTile - 1) / kTile;
        m_rows = (size.height() + kTile - 1) / kTile;
        m_tiles.assign(std::size_t(m_columns) * std::size_t(m_rows), Tile {});
        for (Tile& tile : m_tiles)
            tile.dirty = true;
    }
    if (model->compositor().transparent() != m_transparent) {
        m_transparent = model->compositor().transparent();
        for (Tile& tile : m_tiles)
            tile.dirty = true; // drawn again in the other format, their textures made again
    }
    const annot::Document& doc = model->document();
    // The pen stroke under the mouse: the GPU's while it is drawn (`live`).
    quint32 liveNow = 0;
    const QQuickWindow* win = window();
    const bool software = win && win->rendererInterface()
        && win->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    if (m_live && !software && model->gestureActive() && !model->pathActive() && !doc.items.isEmpty()) {
        const annot::Item& last = doc.items.last();
        if (last.kind == annot::Kind::Pen && last.id + 1 == doc.nextId && !last.points.isEmpty())
            liveNow = last.id;
    }
    const auto itemOf = [&doc](quint32 id) -> const annot::Item* {
        for (const annot::Item& item : doc.items) {
            if (id != 0 && item.id == id)
                return &item;
        }
        return nullptr;
    };
    const auto areaOf = [&doc](const annot::Item& item) {
        return annot::bounds(item, doc.dpr, 1, annot::formatOf(doc, item.series)).toAlignedRect().adjusted(-2, -2, 2, 2);
    };
    if (liveNow != m_liveId) {
        // Let go of: the tiles take it back, a few at each frame (all at
        // once took 40-90 ms on a long stroke), the GPU showing it meanwhile.
        if (m_liveId && !liveNow) {
            if (const annot::Item* item = itemOf(m_liveId)) {
                m_handoverId = m_liveId;
                m_handoverArea = areaOf(*item);
                markTiles(m_handoverArea);
            }
        }
        if (liveNow) {
            // A new stroke: the last one's tiles all now. It may still have
            // been live, when no frame came between letting go of it and the
            // next press: the GPU's goes to the new one, the tiles take it.
            if (const annot::Item* last = itemOf(m_liveId ? m_liveId : m_handoverId))
                markTiles(areaOf(*last));
            m_handoverId = 0;
            if (const annot::Item* item = itemOf(liveNow))
                markTiles(areaOf(*item)); // drawn without it
        }
        m_liveId = liveNow;
        update();
    }
    if (const annot::Item* item = itemOf(m_liveId ? m_liveId : m_handoverId)) {
        m_livePoints = item->points;
        m_liveWidth = std::max(1, item->style.width);
        m_liveColor = item->style.color;
        update();
    } else {
        m_handoverId = 0;
        if (!m_livePoints.isEmpty()) {
            m_livePoints.clear();
            update();
        }
    }
    std::optional<quint32> skip = model->skipped();
    if (m_liveId && !skip)
        skip = m_liveId;
    const std::optional<quint32> unworded = model->unworded();
    const std::optional<quint32> draft = model->draft();
    // Sparse: where the objects reach, measured once for all the tiles. The
    // text being typed is not drawn, so it does not count.
    std::vector<QRect> reach;
    if (m_sparse) {
        const QHash<quint32, int> numbers = annot::numbers(doc);
        reach.reserve(std::size_t(doc.items.size()));
        for (const annot::Item& item : doc.items) {
            if (skip && item.id == *skip)
                continue;
            const int number = item.kind == annot::Kind::Number ? numbers.value(item.id, 1) : 1;
            const QRectF r = annot::bounds(item, doc.dpr, number, annot::formatOf(doc, item.series));
            if (!r.isEmpty())
                reach.push_back(r.toAlignedRect());
        }
    }
    const QRect sample = model->sampleBounds();
    bool any = false;
    int drawn = 0;
    bool pending = false;
    // The tiles to look at: all of them, or the view's nearest the middle
    // first (the others let go of at once).
    std::vector<int> order;
    if (m_limited) {
        const QRect range = editview::tileRange(m_viewport, m_size, kTile, kRing);
        const QList<int> near = editview::nearestTiles(range, m_columns, kTile, m_viewport.center(), kMaxTiles);
        std::vector<bool> wanted(m_tiles.size(), false);
        order.reserve(std::size_t(near.size()));
        for (const int i : near) {
            wanted[std::size_t(i)] = true;
            order.push_back(i);
        }
        for (std::size_t i = 0; i < m_tiles.size(); ++i) {
            Tile& tile = m_tiles[i];
            if (wanted[i] || (!tile.shown && tile.image.isNull()))
                continue;
            tile.image = QImage();
            tile.shown = false;
            tile.dirty = true; // drawn again when it comes back
            tile.upload = true; // its node goes
            any = true;
        }
    } else {
        order.resize(m_tiles.size());
        std::iota(order.begin(), order.end(), 0);
    }
    for (const int i : order) {
        Tile& tile = m_tiles[std::size_t(i)];
        if (!tile.dirty && (!m_limited || tile.shown))
            continue;
        if (m_limited && drawn > 0 && clock.elapsed() >= kPolishBudgetMs) {
            pending = true; // the rest at the next frame
            break;
        }
        if (m_handoverId && drawn > 0 && clock.elapsed() >= kHandoverBudgetMs && tileRect(i).intersects(m_handoverArea)) {
            pending = true; // the GPU shows the stroke there one frame more
            continue;
        }
        tile.dirty = false;
        const QRect area = tileRect(i);
        const bool needed = !m_sparse
            || std::any_of(reach.cbegin(), reach.cend(), [&area](const QRect& r) { return r.intersects(area); });
        if (!needed) {
            if (tile.shown) {
                tile.image = QImage();
                tile.shown = false;
                tile.upload = true; // its node goes
                any = true;
            }
            continue;
        }
        // The texture made of the tile's last picture may still hold it:
        // drawing into it would first copy all of it (QImage detaches), only
        // for the compositor to overwrite every pixel. A new picture costs
        // just the allocation (dragging a big rectangle: 99% of the tiles).
        if (!tile.image.isDetached())
            tile.image = QImage();
        model->compositor().render(doc, model->base(), sample, area, &tile.image, skip, unworded, draft);
        ++drawn;
        tile.shown = true;
        tile.upload = true;
        any = true;
    }
    if (m_handoverId && !pending) {
        m_handoverId = 0; // the tiles have it all: the GPU's goes at this sync
        m_livePoints.clear();
        update();
    }
    if (pending)
        polish();
    if (m_limited)
        m_order.assign(order.begin(), order.end());
    else
        m_order.clear();
    if (m_limited) {
        m_mostShown = std::max<int>(m_mostShown, tilesShown());
        m_mostWanted = std::max<int>(m_mostWanted, int(order.size()));
    }
    if (gesture) {
        m_meter.polishNs.push_back(clock.nsecsElapsed());
        m_meter.polishTiles.push_back(drawn);
    } else if (m_meter.on) {
        reportMeter(model->compositor().patchesMade());
    }
    if (any || m_dropNodes)
        update();
}

void AnnotationCanvas::reportMeter(qint64 patchesNow)
{
    using namespace Qt::StringLiterals;
    Meter m = std::exchange(m_meter, Meter {});
    if (m.polishNs.size() < 8)
        return;
    const auto at = [](std::vector<qint64> v, double q) {
        if (v.empty())
            return 0.0;
        std::ranges::sort(v);
        return v[static_cast<std::size_t>(q * double(v.size() - 1))] / 1e6;
    };
    const int tiles = std::accumulate(m.polishTiles.cbegin(), m.polishTiles.cend(), 0);
    qInfo().noquote() << u"Annotation canvas: %1 polishes, %2 tiles drawn (most %3 at once), drawing p50 %4 ms, p95 %5 ms, max %6 ms; "
                         u"%7 syncs, %8 textures made, sync p50 %9 ms, p95 %10 ms; %11 mosaic patches made"_s
                             .arg(m.polishNs.size())
                             .arg(tiles)
                             .arg(*std::ranges::max_element(m.polishTiles))
                             .arg(at(m.polishNs, 0.5), 0, 'f', 2)
                             .arg(at(m.polishNs, 0.95), 0, 'f', 2)
                             .arg(at(m.polishNs, 1.0), 0, 'f', 2)
                             .arg(m.syncNs.size())
                             .arg(m.textures)
                             .arg(at(m.syncNs, 0.5), 0, 'f', 2)
                             .arg(at(m.syncNs, 0.95), 0, 'f', 2)
                             .arg(patchesNow - m.patchesBefore);
}

// The render thread, the GUI thread waiting: textures of the tiles that
// were drawn again; nothing is drawn here.
QSGNode* AnnotationCanvas::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    QElapsedTimer clock;
    clock.start();
    struct Measured {
        Meter& meter;
        QElapsedTimer& clock;
        ~Measured()
        {
            if (meter.on)
                meter.syncNs.push_back(clock.nsecsElapsed());
        }
    } measured {m_meter, clock};
    if (!old) {
        // The first frame, or the scene graph deleted the nodes (another
        // window): what m_nodes points at is gone, every picture needs its
        // texture again.
        m_nodes.clear();
        forgetLive();
        for (Tile& tile : m_tiles)
            tile.upload = tile.upload || tile.shown;
    }
    if (m_dropNodes) {
        m_dropNodes = false;
        delete old; // its children, and their textures, with it
        old = nullptr;
        m_nodes.clear();
        forgetLive();
    }
    QQuickWindow* win = window();
    if (m_tiles.empty() || !win) {
        delete old;
        m_nodes.clear();
        forgetLive();
        return nullptr;
    }
    QSGNode* root = old ? old : new QSGNode;
    m_nodes.resize(m_tiles.size(), nullptr);
    // Zoomed out on a long picture hundreds of tiles come into view at once, and all their
    // textures made in one frame took 50 ms: a limited canvas makes a few dozen a frame, those
    // nearest the middle first, and asks for the next frame.
    // (The software renderer has no textures to upload, and shows nodes added in a later frame wrongly.)
    const bool software = win->rendererInterface() && win->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    int made = 0;
    bool deferred = false;
    const auto step = [&](std::size_t i) {
        Tile& tile = m_tiles[i];
        QSGImageNode*& node = m_nodes[i];
        if (tile.upload) {
            const bool creating = tile.shown && !tile.image.isNull();
            if (creating && m_limited && !software && made >= kUploadsPerFrame) {
                deferred = true; // the node it has now (an older picture) stays meanwhile
                if (node && m_rectsStale)
                    node->setRect(nodeRect(int(i)));
                if (node && m_filterStale)
                    node->setFiltering(m_linear ? QSGTexture::Linear : QSGTexture::Nearest);
                return;
            }
            tile.upload = false;
            // A new picture gets a new node, as CaptureImage does: the old
            // one goes with its texture (setOwnsTexture).
            if (node) {
                root->removeChildNode(node);
                delete node;
                node = nullptr;
            }
            if (creating) {
                ++made;
                node = win->createImageNode();
                node->setOwnsTexture(true);
                node->setFiltering(m_linear ? QSGTexture::Linear : QSGTexture::Nearest);
                node->setMipmapFiltering(QSGTexture::None);
                node->setTexture(win->createTextureFromImage(tile.image,
                    m_transparent ? QQuickWindow::CreateTextureOptions() : QQuickWindow::TextureIsOpaque));
                if (m_meter.on)
                    ++m_meter.textures;
                node->setSourceRect(QRectF(QPointF(0, 0), tile.image.size()));
                node->setRect(nodeRect(int(i)));
                root->appendChildNode(node);
            }
        } else if (node) {
            if (m_rectsStale)
                node->setRect(nodeRect(int(i)));
            if (m_filterStale)
                node->setFiltering(m_linear ? QSGTexture::Linear : QSGTexture::Nearest);
        }
    };
    if (m_limited && !m_order.empty()) {
        std::vector<bool> seen(m_tiles.size(), false);
        for (const int i : m_order) {
            if (i >= 0 && std::size_t(i) < m_tiles.size()) {
                seen[std::size_t(i)] = true;
                step(std::size_t(i));
            }
        }
        for (std::size_t i = 0; i < m_tiles.size(); ++i) {
            if (!seen[i])
                step(i);
        }
    } else {
        for (std::size_t i = 0; i < m_tiles.size(); ++i)
            step(i);
    }
    if (deferred)
        QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
    m_rectsStale = false;
    m_filterStale = false;
    updateLive(root);
    return root;
}

void AnnotationCanvas::forgetLive()
{
    m_liveRoot = nullptr;
    m_liveChunks.clear();
    m_liveSettled.clear();
    m_liveShown = 0;
}

// The live stroke's triangles (LiveStroke.h): the chunks no new point
// changes are made once, the newest again at each frame; above the tiles.
void AnnotationCanvas::updateLive(QSGNode* root)
{
    const quint32 shown = m_liveId ? m_liveId : m_handoverId;
    if (!shown || m_livePoints.isEmpty()) {
        if (m_liveRoot) {
            root->removeChildNode(m_liveRoot);
            delete m_liveRoot;
        }
        forgetLive();
        return;
    }
    if (m_liveShown != shown || m_liveShownWidth != m_liveWidth || m_liveShownColor != m_liveColor) {
        if (m_liveRoot) {
            root->removeChildNode(m_liveRoot);
            delete m_liveRoot;
        }
        forgetLive();
        m_liveShown = shown;
        m_liveShownWidth = m_liveWidth;
        m_liveShownColor = m_liveColor;
    }
    if (!m_liveRoot)
        m_liveRoot = new QSGNode;
    if (root->lastChild() != m_liveRoot) { // tiles made since go beneath it
        if (m_liveRoot->parent())
            root->removeChildNode(m_liveRoot);
        root->appendChildNode(m_liveRoot);
    }
    const qsizetype count = m_livePoints.size();
    const qsizetype chunks = std::max<qsizetype>(1, (live::pieceCount(count) + live::kChunk - 1) / live::kChunk);
    const qsizetype settled = live::settledPieces(count);
    live::Mesh mesh;
    for (qsizetype c = 0; c < chunks; ++c) {
        if (std::size_t(c) < m_liveChunks.size() && m_liveSettled[std::size_t(c)])
            continue;
        if (std::size_t(c) >= m_liveChunks.size()) {
            auto* node = new QSGGeometryNode;
            auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0, QSGGeometry::UnsignedIntType);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            node->setGeometry(geometry);
            node->setFlag(QSGNode::OwnsGeometry);
            node->setMaterial(new QSGVertexColorMaterial);
            node->setFlag(QSGNode::OwnsMaterial);
            m_liveRoot->appendChildNode(node);
            m_liveChunks.push_back(node);
            m_liveSettled.push_back(false);
        }
        mesh.clear();
        live::appendChunk(mesh, m_livePoints, c, m_liveWidth, 1.0, m_liveColor, m_scaleX, m_scaleY);
        QSGGeometryNode* node = m_liveChunks[std::size_t(c)];
        QSGGeometry* geometry = node->geometry();
        geometry->allocate(int(mesh.vertices.size()), int(mesh.indices.size()));
        static_assert(sizeof(live::Vertex) == sizeof(QSGGeometry::ColoredPoint2D));
        if (!mesh.vertices.empty())
            std::memcpy(geometry->vertexData(), mesh.vertices.data(), mesh.vertices.size() * sizeof(live::Vertex));
        if (!mesh.indices.empty())
            std::memcpy(geometry->indexDataAsUInt(), mesh.indices.data(), mesh.indices.size() * sizeof(quint32));
        node->markDirty(QSGNode::DirtyGeometry);
        m_liveSettled[std::size_t(c)] = (c + 1) * live::kChunk <= settled;
    }
}

// The item leaves its window (or the window its scene graph): the nodes go
// with the scene graph; the pictures go here, drawn again if it comes back.
void AnnotationCanvas::releaseResources()
{
    dropTiles();
}

void AnnotationCanvas::itemChange(ItemChange change, const ItemChangeData& value)
{
    if (change == ItemSceneChange && value.window)
        markAll();
    QQuickItem::itemChange(change, value);
}

} // namespace ws::capture
