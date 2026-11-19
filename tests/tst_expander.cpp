// The text expander plugin (src/expander-plugin, GitHub #403) without its
// hooks: the rules (Expander) -- the abbreviations its setting lists (a
// phrase a line, the lines after it with no "=", notes, the lines not used
// and why), the one typed text ends with (as written, Capitalized or in
// CAPITALS, the longest, not inside a word), what a Backspace counts as and
// the programs left alone.

#include "Expander.h"

#include <QTest>

using namespace Qt::StringLiterals;

using namespace expander;

namespace {

QString q(const std::wstring& s) { return QString::fromStdWString(s); }

const Phrase* phraseOf(const Phrases& p, std::wstring_view abbreviation)
{
    for (const Phrase& phrase : p.list) {
        if (phrase.abbreviation == abbreviation)
            return &phrase;
    }
    return nullptr;
}

// What the text typed so far turns into, or "" for nothing.
QString expanded(const std::vector<Phrase>& phrases, std::wstring_view typed)
{
    const std::optional<Match> m = find(phrases, typed);
    return m ? q(m->text) : QString();
}

} // namespace

class ExpanderTest : public QObject {
    Q_OBJECT

private slots:
    void aPhraseALine()
    {
        const Phrases p = parse(L"cmd = command\r\n  btw=by the way  \n\n# a note = not a phrase\n;addr ＝ 北京市海淀区\n@@ = me@example.com = mine");
        QCOMPARE(int(p.list.size()), 4);
        QVERIFY(p.problems.empty());
        QCOMPARE(q(p.list[0].abbreviation), u"cmd"_s);
        QCOMPARE(q(p.list[0].text), u"command"_s);
        QCOMPARE(p.list[0].line, 1);
        QCOMPARE(q(phraseOf(p, L"btw")->text), u"by the way"_s); // spaces around both sides left out
        QCOMPARE(q(phraseOf(p, L";addr")->text), u"北京市海淀区"_s); // the full-width ＝
        QCOMPARE(phraseOf(p, L";addr")->line, 5);
        QCOMPARE(q(phraseOf(p, L"@@")->text), u"me@example.com = mine"_s); // split at the first
        QVERIFY(!phraseOf(p, L"# a note"));
    }

    void linesWithoutEqualsGoOnTheOneAbove()
    {
        const Phrases p = parse(L"sig = Best regards,\nTom Smith\n\n    Room 5  \n\n\nnext = one\nblock =\n\n  first\n  second\n\n");
        QVERIFY(p.problems.empty());
        // Its lines as written but for the spaces at their ends; the blank
        // lines between them kept, the ones after the last not.
        QCOMPARE(q(phraseOf(p, L"sig")->text), u"Best regards,\nTom Smith\n\n    Room 5"_s);
        QCOMPARE(q(phraseOf(p, L"next")->text), u"one"_s);
        // "abbreviation =" alone: the lines under it, from the first that has words.
        QCOMPARE(q(phraseOf(p, L"block")->text), u"  first\n  second"_s);
    }

    void linesNotUsedSayWhy()
    {
        const Phrases p = parse(L"lost line\n= no abbreviation\nempty =\nc md = spaced\n"
                                L"abcdefghijklmnopqrstuvwxyz0123456789 = long\n中文 = 打不出来\n"
                                L"ok = first\nok = second\nOK = capitals are another one\nfull＃ = 全角");
        using Kind = Problem::Kind;
        const std::vector<std::pair<Kind, int>> expected = {
            {Kind::NoEquals, 1}, {Kind::NoAbbreviation, 2}, {Kind::NoText, 3}, {Kind::Spaces, 4},
            {Kind::TooLong, 5}, {Kind::NotTyped, 6}, {Kind::Repeated, 8}, {Kind::NotTyped, 10},
        };
        QCOMPARE(int(p.problems.size()), int(expected.size()));
        for (std::size_t i = 0; i < expected.size(); ++i) {
            QCOMPARE(int(p.problems[i].kind), int(expected[i].first));
            QCOMPARE(p.problems[i].line, expected[i].second);
        }
        QCOMPARE(q(p.problems[3].abbreviation), u"c md"_s);
        QCOMPARE(q(phraseOf(p, L"ok")->text), u"first"_s); // the first one counts
        QCOMPARE(q(phraseOf(p, L"OK")->text), u"capitals are another one"_s);
        QCOMPARE(int(p.list.size()), 2);
        // A line not used takes its own next lines with it: not a phrase's.
        const Phrases q2 = parse(L"c md = one\ntwo\nok = three");
        QCOMPARE(int(q2.problems.size()), 1);
        QCOMPARE(q(phraseOf(q2, L"ok")->text), u"three"_s);
    }

