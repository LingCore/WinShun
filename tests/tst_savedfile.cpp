// What a click on the "截图已保存" notification shows (saved::target,
// GitHub #99): the file in its folder, the folder when the file is gone,
// nothing when that is gone too.
#include "SavedFile.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace ws;

class SavedFileTest : public QObject {
    Q_OBJECT
private slots:
    void fileThere()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"shot.png"_s);
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        const auto t = saved::target(file);
        QCOMPARE(t.show, saved::Show::File);
        QCOMPARE(t.path, file);
    }

    void fileGoneFolderStays()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto t = saved::target(dir.filePath(u"gone.png"_s));
        QCOMPARE(t.show, saved::Show::Folder);
        QCOMPARE(QDir::cleanPath(t.path), QDir::cleanPath(dir.path()));
    }

    void bothGone()
    {
        QString file;
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            file = dir.filePath(u"gone.png"_s);
        } // removed with its folder
        QCOMPARE(saved::target(file).show, saved::Show::Nothing);
    }

    void folderIsNotAFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A path that names a folder: not "the file", so its parent is shown.
        QVERIFY(QDir(dir.path()).mkdir(u"sub"_s));
        const auto t = saved::target(dir.filePath(u"sub"_s));
        QCOMPARE(t.show, saved::Show::Folder);
    }

    void emptyPath()
    {
        QCOMPARE(saved::target(QString()).show, saved::Show::Nothing);
    }
};

QTEST_APPLESS_MAIN(SavedFileTest)
#include "tst_savedfile.moc"
