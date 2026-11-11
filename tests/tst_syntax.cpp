#include "SyntaxSpans.h"

#include <QTest>

using namespace ws;
using namespace ws::syntax;
using namespace Qt::StringLiterals;

namespace {

// "kind:text" of every span, for reading a line's colours at a glance.
QStringList told(Language language, const QString& line, State& state)
{
    static const char* const names[] = {"comment", "string", "number", "keyword", "type", "key", "tag", "h1", "h2", "h3",
        "strong", "em", "code", "link", "quote", "mark"};
    QStringList out;
    int end = 0;
    for (const Span& span : highlight(language, line, state)) {
        if (span.start < end || span.length <= 0 || span.start + span.length > line.size())
            return {u"BAD SPAN %1+%2"_s.arg(span.start).arg(span.length)}; // in order, apart, inside the line
        end = span.start + span.length;
        out << QLatin1StringView(names[static_cast<int>(span.kind)]) + u':' + line.mid(span.start, span.length);
    }
    return out;
}

QStringList told(Language language, const QString& line)
{
    State state;
    return told(language, line, state);
}

} // namespace

// The preview's colours (GitHub #291).
class SyntaxTest : public QObject {
    Q_OBJECT

private slots:
    void languages()
    {
        QCOMPARE(languageFor(u"main.CPP"), Language::CFamily);
        QCOMPARE(languageFor(u"C:\\src\\app.tsx"), Language::CFamily);
        QCOMPARE(languageFor(u"run.py"), Language::Python);
        QCOMPARE(languageFor(u"build.ps1"), Language::Shell);
        QCOMPARE(languageFor(u"Dockerfile"), Language::Shell);
        QCOMPARE(languageFor(u"CMakeLists.txt"), Language::Shell);
        QCOMPARE(languageFor(u"a.bat"), Language::Batch);
        QCOMPARE(languageFor(u"package.json"), Language::Json);
        QCOMPARE(languageFor(u"ci.yml"), Language::Yaml);
        QCOMPARE(languageFor(u"WinShun.ini"), Language::Ini);
        QCOMPARE(languageFor(u"Cargo.toml"), Language::Ini);
        QCOMPARE(languageFor(u"index.html"), Language::Xml);
        QCOMPARE(languageFor(u"README.md"), Language::Markdown);
        QCOMPARE(languageFor(u"notes.txt"), Language::None);
        QCOMPARE(languageFor(u"noextension"), Language::None);
        QCOMPARE(languageFor(u""), Language::None);
        QVERIFY(told(Language::None, u"int x = 1; // plain"_s).isEmpty());
    }

    void code()
    {
        QCOMPARE(told(Language::CFamily, u"const int count = 0x1F; // how many"_s),
            (QStringList {u"keyword:const"_s, u"type:int"_s, u"number:0x1F"_s, u"comment:// how many"_s}));
        QCOMPARE(told(Language::CFamily, u"return name == \"a \\\" b\" ? 'x' : nullptr;"_s),
            (QStringList {u"keyword:return"_s, u"string:\"a \\\" b\""_s, u"string:'x'"_s, u"type:nullptr"_s}));
        // A name with digits or a keyword inside it stays plain; so does a mark inside a string.
        QCOMPARE(told(Language::CFamily, u"int32x format1 = \"// no comment\";"_s), (QStringList {u"string:\"// no comment\""_s}));
        // A block comment over lines, code after its end.
        State state;
        QCOMPARE(told(Language::CFamily, u"int a; /* one"_s, state), (QStringList {u"type:int"_s, u"comment:/* one"_s}));
        QCOMPARE(state.mode, State::BlockComment);
        QCOMPARE(told(Language::CFamily, u"two"_s, state), (QStringList {u"comment:two"_s}));
        QCOMPARE(told(Language::CFamily, u"three */ return;"_s, state), (QStringList {u"comment:three */"_s, u"keyword:return"_s}));
        QCOMPARE(state, State {});
        // A string left open ends with its line; a template string goes on.
        QCOMPARE(told(Language::CFamily, u"x = \"open"_s, state), (QStringList {u"string:\"open"_s}));
        QCOMPARE(state, State {});
        QCOMPARE(told(Language::CFamily, u"const s = `a"_s, state), (QStringList {u"keyword:const"_s, u"string:`a"_s}));
        QCOMPARE(told(Language::CFamily, u"b` + 1"_s, state), (QStringList {u"string:b`"_s, u"number:1"_s}));

        QCOMPARE(told(Language::Python, u"def f(x): return None  # why"_s),
            (QStringList {u"keyword:def"_s, u"keyword:return"_s, u"type:None"_s, u"comment:# why"_s}));
        State py;
        QCOMPARE(told(Language::Python, u"\"\"\"Docs"_s, py), (QStringList {u"string:\"\"\"Docs"_s}));
        QCOMPARE(told(Language::Python, u"more\"\"\" + 'x'"_s, py), (QStringList {u"string:more\"\"\""_s, u"string:'x'"_s}));

        QCOMPARE(told(Language::Shell, u"if [ -f \"$1\" ]; then # test"_s),
            (QStringList {u"keyword:if"_s, u"string:\"$1\""_s, u"keyword:then"_s, u"comment:# test"_s}));
        QCOMPARE(told(Language::Batch, u"REM a note"_s), (QStringList {u"comment:REM a note"_s}));
        QCOMPARE(told(Language::Batch, u"IF EXIST x.txt echo REM"_s), (QStringList {u"keyword:IF"_s, u"keyword:EXIST"_s, u"keyword:echo"_s}));
        QCOMPARE(told(Language::Sql, u"SELECT name FROM t WHERE id = 3 -- one"_s),
            (QStringList {u"keyword:SELECT"_s, u"keyword:FROM"_s, u"keyword:WHERE"_s, u"number:3"_s, u"comment:-- one"_s}));
        QCOMPARE(told(Language::Lua, u"local x = nil -- none"_s), (QStringList {u"keyword:local"_s, u"type:nil"_s, u"comment:-- none"_s}));
    }