    void theAbbreviationTypedEndsWith()
    {
        const std::vector<Phrase> phrases = parse(L"cmd = command\nbtw = by the way\n;sig = Best regards\nsig = signature\ne.g = for example").list;
        QCOMPARE(expanded(phrases, L"run cmd"), u"command"_s);
        QCOMPARE(expanded(phrases, L"cmd"), u"command"_s); // nothing before it
        QCOMPARE(expanded(phrases, L"(cmd"), u"command"_s); // punctuation before it
        QCOMPARE(expanded(phrases, L"xcmd"), QString()); // inside a word
        QCOMPARE(expanded(phrases, L"_cmd"), QString());
        QCOMPARE(expanded(phrases, L"2cmd"), QString());
        QCOMPARE(expanded(phrases, L"écmd"), QString()); // a letter of any script
        QCOMPARE(expanded(phrases, L"cm"), QString());
        QCOMPARE(expanded(phrases, L"cmd "), QString()); // the end key is not part of it
        // One that starts with a sign counts anywhere; the longest wins.
        QCOMPARE(expanded(phrases, L"word;sig"), u"Best regards"_s);
        QCOMPARE(expanded(phrases, L" sig"), u"signature"_s);
        QCOMPARE(expanded(phrases, L"see e.g"), u"for example"_s);
        const std::optional<Match> m = find(phrases, L"see e.g");
        QCOMPARE(int(m->typedLength), 3);
        QCOMPARE(q(m->phrase->abbreviation), u"e.g"_s);
    }

    void theCaseItIsTypedIn()
    {
        const std::vector<Phrase> phrases = parse(L"btw = by the way\nms = millisecond\nMS = Microsoft\nOK = okay\na1 = (first) answer\nx = ex").list;
        QCOMPARE(expanded(phrases, L"btw"), u"by the way"_s);
        QCOMPARE(expanded(phrases, L"Btw"), u"By the way"_s);
        QCOMPARE(expanded(phrases, L"BTW"), u"BY THE WAY"_s);
        QCOMPARE(expanded(phrases, L"bTw"), QString()); // other mixtures: not it
        QCOMPARE(expanded(phrases, L"BTw"), QString());
        // Written as typed comes first.
        QCOMPARE(expanded(phrases, L"MS"), u"Microsoft"_s);
        QCOMPARE(expanded(phrases, L"ms"), u"millisecond"_s);
        QCOMPARE(expanded(phrases, L"Ms"), u"Millisecond"_s);
        // Written in capitals: only so.
        QCOMPARE(expanded(phrases, L"OK"), u"okay"_s);
        QCOMPARE(expanded(phrases, L"ok"), QString());
        QCOMPARE(expanded(phrases, L"Ok"), QString());
        // One letter: a capital is the first letter's, the text's first letter follows.
        QCOMPARE(expanded(phrases, L"A1"), u"(First) answer"_s);
        QCOMPARE(expanded(phrases, L"X"), u"Ex"_s);
    }

    void backspacesCountCodePoints()
    {
        QCOMPARE(int(codePoints(L"command")), 7);
        QCOMPARE(int(codePoints(L"北京")), 2);
        QCOMPARE(int(codePoints(L"a\U0001F600b")), 3); // an emoji is one, two units
        QCOMPARE(int(codePoints(L"")), 0);
    }

    void programsLeftAlone()
    {
        const std::vector<std::wstring> list = programList(L" Code.exe; \"C:\\Program Files\\x\\mstsc.exe\"；notepad ,, KeePass.EXE\nnotepad.exe");
        QCOMPARE(int(list.size()), 4);
        QCOMPARE(q(list[0]), u"code.exe"_s);
        QCOMPARE(q(list[1]), u"mstsc.exe"_s);
        QCOMPARE(q(list[2]), u"notepad.exe"_s); // .exe added, then once
        QCOMPARE(q(list[3]), u"keepass.exe"_s);
        QVERIFY(listed(list, L"C:\\Users\\me\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe"));
        QVERIFY(listed(list, L"KEEPASS.EXE"));
        QVERIFY(!listed(list, L"C:\\Windows\\explorer.exe"));
        QVERIFY(!listed(list, L""));
        QVERIFY(programList(L"  ;  ").empty());
    }
};

QTEST_GUILESS_MAIN(ExpanderTest)
#include "tst_expander.moc"
