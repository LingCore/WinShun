// When a portable copy offers to update the index service it installed
// (ServiceUpdate.h, GitHub #243).

#include "ServiceUpdate.h"

#include <QTest>

using namespace ws::serviceupdate;
using namespace Qt::StringLiterals;

class ServiceUpdateTest : public QObject {
    Q_OBJECT

    static Facts older()
    {
        Facts f;
        f.portable = true;
        f.installable = true;
        f.ownCopy = true;
        f.serviceVersion = u"0.22.0"_s;
        f.appVersion = u"0.23.0"_s;
        return f;
    }

private slots:
    void parsesThreeNumbers()
    {
        QCOMPARE(parseVersion(u"0.22.0"_s), (std::array<int, 3> {0, 22, 0}));
        QCOMPARE(parseVersion(u"1.10.12"_s), (std::array<int, 3> {1, 10, 12}));
        QVERIFY(!parseVersion(QString()));
        QVERIFY(!parseVersion(u"0.22"_s));
        QVERIFY(!parseVersion(u"0.22.0-beta"_s));
        QVERIFY(!parseVersion(u"0.22.0.1"_s));
        QVERIFY(!parseVersion(u"0.-1.0"_s));
    }

    void olderOwnServiceIsOffered()
    {
        QVERIFY(due(older()));
        Facts f = older();
        f.serviceVersion = u"0.9.5"_s; // numbers, not text: 9 < 22
        f.appVersion = u"0.22.0"_s;
        QVERIFY(due(f));
        f.serviceVersion = u"0.22.0"_s;
        f.appVersion = u"0.22.1"_s;
        QVERIFY(due(f));
    }

    void notOffered_data()
    {
        QTest::addColumn<int>("what");
        QTest::newRow("installed copy") << 0;
        QTest::newRow("cannot install") << 1;
        QTest::newRow("another copy's service") << 2;
        QTest::newRow("same version") << 3;
        QTest::newRow("newer service") << 4;
        QTest::newRow("service has not answered") << 5;
        QTest::newRow("unreadable version") << 6;
    }
    void notOffered()
    {
        QFETCH(int, what);
        Facts f = older();
        switch (what) {
        case 0: f.portable = false; break;
        case 1: f.installable = false; break;
        case 2: f.ownCopy = false; break;
        case 3: f.serviceVersion = f.appVersion; break;
        case 4: f.serviceVersion = u"0.30.0"_s; break;
        case 5: f.serviceVersion.clear(); break;
        case 6: f.serviceVersion = u"0.22"_s; break;
        }
        QVERIFY(!due(f));
    }
};

QTEST_APPLESS_MAIN(ServiceUpdateTest)
#include "tst_serviceupdate.moc"
