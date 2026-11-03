#pragma once

#include "AnnotationModel.h"

#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <vector>

class QSGGeometryNode;
class QSGImageNode;

namespace ws::capture {

// The annotations as the overlay shows them (docs/design-capture-roadmap.md,
// 3.3): the compositor's picture in 256 x 256 image px tiles, each a texture
// of its own, so a change draws and uploads again only the tiles it reaches.
// The tiles are drawn on the GUI thread (updatePolish), where the model and
// its compositor live; the render thread only makes textures of the finished
// pictures. The model never hands a picture through a property: the canvas
// reads model->base() and the document itself.
class AnnotationCanvas : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(ws::capture::AnnotationModel* model READ model WRITE setModel NOTIFY modelChanged FINAL)
    // Item px per image px, as the CaptureImage beneath maps the same picture.
    Q_PROPERTY(qreal scaleX READ scaleX WRITE setScaleX NOTIFY scaleChanged FINAL)
    Q_PROPERTY(qreal scaleY READ scaleY WRITE setScaleY NOTIFY scaleChanged FINAL)
    // Only the tiles some object reaches have a texture; elsewhere what is
    // beneath (the CaptureImage of the same picture) shows the same pixels.
    // The overlay's way, which keeps a mostly empty screen nearly free.
    Q_PROPERTY(bool sparse READ sparse WRITE setSparse NOTIFY sparseChanged FINAL)
    // The editing window's: with `limited`, only the tiles `viewport` (the
    // part of the picture that shows, image px) reaches, and one ring of
    // tiles more, have pictures and textures; the others are let go of, and
    // drawn again when they come into view, nearest the middle first and a
    // few milliseconds at a time (a 1080 x 30000 picture is 590 tiles). Not
    // limited: all the tiles, as in the overlay.
    Q_PROPERTY(bool limited READ limited WRITE setLimited NOTIFY limitedChanged FINAL)
    Q_PROPERTY(QRectF viewport READ viewport WRITE setViewport NOTIFY viewportChanged FINAL)
    // Smoothed (linear) when the picture is shown smaller than it is; sharp
    // (nearest) at 100 % and larger, where it must be pixel for pixel.
    Q_PROPERTY(bool linear READ linear WRITE setLinear NOTIFY linearChanged FINAL)
    // The presenter's (GitHub #189): the pen stroke under the mouse is left
    // out of the tiles and drawn by the GPU (LiveStroke.h) until it is let
    // go of, so a long stroke costs no more at each move than a short one.
    // Not on the software scene graph, which draws no triangles.
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged FINAL)

public:
    static constexpr int kTile = 256;

    explicit AnnotationCanvas(QQuickItem* parent = nullptr);
    ~AnnotationCanvas() override;

    AnnotationModel* model() const { return m_model; }
    void setModel(AnnotationModel* model);
    qreal scaleX() const { return m_scaleX; }
    void setScaleX(qreal scale);
    qreal scaleY() const { return m_scaleY; }
    void setScaleY(qreal scale);
    bool sparse() const { return m_sparse; }
    void setSparse(bool sparse);
    bool limited() const { return m_limited; }
    void setLimited(bool limited);
    QRectF viewport() const { return m_viewport; }
    void setViewport(const QRectF& viewport);
    bool linear() const { return m_linear; }
    void setLinear(bool linear);
    bool live() const { return m_live; }
    void setLive(bool live);
    // The item the GPU draws now (0: none), for the tests.
    quint32 liveItem() const { return m_liveId; }
    // The stroke let go of whose tiles are still being drawn (0: none).
    quint32 handoverItem() const { return m_handoverId; }
    // The tiles that have a picture now (what the tests see of sparse mode).
    int tilesShown() const;
    // The most tiles a limited canvas keeps (the nearest to the middle of the view).
    static constexpr int kMaxTiles = 480;
    static constexpr int kRing = 1;
    static constexpr int kPolishBudgetMs = 8;
    // The textures a limited canvas makes in one frame; the rest follow in the next ones.
    static constexpr int kUploadsPerFrame = 8;
    // A live stroke let go of: what drawing its tiles back may take in one frame.
    static constexpr int kHandoverBudgetMs = 3;