    void data()
    {
        QCOMPARE(told(Language::Json, u"  \"name\": \"Win顺\", \"size\": 12.5, \"on\": true"_s),
            (QStringList {u"key:\"name\""_s, u"string:\"Win顺\""_s, u"key:\"size\""_s, u"number:12.5"_s, u"key:\"on\""_s, u"type:true"_s}));
        QCOMPARE(told(Language::Yaml, u"  - name: build # the job"_s), (QStringList {u"keyword:-"_s, u"key:name"_s, u"comment:# the job"_s}));
        QCOMPARE(told(Language::Yaml, u"enabled: true"_s), (QStringList {u"key:enabled"_s, u"type:true"_s}));
        QCOMPARE(told(Language::Yaml, u"url: \"http://a#b\""_s), (QStringList {u"key:url"_s, u"string:\"http://a#b\""_s}));
        QCOMPARE(told(Language::Yaml, u"---"_s), (QStringList {u"mark:---"_s}));
        QCOMPARE(told(Language::Ini, u"[Launcher]"_s), (QStringList {u"tag:[Launcher]"_s}));
        QCOMPARE(told(Language::Ini, u"Hotkey = \"Alt+Space\""_s), (QStringList {u"key:Hotkey"_s, u"string:\"Alt+Space\""_s}));
        QCOMPARE(told(Language::Ini, u"Index=false"_s), (QStringList {u"key:Index"_s, u"type:false"_s}));
        QCOMPARE(told(Language::Ini, u"; a note = not a key"_s), (QStringList {u"comment:; a note = not a key"_s}));
        QCOMPARE(told(Language::Css, u"  font-size: 12px; /* small */"_s), (QStringList {u"key:font-size"_s, u"number:12px"_s, u"comment:/* small */"_s}));

        QCOMPARE(told(Language::Xml, u"<a href=\"x\" class='y'>text</a>"_s),
            (QStringList {u"tag:a"_s, u"key:href"_s, u"string:\"x\""_s, u"key:class"_s, u"string:'y'"_s, u"tag:a"_s}));
        State xml;
        QCOMPARE(told(Language::Xml, u"<div <!-- no --> id=\"1\""_s, xml).first(), u"tag:div"_s);
        State comment;
        QCOMPARE(told(Language::Xml, u"<p/> <!-- one"_s, comment), (QStringList {u"tag:p"_s, u"comment:<!-- one"_s}));
        QCOMPARE(told(Language::Xml, u"two --> <b>"_s, comment), (QStringList {u"comment:two -->"_s, u"tag:b"_s}));
        // A tag over lines: the attributes on the next line are still its own.
        State tag;
        QCOMPARE(told(Language::Xml, u"<Rectangle"_s, tag), (QStringList {u"tag:Rectangle"_s}));
        QCOMPARE(tag.mode, State::InTag);
        QCOMPARE(told(Language::Xml, u"    width=\"3\"> plain words"_s, tag), (QStringList {u"key:width"_s, u"string:\"3\""_s}));
        QCOMPARE(tag, State {});
    }

