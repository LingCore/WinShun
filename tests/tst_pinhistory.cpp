#include "AnnotationFormat.h"
#include "CaptureHistory.h"
#include "PinHistory.h"

#include <QJsonDocument>
#include <QSet>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace ws::capture;

namespace {

QImage solidImage(QSize size, QColor color)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img;
}

} // namespace

class PinHistoryTest : public QObject {
    Q_OBJECT

private slots:
    void orderAndNameRoundTrip()
    {
        QCOMPARE(pinHistoryOrderFromName(u"closed"_s), PinHistoryOrder::RecentClosed);
        QCOMPARE(pinHistoryOrderFromName(u"created"_s), PinHistoryOrder::CreatedTime);
        QCOMPARE(pinHistoryOrderFromName(u" CREATED "_s), PinHistoryOrder::CreatedTime);
        QCOMPARE(pinHistoryOrderFromName(u"unknown"_s), PinHistoryOrder::RecentClosed);
        QCOMPARE(pinHistoryOrderName(PinHistoryOrder::RecentClosed), u"closed"_s);
        QCOMPARE(pinHistoryOrderName(PinHistoryOrder::CreatedTime), u"created"_s);
    }

    void sequentialRecallSkipsOpenPins()
    {
        PinHistory history;
        history.setLimit(10);
        history.setOrder(PinHistoryOrder::RecentClosed);

        const quint64 a = history.record({.image = solidImage({16, 16}, Qt::red), .origin = {10, 10}, .createdMs = 100});
        const quint64 b = history.record({.image = solidImage({16, 16}, Qt::green), .origin = {20, 20}, .createdMs = 200});
        const quint64 c = history.record({.image = solidImage({16, 16}, Qt::blue), .origin = {30, 30}, .createdMs = 300});
        QCOMPARE(history.count(), 3);

        QSet<quint64> open;
        const auto isOpen = [&](quint64 id) { return open.contains(id); };

        // Repeated presses restore C -> B -> A -> none.
        auto first = history.nextToRestore(isOpen);
        QVERIFY(first.has_value());
        QCOMPARE(first->id, c);
        open.insert(c);

        auto second = history.nextToRestore(isOpen);
        QVERIFY(second.has_value());
        QCOMPARE(second->id, b);
        open.insert(b);

        auto third = history.nextToRestore(isOpen);
        QVERIFY(third.has_value());
        QCOMPARE(third->id, a);
        open.insert(a);

        QVERIFY(!history.nextToRestore(isOpen).has_value());
    }

    void recentClosedVsCreatedOrder()
    {
        PinHistory history;
        history.setLimit(10);

        const quint64 a = history.record({.image = solidImage({10, 10}, Qt::red), .origin = {1, 1}, .createdMs = 100});
        const quint64 b = history.record({.image = solidImage({10, 10}, Qt::green), .origin = {2, 2}, .createdMs = 200});
        const quint64 c = history.record({.image = solidImage({10, 10}, Qt::blue), .origin = {3, 3}, .createdMs = 300});

        // Close C at t=500, then close A at t=600 (B remains open).
        history.markClosed(c, 500);
        history.markClosed(a, 600);
        const auto openB = [b](quint64 id) { return id == b; };

        // RecentClosed brings back A (closed last) first, then C.
        history.setOrder(PinHistoryOrder::RecentClosed);
        auto r1 = history.nextToRestore(openB);
        QVERIFY(r1.has_value());
        QCOMPARE(r1->id, a);

        // CreatedTime ignores close times and brings back C (created newer than A) first.
        history.setOrder(PinHistoryOrder::CreatedTime);
        auto r2 = history.nextToRestore(openB);
        QVERIFY(r2.has_value());
        QCOMPARE(r2->id, c);

        // A fresh capture at t=700 outranks the pin closed at t=600 in RecentClosed mode too.
        history.setOrder(PinHistoryOrder::RecentClosed);
        const quint64 d = history.record({.image = solidImage({10, 10}, Qt::yellow), .origin = {4, 4}, .createdMs = 700});
        auto r3 = history.nextToRestore(openB);
        QVERIFY(r3.has_value());
        QCOMPARE(r3->id, d);
    }

    void pngEncodingAndGenerationGuard()
    {
        PinHistory history;
        history.setLimit(5);

        const QImage initial = solidImage({20, 12}, QColor(0x22, 0x88, 0xCC));
        const quint64 id = history.record({.image = initial, .createdMs = 100});
        const quint64 gen1 = history.generationOf(id);
        QCOMPARE(gen1, 1ULL);

        // Edit the pin before gen1 finishes encoding in the background.
        const QImage edited = solidImage({24, 16}, QColor(0xEE, 0x44, 0x11));
        const quint64 gen2 = history.updateImage(id, edited, QByteArray("proj"));
        QCOMPARE(gen2, 2ULL);

        // Stale gen1 PNG is dropped; gen2 keeps its edited image.
        history.setEncodedPng(id, gen1, PinHistory::encodePng(initial));
        const PinHistoryEntry* entry = history.find(id);
        QVERIFY(entry != nullptr);
        QVERIFY(!entry->image.isNull());
        QCOMPARE(entry->image.size(), QSize(24, 16));
        QVERIFY(entry->png.isEmpty());

        // Matching gen2 PNG replaces the uncompressed QImage and decodes back faithfully.
        history.setEncodedPng(id, gen2, PinHistory::encodePng(edited));
        entry = history.find(id);
        QVERIFY(entry != nullptr);
        QVERIFY(entry->image.isNull());
        QVERIFY(!entry->png.isEmpty());
        const QImage decoded = PinHistory::resolveImage(*entry);
        QCOMPARE(decoded.size(), QSize(24, 16));
        QCOMPARE(decoded.pixelColor(0, 0), QColor(0xEE, 0x44, 0x11));
    }

