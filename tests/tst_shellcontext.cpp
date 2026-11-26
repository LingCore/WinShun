// The shell's own context menu on results (src/app/platform/ShellContextMenu):
// which handler the items get, without the menu on screen. Files of several
// folders are on one menu together (GitHub #464), as files of one folder
// are; told by a command that is run on them all.
#include "platform/ShellContextMenu.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <objbase.h>

using namespace Qt::StringLiterals;

namespace {

QString made(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("x") == 1 ? path : QString();
}

} // namespace

class ShellContextTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(SUCCEEDED(::OleInitialize(nullptr))); }
    void cleanupTestCase() { ::OleUninitialize(); }

    void menuForItems_data()
    {
        QTest::addColumn<QStringList>("names"); // under the scratch folder; a name ending in '/' is a folder
        QTest::newRow("one file") << QStringList {u"a/one.txt"_s};
        QTest::newRow("files of one folder") << QStringList {u"a/one.txt"_s, u"a/two.txt"_s};
        QTest::newRow("files of two folders") << QStringList {u"a/one.txt"_s, u"b/two.txt"_s};
        QTest::newRow("the one asked on in the deeper folder") << QStringList {u"a/deep/er/one.txt"_s, u"two.txt"_s, u"b/three.md"_s};
        QTest::newRow("a file and a folder elsewhere") << QStringList {u"a/one.txt"_s, u"b/inner/"_s};
        QTest::newRow("folders of two folders") << QStringList {u"a/first/"_s, u"b/second/"_s};
        QTest::newRow("Chinese names") << QStringList {u"甲/报告 一.txt"_s, u"乙/报告 二.txt"_s};
    }

    // Every item is on the menu: its 复制, 剪切, 删除 and 属性 are there, and
    // 删除 (Shift held: for good, nothing left in the recycle bin) takes
    // them all.
    void menuForItems()
    {
        QFETCH(QStringList, names);
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        QStringList paths;
        for (const QString& name : std::as_const(names)) {
            const QString path = scratch.filePath(name.endsWith(u'/') ? name.chopped(1) : name);
            if (name.endsWith(u'/'))
                QVERIFY(QDir().mkpath(path));
            else
                QVERIFY(!made(path).isEmpty());
            paths.append(path);
        }

        const ws::shellcontext::Offer offer = ws::shellcontext::offered(paths);
        QVERIFY(!offer.singleFallback);
        for (const QString& verb : {u"copy"_s, u"cut"_s, u"delete"_s, u"properties"_s})
            QVERIFY2(offer.verbs.contains(verb), qPrintable(verb + u" not among: "_s + offer.verbs.join(u' ')));

        QVERIFY(ws::shellcontext::invokeQuietly(paths, L"delete", true));
        for (const QString& path : std::as_const(paths))
            QTRY_VERIFY2(!QFileInfo::exists(path), qPrintable(path));
    }

    // Nothing there to ask about: no menu, nothing run.
    void nothingForMissingItems()
    {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QStringList gone {scratch.filePath(u"a/none.txt"_s), scratch.filePath(u"b/none.txt"_s)};
        QVERIFY(ws::shellcontext::offered(gone).verbs.isEmpty());
        QVERIFY(!ws::shellcontext::invokeQuietly(gone, L"delete", true));
    }
};

QTEST_MAIN(ShellContextTest)
#include "tst_shellcontext.moc"
