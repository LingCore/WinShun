// winshun:// links (GitHub #358, src/core/WinShunUri): what a link asks for.
#include "WinShunUri.h"

#include <QTest>

using namespace ws::winshunuri;
using namespace Qt::StringLiterals;

class TestWinShunUri : public QObject {
    Q_OBJECT

private:
    static void expectSearch(const QString& link, const QString& text)
    {
        const Request r = parse(link);
        QVERIFY2(r.kind == Kind::Search, qPrintable(link));
        QCOMPARE(r.text, text);
        QVERIFY(r.page.isEmpty());
    }
    static void expectOpen(const QString& link)
    {
        const Request r = parse(link);
        QVERIFY2(r.kind == Kind::Open, qPrintable(link));
        QVERIFY(r.text.isEmpty());
        QVERIFY(r.page.isEmpty());
    }
    static void expectSettings(const QString& link, const QString& page)
    {
        const Request r = parse(link);
        QVERIFY2(r.kind == Kind::Settings, qPrintable(link));
        QCOMPARE(r.page, page);
        QVERIFY(r.text.isEmpty());
    }

private slots:
    void searchPath()
    {
        expectSearch(u"winshun://search/report"_s, u"report"_s);
        expectSearch(u"WinShun://SEARCH/Report"_s, u"Report"_s); // the scheme and the word in any case, the text as it is
        expectSearch(u"  winshun://search/report \r\n"_s, u"report"_s); // what a shell leaves around it
        expectSearch(u"winshun:search/report"_s, u"report"_s); // no slashes
    }

    void searchPathIsEverythingAfterTheWord()
    {
        expectSearch(u"winshun://search/D:/Work/contracts"_s, u"D:/Work/contracts"_s);
        expectSearch(u"winshun://search/D:\\Work\\"_s, u"D:\\Work\\"_s); // a backslash at the end stays
        expectSearch(u"winshun://search/a/b/"_s, u"a/b/"_s);
        expectSearch(u"winshun://search/what?"_s, u"what?"_s);
        expectSearch(u"winshun://search/a?q=b"_s, u"a?q=b"_s); // not a form: it is typed as it is
        expectSearch(u"winshun://search/tag#todo"_s, u"tag#todo"_s);
        expectSearch(u"winshun://search/1+1"_s, u"1+1"_s); // "+" is no space in this form
        expectSearch(u"winshun://search/a%2Bb"_s, u"a+b"_s);
    }

    void percentDecodedAsUtf8()
    {
        expectSearch(u"winshun://search/%E5%90%88%E5%90%8C%E7%BC%96%E5%8F%B7123"_s, u"合同编号123"_s);
        expectSearch(u"winshun://search/合同编号123"_s, u"合同编号123"_s); // raw Unicode too (Obsidian)
        expectSearch(u"winshun://search/my%20file"_s, u"my file"_s);
        expectSearch(u"winshun://search/%e5%90%88"_s, u"合"_s); // lower-case hex
        expectSearch(u"winshun://search/100%"_s, u"100%"_s); // not an escape: stays
        expectSearch(u"winshun://search/%ZZ1"_s, u"%ZZ1"_s);
        expectSearch(u"winshun://search/%F0%9F%98%80"_s, QString::fromUcs4(U"\U0001F600")); // beyond the BMP
    }

    void searchForm()
    {
        expectSearch(u"winshun://search?q=report"_s, u"report"_s);
        expectSearch(u"winshun://search/?q=report"_s, u"report"_s); // a slash a browser may add
        expectSearch(u"winshun://search?q=my+file"_s, u"my file"_s); // "+" is a space here
        expectSearch(u"winshun://search?q=a%2Bb"_s, u"a+b"_s); // and %2B a plus
        expectSearch(u"winshun://search?q=%E5%90%88%E5%90%8C"_s, u"合同"_s);
        expectSearch(u"winshun://search?x=1&q=a&q=b"_s, u"a"_s); // the first q
        expectSearch(u"winshun://search?Q=a"_s, u"a"_s);
        expectSearch(u"winshun://search?query=a%26b"_s, u"a&b"_s);
        expectOpen(u"winshun://search?x=1"_s);
        expectOpen(u"winshun://search?q="_s);
        expectOpen(u"winshun://search?q=+"_s);
    }

    void searchWithNothingOpensTheBox()
    {
        expectOpen(u"winshun://search"_s);
        expectOpen(u"winshun://search/"_s);
        expectOpen(u"winshun://SEARCH/"_s);
        expectOpen(u"winshun://search/%20"_s);
        expectOpen(u"winshun://search/%0A"_s);
        expectOpen(u"winshun://search#x"_s);
    }

