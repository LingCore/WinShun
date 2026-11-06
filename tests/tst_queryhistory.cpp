#include "QueryHistory.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;

class QueryHistoryTest : public QObject {
    Q_OBJECT
private slots:
    void exactQueriesAndPersistence()
    {
        QTemporaryDir profile;
        const QString file = profile.filePath(u"nested/queries.json"_s);
        ws::QueryHistory history(file, 3);
        QVERIFY(!history.record(u" \n\t"_s));
        QVERIFY(history.record(u" :doc \"Annual  Report\" !old "_s));
        QVERIFY(history.record(u"F:\\Work\\CaseSensitive"_s));
        QVERIFY(history.record(u"中文 查询"_s));
        QVERIFY(!history.record(u"中文 查询"_s));
        QVERIFY(history.record(u":doc \"Annual  Report\" !old"_s));
        QCOMPARE(history.items().first(), u":doc \"Annual  Report\" !old"_s);
        QVERIFY(history.record(u"four\nwords"_s));
        QCOMPARE(history.items().size(), 3);
        QVERIFY(!history.items().contains(u"F:\\Work\\CaseSensitive"_s));
        ws::QueryHistory restored(file, 3);
        restored.load();
        QCOMPARE(restored.items(), history.items());
        QVERIFY(restored.remove(1));
        QVERIFY(!restored.remove(-1));
        QVERIFY(!restored.remove(9));
        history.load();
        QCOMPARE(history.items(), restored.items());
        QVERIFY(restored.clear());
        history.load();
        QVERIFY(history.items().isEmpty());
    }
    void navigationRestoresDraft()
    {
        QTemporaryDir profile;
        ws::QueryHistory history(profile.filePath(u"queries.json"_s));
        QVERIFY(!history.step(u"draft"_s, 1));
        history.record(u"older"_s);
        history.record(u"latest"_s);
        QVERIFY(!history.step(u"draft"_s, -1));
        QCOMPARE(history.step(u"unfinished  input"_s, 1).value(), u"latest"_s);
        QCOMPARE(history.step(u"latest"_s, 1).value(), u"older"_s);
        QCOMPARE(history.step(u"older"_s, 1).value(), u"older"_s);
        QCOMPARE(history.step({}, -1).value(), u"latest"_s);
        QCOMPARE(history.step({}, -1).value(), u"unfinished  input"_s);
        QVERIFY(!history.step({}, -1));
        history.step(u"draft"_s, 1);
        history.resetNavigation();
        QCOMPARE(history.step(u"edited"_s, 1).value(), u"latest"_s);
        QCOMPARE(history.step({}, -1).value(), u"edited"_s);
        history.remove(0);
        QCOMPARE(history.step(u"after removal"_s, 1).value(), u"older"_s);
    }
    void corruptAndBoundedInput()
    {
        QTemporaryDir profile;
        const QString path = profile.filePath(u"queries.json"_s);
        const auto write = [&](const QByteArray& bytes) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size()); };
        ws::QueryHistory history(path, 2);
        write("[\"same\",\"same\",null,3,\" \",\"second\",\"third\"]");
        history.load();
        QCOMPARE(history.items(), QStringList({u"same"_s, u"second"_s}));
        QVERIFY(!history.record(QString(2049, u'x')));
        QVERIFY(!history.record(QString(QChar::Null)));
        write("{broken json");
        history.load();
        QVERIFY(history.items().isEmpty());
        write(QByteArray(1024 * 1024 + 1, ' '));
        history.load();
        QVERIFY(history.items().isEmpty());
        QVERIFY(history.record(u"works after a damaged file"_s));
        ws::QueryHistory restored(path);
        restored.load();
        QCOMPARE(restored.items(), history.items());
    }
};
QTEST_GUILESS_MAIN(QueryHistoryTest)
#include "tst_queryhistory.moc"
