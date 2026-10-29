#include "ClipModel.h"
#include "FileIconProvider.h"
#include "HotkeySteps.h"

#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace ws;
using namespace Qt::StringLiterals;

// ClipModel only asks it for an icon address; the real one needs the shell.
QString FileIconProvider::iconUrl(const QString&, bool)
{
    return {};
}

namespace {

constexpr qint64 kStart = 1'790'000'000'000;

ClipCapture text(const QString& s)
{
    ClipCapture c;
    c.kind = kindOfText(s);
    c.text = s;
    c.source = u"记事本"_s;
    return c;
}

// Six entries, newest first: 苹果 (pinned), 香蕉, 苹果派 (in 工作), 橙子, 梨 (pinned), 葡萄.
struct Fixture {
    QTemporaryDir dir;
    ClipStore store {dir.path()};
    ClipModel model {&store};
    qint64 work = 0;

    Fixture()
    {
        store.open(kStart);
        work = store.addGroup(u"工作"_s);
        qint64 id[6] {};
        const QStringList names {u"葡萄"_s, u"梨"_s, u"橙子"_s, u"苹果派"_s, u"香蕉"_s, u"苹果"_s};
        for (int i = 0; i < 6; ++i)
            id[i] = store.add(text(names[i]), kStart + i);
        const qint64 pinned[] {id[1], id[5]};
        store.setGroup(pinned, ClipStore::kPinned);
        const qint64 grouped[] {id[3]};
        store.setGroup(grouped, work);
        show({});
    }

    void show(const ClipFilter& filter, const QString& query = {})
    {
        model.setRows(store.find(filter), query);
    }
    QStringList picked() const
    {
        QStringList names;
        for (const Clip* c : model.selection())
            names.append(c->text);
        return names;
    }
};

} // namespace

class ClipSelectTest : public QObject {
    Q_OBJECT

private slots:
    void imageFilePresentationKeepsItsOriginalKind_data()
    {
        QTest::addColumn<ClipKind>("kind");
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("thumbnail");
        QTest::newRow("wechat-jpeg") << ClipKind::Files << u"手机截图 #+ 100%.JPG"_s << true;
        QTest::newRow("png") << ClipKind::Files << u"picture.png"_s << true;
        QTest::newRow("gif") << ClipKind::Files << u"animation.gif"_s << true;
        QTest::newRow("image-path") << ClipKind::Path << u"picture.jpeg"_s << true;
        QTest::newRow("document") << ClipKind::Files << u"document.pdf"_s << false;
        QTest::newRow("text") << ClipKind::Text << u"picture.jpg"_s << false;
        QTest::newRow("folder-with-image-suffix") << ClipKind::Files << u"folder.jpg"_s << false;
        QTest::newRow("missing") << ClipKind::Files << u"missing.jpg"_s << false;
        QTest::newRow("multiple") << ClipKind::Files << u"first.jpg\nsecond.png"_s << false;
        QTest::newRow("network") << ClipKind::Files << u"\\\\unavailable\\share\\picture.jpg"_s << false;
    }

