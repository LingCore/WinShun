#include "PluginReadme.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QtTest>

using namespace Qt::StringLiterals;

class PluginReadmeTest : public QObject {
    Q_OBJECT
    static void write(const QString& path, const QByteArray& bytes)
    { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size()); }
private slots:
    void formats_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("markdown") << u"README.md"_s << QByteArray("# Heading\n\n**body** [site](https://example.org/help)\n");
        QTest::newRow("html") << u"README.html"_s << QByteArray("<html><h1>Heading</h1><p><b>body</b> <a href='https://example.org/help'>site</a></p></html>");
        QTest::newRow("htm") << u"README.htm"_s << QByteArray("<h1>Heading</h1><p>body site</p>");
        QTest::newRow("plain") << u"README.txt"_s << QByteArray("Heading\n\nbody site");
        QTest::newRow("utf16") << u"README.txt"_s << QByteArray::fromHex("fffe480065006100640069006e0067000a0062006f006400790020007300690074006500");
    }
    void formats()
    {
        QFETCH(QString, name); QFETCH(QByteArray, bytes);
        QTemporaryDir profile;
        write(profile.filePath(name), bytes);
        QVERIFY(ws::readme::exists(profile.path()));
        const auto result = ws::readme::load(profile.path());
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QTextDocument doc;
        doc.setHtml(result.html);
        QVERIFY(doc.toPlainText().contains(u"Heading"));
        QVERIFY(doc.toPlainText().contains(u"body"));
        QVERIFY(doc.toPlainText().contains(u"site"));
    }
    void passiveContent()
    {
        QTemporaryDir profile;
        write(profile.filePath(u"README.html"_s), "<h1>Guide</h1><script>alert('bad')</script><img src='file:///private.png' alt='illustration'><a href='file:///run.exe'>local</a><a href='javascript:alert(1)'>script</a><a href='https://example.org/help'>web</a>");
        const auto result = ws::readme::load(profile.path());
        QVERIFY(result.error.isEmpty());
        QVERIFY(!result.html.contains(u"file:"));
        QVERIFY(!result.html.contains(u"javascript:"));
        QVERIFY(!result.html.contains(u"alert("));
        QVERIFY(result.html.contains(u"illustration"));
        QVERIFY(result.html.contains(u"https://example.org/help"));
        QVERIFY(!result.html.contains(u"<img"));
    }
    void boundedAndMissing()
    {
        QTemporaryDir profile;
        QVERIFY(!ws::readme::exists(profile.path()));
        QVERIFY(!ws::readme::load(profile.path()).error.isEmpty());
        write(profile.filePath(u"README.md"_s), QByteArray(1024 * 1024 + 1, 'x'));
        QVERIFY(!ws::readme::load(profile.path()).error.isEmpty());
    }
    void literalTextAndLinks()
    {
        QTemporaryDir profile;
        write(profile.filePath(u"README.txt"_s), "<script>literal</script> **not bold** [not a link](file:///secret)");
        const auto result = ws::readme::load(profile.path());
        QTextDocument doc;
        doc.setHtml(result.html);
        QCOMPARE(doc.toPlainText(), u"<script>literal</script> **not bold** [not a link](file:///secret)"_s);
        QVERIFY(ws::readme::webLink(u"https://example.org/help?q=word#section"_s));
        QVERIFY(ws::readme::webLink(u"http://localhost/help"_s));
        for (const auto& link : {u"file:///run.exe"_s, u"javascript:run()"_s, u"shell:AppsFolder"_s, u"relative.md"_s, u"https:"_s, u"https://user:pass@example.org"_s})
            QVERIFY(!ws::readme::webLink(link));
    }
    // A plugin's 主页 (the installed card's menu, the library's entry: GitHub
    // #248) is offered and opened only for what webLink takes. A manifest's
    // or the list's "homepage" is any string, and what is opened is handed to
    // the shell, which starts a program as readily as a page: none of these.
    void homepageIsOnlyAnAddressOfTheWeb()
    {
        QVERIFY(ws::readme::webLink(u"https://github.com/LingCore/WinShun-plugins"_s));
        QVERIFY(ws::readme::webLink(u"HTTPS://Example.org/Plugin"_s)); // the scheme in capitals is the same scheme
        for (const QString& homepage : {u"calc.exe"_s, u"C:/Windows/System32/calc.exe"_s, u"//server/share/tool.exe"_s,
                 u"file:///C:/Users/Public/tool.bat"_s, u"ms-settings:display"_s, u"mailto:someone@example.org"_s,
                 u"search-ms:query=x"_s, u"http:"_s, u"https:///no-host"_s, u" "_s, QString()}) {
            QVERIFY2(!ws::readme::webLink(homepage), qPrintable(homepage));
        }
    }
};
QTEST_MAIN(PluginReadmeTest)
#include "tst_pluginreadme.moc"