    void markClosedRefreshesStateAndLimitPrunesOldest()
    {
        PinHistory history;
        history.setLimit(2);
        history.setOrder(PinHistoryOrder::RecentClosed);

        const quint64 a = history.record({.image = solidImage({8, 8}, Qt::red), .origin = {1, 2}, .createdMs = 100});
        const quint64 b = history.record({.image = solidImage({8, 8}, Qt::green), .origin = {3, 4}, .createdMs = 200});
        history.markClosed(a, 500, solidImage({9, 9}, Qt::cyan), QByteArray("p2"), QPoint(77, 88), u"C:\\a.png"_s);

        // Adding C evicts B (key 200) rather than A (closed at 500).
        const quint64 c = history.record({.image = solidImage({8, 8}, Qt::blue), .origin = {5, 6}, .createdMs = 300});
        QCOMPARE(history.count(), 2);
        QVERIFY(history.find(b) == nullptr);
        const PinHistoryEntry* keptA = history.find(a);
        QVERIFY(keptA != nullptr);
        QCOMPARE(keptA->origin, QPoint(77, 88));
        QCOMPARE(keptA->sourcePath, u"C:\\a.png"_s);
        QCOMPARE(keptA->project, QByteArray("p2"));
        QVERIFY(history.find(c) != nullptr);

        // Setting limit to 0 clears all entries and refuses new records.
        history.setLimit(0);
        QVERIFY(!history.enabled());
        QCOMPARE(history.count(), 0);
        QCOMPARE(history.record({.image = solidImage({8, 8}, Qt::white), .createdMs = 999}), 0ULL);
    }

    void deduplicatesCaptureHistoryIdAndLooksUpClipboard()
    {
        PinHistory history;
        history.setLimit(10);

        const quint64 first = history.record({
            .image = solidImage({12, 12}, Qt::red),
            .captureHistoryId = u"1700000001"_s,
            .clipboardOriginId = u"orig-1"_s,
            .clipboardSeq = 42,
            .createdMs = 100,
        });
        QVERIFY(history.containsCaptureHistoryId(u"1700000001"_s));
        QCOMPARE(history.findByClipboardOrigin(u"orig-1"_s), std::optional<quint64>(first));
        QCOMPARE(history.findByClipboardSeq(42), std::optional<quint64>(first));

        // Recording the same captureHistoryId updates in place rather than duplicating.
        const quint64 again = history.record({
            .image = solidImage({14, 14}, Qt::blue),
            .captureHistoryId = u"1700000001"_s,
            .createdMs = 200,
        });
        QCOMPARE(again, first);
        QCOMPARE(history.count(), 1);
        QCOMPARE(history.find(first)->image.size(), QSize(14, 14));
    }

    void renderHistoryShotSingleAndSpanning()
    {
        // Single monitor with a rectangle annotation inside the selection.
        HistoryShot single;
        single.id = u"1700000010"_s;
        single.screens.push_back({QRect(100, 50, 200, 150), 1.0, solidImage({200, 150}, Qt::white)});
        single.screen = 0;
        single.selection = QRect(20, 30, 80, 60);

        annot::Document doc;
        doc.canvas = QSize(200, 150);
        doc.crop = single.selection;
        annot::Item rect;
        rect.id = 1;
        rect.kind = annot::Kind::Rect;
        rect.points = {QPointF(30, 40), QPointF(70, 70)};
        rect.style.color = qRgb(0xFF, 0x00, 0x00);
        rect.style.width = 4;
        doc.items.append(rect);
        single.document = QJsonDocument(annot::toJson(doc)).toJson(QJsonDocument::Compact);

        const auto rendered = renderHistoryShot(single);
        QVERIFY(rendered.has_value());
        QCOMPARE(rendered->origin, QPoint(120, 80));
        QCOMPARE(rendered->image.size(), QSize(80, 60));
        QVERIFY(!rendered->project.isEmpty());
        // The rectangle outline sits at local (10, 10) inside the 80x60 crop.
        QCOMPARE(rendered->image.pixelColor(10, 10), QColor(0xFF, 0x00, 0x00));
        QCOMPARE(rendered->image.pixelColor(0, 0), QColor(Qt::white));

        // Spanning two monitors side by side: left monitor red, right monitor blue.
        HistoryShot spanning;
        spanning.id = u"1700000020"_s;
        spanning.screens.push_back({QRect(0, 0, 100, 100), 1.0, solidImage({100, 100}, QColor(0xDD, 0x22, 0x22))});
        spanning.screens.push_back({QRect(100, 0, 100, 100), 1.0, solidImage({100, 100}, QColor(0x22, 0x44, 0xDD))});
        spanning.spans = true;
        spanning.screen = -1;
        spanning.selection = QRect(80, 10, 40, 30);

        const auto spanRendered = renderHistoryShot(spanning);
        QVERIFY(spanRendered.has_value());
        QCOMPARE(spanRendered->origin, QPoint(80, 10));
        QCOMPARE(spanRendered->image.size(), QSize(40, 30));
        QCOMPARE(spanRendered->image.pixelColor(5, 5), QColor(0xDD, 0x22, 0x22));
        QCOMPARE(spanRendered->image.pixelColor(35, 5), QColor(0x22, 0x44, 0xDD));
    }
};

QTEST_MAIN(PinHistoryTest)
#include "tst_pinhistory.moc"