    void imageFilePresentationKeepsItsOriginalKind()
    {
        QFETCH(ClipKind, kind);
        QFETCH(QString, name);
        QFETCH(bool, thumbnail);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = name.startsWith(u"\\\\") ? name : dir.filePath(name);
        if (name == u"folder.jpg") {
            QVERIFY(QDir().mkpath(path));
        } else if (!name.startsWith(u"\\\\") && !name.contains(u'\n') && name != u"missing.jpg") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("metadata-only fixture: the model must not decode images");
        }
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(kStart));
        ClipCapture capture;
        capture.kind = kind;
        capture.text = path;
        capture.source = u"Weixin"_s;
        const qint64 id = store.add(capture, kStart);
        QVERIFY(id > 0);
        ClipModel model(&store);
        model.setRows(store.find({}), {});
        const QModelIndex index = model.index(0);
        const QString image = model.data(index, ClipModel::ImageRole).toString();
        QCOMPARE(image, thumbnail ? QUrl::fromLocalFile(path).toString() : QString());
        QCOMPARE(model.data(index, ClipModel::KindRole).toInt(), static_cast<int>(kind));
        QCOMPARE(store.payload(id).text, path);
        if (thumbnail) {
            QSignalSpy changes(&model, &ClipModel::dataChanged);
            QVERIFY(QFile::remove(path));
            model.refreshTimes();
            QCOMPARE(changes.size(), 1);
            QVERIFY(changes.front().at(2).value<QList<int>>().contains(ClipModel::ImageRole));
            QVERIFY(model.data(index, ClipModel::ImageRole).toString().isEmpty());
        }
    }

    // Ctrl+A in 全部: what is in no group, in the order listed.
    void allTabSkipsPinnedAndGrouped()
    {
        Fixture f;
        QCOMPARE(f.model.count(), 6);
        QCOMPARE(f.model.selectListed(false), 3);
        QCOMPARE(f.picked(), (QStringList {u"香蕉"_s, u"橙子"_s, u"葡萄"_s}));
        QVERIFY(!f.model.isSelected(0)); // 苹果, pinned
    }

    // A group's tab (and 固定's): all of its entries.
    void groupTabTakesTheWholeGroup()
    {
        Fixture f;
        f.show({ClipFilter::Category::Group, ClipStore::kPinned, {}});
        QCOMPARE(f.model.count(), 2);
        QCOMPARE(f.model.selectListed(true), 2);
        QCOMPARE(f.picked().size(), 2);
        f.show({ClipFilter::Category::Group, f.work, {}});
        QCOMPARE(f.model.selectListed(true), 1);
        QCOMPARE(f.picked(), (QStringList {u"苹果派"_s})); // instead of the earlier picks
    }

    void searchLimitsIt()
    {
        Fixture f;
        f.show({ClipFilter::Category::All, 0, u"苹果"_s}, u"苹果"_s);
        QCOMPARE(f.model.count(), 2); // 苹果 (pinned) and 苹果派 (in 工作)
        QCOMPARE(f.model.selectListed(false), 0);
        QVERIFY(f.picked().isEmpty());
        f.show({ClipFilter::Category::All, 0, u"香蕉"_s}, u"香蕉"_s);
        QCOMPARE(f.model.selectListed(false), 1);
        QCOMPARE(f.picked(), (QStringList {u"香蕉"_s}));
    }

    void nothingToPickKeepsThePicks()
    {
        Fixture f;
        f.model.toggle(1);
        QCOMPARE(f.picked().size(), 1);
        f.show({ClipFilter::Category::All, 0, u"梨"_s}, u"梨"_s); // only a pinned one
        QCOMPARE(f.model.selectListed(false), 0);
        QCOMPARE(f.picked().size(), 1);
    }

    void againChangesNothing()
    {
        Fixture f;
        QSignalSpy spy(&f.model, &ClipModel::selectionChanged);
        QCOMPARE(f.model.selectListed(false), 3);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(f.model.selectListed(false), 3);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(f.model.selectedCount(), 3);
    }

    // 删除全部, then Ctrl+Z: one remove, one restore; the pinned and the
    // grouped stay all the while and come back where they were.
    void deleteAllAndUndo()
    {
        Fixture f;
        const std::vector<qint64> ids = f.model.listedIds(false);
        QCOMPARE(ids.size(), std::size_t(3));
        std::vector<ClipRecord> removed = f.store.remove(ids);
        QCOMPARE(removed.size(), std::size_t(3));
        f.show({});
        QCOMPARE(f.model.count(), 3);
        QCOMPARE(f.store.clips().size(), std::size_t(3));

        f.store.restore(std::move(removed));
        f.show({});
        QCOMPARE(f.model.count(), 6);
        QCOMPARE(f.model.listedIds(false).size(), std::size_t(3));
        QCOMPARE(f.store.groupSize(ClipStore::kPinned), 2);
        QCOMPARE(f.store.groupSize(f.work), 1);
    }

    // A hold of the clipboard's hotkey is on an entry (GitHub #107): a copy
    // coming in above moves it a row down, it stays the one; gone, its row is
    // -1 and letting go pastes nothing (Clipboard::finishSteps).
    void heldEntryFollowsItsRow()
    {
        Fixture f;
        const qint64 held = f.model.idAt(ws::hotkeysteps::stepRow(f.model.rowOf(f.model.idAt(0)), f.model.count()));
        QCOMPARE(f.model.rowOf(held), 1);
        f.store.add(text(u"新复制的"_s), kStart + 100);
        f.show({});
        QCOMPARE(f.model.rowOf(held), 2);
        QCOMPARE(ws::hotkeysteps::stepRow(f.model.rowOf(held), f.model.count()), 3);
        const qint64 last = f.model.idAt(f.model.count() - 1);
        QCOMPARE(ws::hotkeysteps::stepRow(f.model.rowOf(last), f.model.count()), f.model.count() - 1);
        const qint64 gone[] {held};
        f.store.remove(gone);
        f.show({});
        QCOMPARE(f.model.rowOf(held), -1);
    }

    // Deleted entries leave the picks (the whole list picked, then deleted).
    void deletedEntriesAreNoLongerPicked()
    {
        Fixture f;
        QCOMPARE(f.model.selectListed(false), 3);
        f.store.remove(f.model.listedIds(false));
        f.show({});
        QCOMPARE(f.model.selectedCount(), 0);
    }

    void manyEntries()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        store.setLimits({0, 0}, kStart); // no limit on the number
        ClipModel model(&store);
        constexpr int n = 4000;
        for (int i = 0; i < n; ++i)
            store.add(text(u"条目 %1"_s.arg(i)), kStart + i);
        model.setRows(store.find({}), {});
        QCOMPARE(model.count(), n);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(model.selectListed(false), n);
        QVERIFY2(timer.elapsed() < 500, qPrintable(u"selecting took %1 ms"_s.arg(timer.elapsed())));
        QCOMPARE(model.selectedCount(), n);
        timer.restart();
        const std::vector<qint64> ids = model.listedIds(false);
        std::vector<ClipRecord> removed = store.remove(ids);
        model.setRows(store.find({}), {});
        QVERIFY2(timer.elapsed() < 5000, qPrintable(u"deleting took %1 ms"_s.arg(timer.elapsed())));
        QCOMPARE(model.count(), 0);
        QCOMPARE(model.selectedCount(), 0);
        store.restore(std::move(removed));
        model.setRows(store.find({}), {});
        QCOMPARE(model.count(), n);
    }
};

QTEST_MAIN(ClipSelectTest)
#include "tst_clipselect.moc"