    void markdown()
    {
        QCOMPARE(told(Language::Markdown, u"# 标题"_s), (QStringList {u"mark:#"_s, u"h1:标题"_s}));
        QCOMPARE(told(Language::Markdown, u"## 第二章 北极元晶"_s), (QStringList {u"mark:##"_s, u"h2:第二章 北极元晶"_s}));
        QCOMPARE(told(Language::Markdown, u"#### deep"_s), (QStringList {u"mark:####"_s, u"h3:deep"_s}));
        QVERIFY(told(Language::Markdown, u"#hashtag is no heading"_s).isEmpty());
        QCOMPARE(told(Language::Markdown, u"some **bold** and *soft* and `code`"_s),
            (QStringList {u"mark:**"_s, u"strong:bold"_s, u"mark:**"_s, u"mark:*"_s, u"em:soft"_s, u"mark:*"_s, u"code:`code`"_s}));
        QCOMPARE(told(Language::Markdown, u"![logo](../Images/logo-5.png) and [site](https://a.b)"_s),
            (QStringList {u"mark:!["_s, u"link:logo"_s, u"mark:](../Images/logo-5.png)"_s, u"mark:["_s, u"link:site"_s, u"mark:](https://a.b)"_s}));
        QCOMPARE(told(Language::Markdown, u"see https://example.com/x now"_s), (QStringList {u"link:https://example.com/x"_s}));
        QCOMPARE(told(Language::Markdown, u"- [x] done"_s), (QStringList {u"keyword:-"_s, u"keyword:[x]"_s}));
        QCOMPARE(told(Language::Markdown, u"12. twelfth"_s), (QStringList {u"keyword:12."_s}));
        QCOMPARE(told(Language::Markdown, u"> quoted"_s), (QStringList {u"mark:>"_s, u"quote: quoted"_s}));
        QCOMPARE(told(Language::Markdown, u"| --- | :-: |"_s), (QStringList {u"mark:| --- | :-: |"_s}));
        // Ordinary text with marks that open nothing: plain.
        QVERIFY(told(Language::Markdown, u"2 * 3 = 6, snake_case_name, a_b"_s).isEmpty());
        QVERIFY(told(Language::Markdown, u"半年后，天南越国镜州某处。"_s).isEmpty());
        // A fenced block: code until its end, whatever is in it.
        State state;
        QCOMPARE(told(Language::Markdown, u"```cpp"_s, state), (QStringList {u"mark:```cpp"_s}));
        QCOMPARE(told(Language::Markdown, u"# not a heading"_s, state), (QStringList {u"code:# not a heading"_s}));
        QCOMPARE(told(Language::Markdown, u"```"_s, state), (QStringList {u"mark:```"_s}));
        QCOMPARE(told(Language::Markdown, u"# heading again"_s, state), (QStringList {u"mark:#"_s, u"h1:heading again"_s}));
    }

    void words()
    {
        const auto at = [](const QString& line, const QStringList& words) {
            QStringList out;
            for (const Found& f : findWords(line, words))
                out << line.mid(f.start, f.length);
            return out;
        };
        QCOMPARE(at(u"Chapter one, chapter two"_s, {u"chapter"_s}), (QStringList {u"Chapter"_s, u"chapter"_s}));
        QCOMPARE(at(u"报销单和报告"_s, {u"报销"_s, u"单"_s}), (QStringList {u"报销单"_s})); // touching: one
        QCOMPARE(at(u"abcabc"_s, {u"abc"_s, u"bca"_s}), (QStringList {u"abcabc"_s})); // overlapping: one
        QVERIFY(at(u"a b c 1"_s, {u"a"_s, u"1"_s, QString()}).isEmpty()); // a letter or a digit alone says nothing
        QVERIFY(at(u"nothing here"_s, {u"chapter"_s}).isEmpty());
        QVERIFY(at(QString(), {u"x y"_s}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(SyntaxTest)
#include "tst_syntax.moc"