signals:
    void modelChanged();
    void scaleChanged();
    void sparseChanged();
    void limitedChanged();
    void viewportChanged();
    void linearChanged();
    void liveChanged();

protected:
    void updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void releaseResources() override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    struct Tile {
        // The compositor's pixels, rendered into again and again (a new
        // picture while a texture still holds the last one); kept, so a new
        // window needs no new render.
        QImage image;
        bool shown = false; // has a picture (sparse: some object reaches it)
        bool dirty = false; // to render at the next polish
        bool upload = false; // its texture to make, or to remove, at the next sync
    };

    void onChanged(const QRectF& dirty);
    void markAll();
    void markTiles(const QRect& area);
    void dropTiles();
    QRect tileRect(int index) const;
    QRectF nodeRect(int index) const;

    QPointer<AnnotationModel> m_model;
    qreal m_scaleX = 1;
    qreal m_scaleY = 1;
    bool m_sparse = false;
    bool m_limited = false;
    QRectF m_viewport;
    bool m_linear = false;
    bool m_filterStale = false; // m_linear changed: every node's filtering
    QSize m_size; // the picture's, image px
    int m_columns = 0;
    int m_rows = 0;
    std::vector<Tile> m_tiles; // row by row
    // The render thread's, touched only in updatePaintNode() (while the GUI
    // thread waits): the node of each tile, null where there is none.
    std::vector<QSGImageNode*> m_nodes;
    std::vector<int> m_order; // a limited canvas: the tiles it wants, nearest the middle of the view first
    QTimer m_viewMoving; // a limited canvas's view moved a moment ago: measured like a gesture
    int m_mostShown = 0; // a limited canvas: the most tiles it kept, and the most it was asked for, for the log
    int m_mostWanted = 0;
    bool m_dropNodes = false; // the tiles were let go of: so are their nodes, at the next sync
    bool m_rectsStale = true; // the scale changed: every node's rectangle
    // The compositor draws on nothing (the presenter's live desktop): the
    // tiles' textures have alpha. Read on the GUI thread (updatePolish).
    bool m_transparent = false;
    // The live stroke (`live`): its item, and what polish copied of it for
    // the render thread (read in updatePaintNode, the GUI thread waiting).
    bool m_live = false;
    quint32 m_liveId = 0;
    // Let go of: the stroke the tiles are taking back, a few each frame, and where it is (image px).
    quint32 m_handoverId = 0;
    QRect m_handoverArea;
    QList<QPointF> m_livePoints;
    int m_liveWidth = 1;
    QRgb m_liveColor = 0;
    // The render thread's: the node holding the chunks' triangles, each
    // chunk's node and whether no later point changes it.
    QSGNode* m_liveRoot = nullptr;
    std::vector<QSGGeometryNode*> m_liveChunks;
    std::vector<bool> m_liveSettled;
    quint32 m_liveShown = 0;
    int m_liveShownWidth = 0;
    QRgb m_liveShownColor = 0;
    void updateLive(QSGNode* root);
    void forgetLive(); // the nodes went with the root

    // What a gesture cost here, logged when it ends (info): each polish's
    // tiles and drawing time (GUI thread), each sync's textures and time
    // (render thread, the GUI thread waiting: no lock needed).
    struct Meter {
        bool on = false;
        std::vector<qint64> polishNs;
        std::vector<int> polishTiles;
        std::vector<qint64> syncNs;
        int textures = 0;
        qint64 patchesBefore = 0;
    };
    Meter m_meter;
    void reportMeter(qint64 patchesNow);
};

} // namespace ws::capture