    void spacesInsideStay()
    {
        // "<folder>\ " is how a search inside a folder is typed: its space is wanted.
        expectSearch(u"winshun://search/D:\\Work\\%20"_s, u"D:\\Work\\ "_s);
        expectSearch(u"winshun://search/%20a"_s, u" a"_s);
    }

    void controlCharactersAreDropped()
    {
        expectSearch(u"winshun://search/a%00b"_s, u"ab"_s);
        expectSearch(u"winshun://search/a%0Ab"_s, u"a b"_s); // a line break is a space
        expectSearch(u"winshun://search/a%0D%0Ab"_s, u"a  b"_s);
        expectSearch(u"winshun://search/a%09b"_s, u"a b"_s);
        expectSearch(u"winshun://search/a%1Bb%7Fc"_s, u"abc"_s);
        expectSearch(u"winshun://search/a%C2%85b"_s, u"a b"_s); // U+0085
        expectSearch(u"winshun://search/a%C2%90b"_s, u"ab"_s); // U+0090
        expectSearch(u"winshun://search/a%E2%80%AEb"_s, u"ab"_s); // right-to-left override
        expectSearch(u"winshun://search/a%E2%80%A8b"_s, u"a b"_s); // line separator
        expectSearch(u"winshun://search?q=a%0Ab"_s, u"a b"_s);
        expectSearch(QStringLiteral("winshun://search/a\x01") + u"b", u"ab"_s); // raw too
    }

    void textIsCapped()
    {
        const QString many(5000, u'a');
        expectSearch(u"winshun://search/"_s + many, QString(kMaxTextChars, u'a'));
        expectSearch(u"winshun://search?q="_s + many, QString(kMaxTextChars, u'a'));
        // A pair is not cut in half: the 2,000th unit is the first half of one.
        const QString emoji = QString::fromUcs4(U"\U0001F600");
        const QString link = u"winshun://search/"_s + QString(kMaxTextChars - 1, u'a') + emoji + u"tail"_s;
        expectSearch(link, QString(kMaxTextChars - 1, u'a'));
        const QString fits = u"winshun://search/"_s + QString(kMaxTextChars - 2, u'a') + emoji + u"tail"_s;
        expectSearch(fits, QString(kMaxTextChars - 2, u'a') + emoji);
    }

    void settings()
    {
        expectSettings(u"winshun://settings"_s, {});
        expectSettings(u"winshun://settings/"_s, {});
        expectSettings(u"winshun://SETTINGS"_s, {});
        expectSettings(u"winshun://settings/clipboard"_s, u"clipboard"_s);
        expectSettings(u"winshun://settings/Clipboard/"_s, u"clipboard"_s);
        expectSettings(u"winshun://settings/files_and_dialogs"_s, u"files_and_dialogs"_s);
        expectSettings(u"winshun://settings/files-and-dialogs"_s, u"files_and_dialogs"_s);
        expectSettings(u"winshun://settings/clipboard?x=1"_s, u"clipboard"_s);
        expectSettings(u"winshun://settings/clipboard#top"_s, u"clipboard"_s);
        expectSettings(u"winshun://settings?page=clipboard"_s, {}); // only the path form
    }

    void settingsPageThatIsNoId()
    {
        expectSettings(u"winshun://settings/a/b"_s, {});
        expectSettings(u"winshun://settings/..%5C..%5Cx"_s, {});
        expectSettings(u"winshun://settings/%E5%89%AA%E8%B4%B4%E6%9D%BF"_s, {});
        expectSettings(u"winshun://settings/"_s + QString(kMaxPageChars + 1, u'a'), {});
        expectSettings(u"winshun://settings/"_s + QString(kMaxPageChars, u'a'), QString(kMaxPageChars, u'a'));
    }

    void anythingElseOpensTheBox()
    {
        expectOpen({});
        expectOpen(u"winshun://"_s);
        expectOpen(u"winshun:"_s);
        expectOpen(u"winshun://open"_s);
        expectOpen(u"winshun://searching/x"_s);
        expectOpen(u"winshun://run/calc.exe"_s);
        expectOpen(u"winshun://delete/C:/x"_s);
        expectOpen(u"winshunx://search/x"_s);
        expectOpen(u"https://search/x"_s);
        expectOpen(u"search/x"_s);
        expectOpen(u"--uri winshun://search/x"_s);
        expectOpen(u"xwinshun://search/x"_s);
    }
};

QTEST_APPLESS_MAIN(TestWinShunUri)
#include "tst_winshunuri.moc"
