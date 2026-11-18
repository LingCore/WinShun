// winshun.devtools (src/devtools-plugin): the JSON reader, its lenient and
// log-line readings, the writer, the tree the window shows and the rows the
// search box gets; and that its manifest is one Win顺 takes.
#include "Json.h"
#include "JsonTool.h"
#include "JsonTree.h"
#include "Launcher.h"
#include "Manifest.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace ws::devtools;

namespace {

// A backslash, built here: escapes typed into this file's literals are
// easy to lose on the way in.
QString bs() { return QString(QChar(0x5C)); }

const Node& childNamed(const Document& d, int parent, const QString& key)
{
    for (int c = d.firstChild(parent); c >= 0; c = d.nodes[std::size_t(c)].next) {
        if (d.nodes[std::size_t(c)].key == key)
            return d.nodes[std::size_t(c)];
    }
    static const Node none;
    return none;
}

std::shared_ptr<const Document> documentOf(const QString& text)
{
    Reading r = read(text);
    return std::make_shared<Document>(std::move(r.document));
}

QStringList shownKeys(const JsonTree& tree)
{
    QStringList keys;
    for (int row = 0; row < tree.rowCount(); ++row)
        keys << QString::number(tree.data(tree.index(row), JsonTree::DepthRole).toInt()) + u':'
                + tree.data(tree.index(row), JsonTree::KeyRole).toString();
    return keys;
}

} // namespace

class TestDevTools : public QObject {
    Q_OBJECT

private slots:
    void parsesInOrder()
    {
        const Reading r = read(uR"({"b": 1, "a": [true, false, null], "c": "x"})"_s);
        QVERIFY(!r.error);
        const Document& d = r.document;
        QCOMPARE(d.nodes[0].kind, Kind::Object);
        QCOMPARE(d.nodes[0].count, 3);
        const int b = d.nodes[0].first;
        QCOMPARE(d.nodes[std::size_t(b)].key, u"b"_s); // as written, not sorted
        QCOMPARE(d.nodes[std::size_t(d.nodes[std::size_t(b)].next)].key, u"a"_s);
        const Node& a = childNamed(d, 0, u"a"_s);
        QCOMPARE(a.kind, Kind::Array);
        QCOMPARE(a.count, 3);
        QCOMPARE(childNamed(d, 0, u"c"_s).text, u"x"_s);
        QVERIFY(r.notes.isEmpty());
    }

    void keepsNumbersExact()
    {
        const QString text = uR"({"id":12345678901234567890123,"f":1.10,"e":-0.5e-10,"z":0})"_s;
        const Reading r = read(text);
        QVERIFY(!r.error);
        QCOMPARE(write(r.document, 0, Layout::Compact), text);
        QCOMPARE(childNamed(r.document, 0, u"id"_s).text, u"12345678901234567890123"_s);
    }

    void decodesEscapes()
    {
        char32_t smile = 0x1F600;
        // "a\"b\\c\/d\n" and a surrogate pair written as two escapes.
        const QString text = u"[\"a"_s + bs() + u"\"b"_s + bs() + bs() + u"c"_s + bs() + u"/d"_s + bs() + u"n\", \""_s
            + bs() + u"ud83d"_s + bs() + u"ude00\"]"_s;
        const Reading r = read(text);
        QVERIFY2(!r.error, qPrintable(r.error ? r.error->message : QString()));
        const Document& d = r.document;
        const int first = d.nodes[0].first;
        QCOMPARE(d.nodes[std::size_t(first)].text, u"a\"b"_s + bs() + u"c/d\n"_s);
        QCOMPARE(d.nodes[std::size_t(d.nodes[std::size_t(first)].next)].text, QString::fromUcs4(&smile, 1));
        // Written back: quotes and backslashes escaped, the emoji as it is.
        QCOMPARE(write(d, 0, Layout::Compact),
            u"[\"a"_s + bs() + u"\"b"_s + bs() + bs() + u"c/d"_s + bs() + u"n\",\""_s + QString::fromUcs4(&smile, 1) + u"\"]"_s);
    }

    void errorsSayWhere_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("words");
        QTest::addColumn<int>("line");
        QTest::addColumn<int>("column");
        QTest::newRow("comma") << uR"({"a":1 "b":2})"_s << u"缺少逗号或 }"_s << 1 << 8;
        QTest::newRow("colon") << u"{\n  \"a\": 1,\n  \"b\" 2\n}"_s << u"冒号"_s << 3 << 7;
        QTest::newRow("unterminated") << u"{\"a\":\"abc"_s << u"字符串没有结束"_s << 1 << 6;
        QTest::newRow("word") << uR"({"a": hello})"_s << u"“hello”不是 JSON 的值"_s << 1 << 7;
        QTest::newRow("array") << u"[1, 2"_s << u"数组没有结束"_s << 1 << 6;
        // Broken pretty JSON is its error, not lines read one by one; the
        // missing comma at the end of the line it belongs to.
        QTest::newRow("pretty") << u"{\n  \"a\": 1\n  \"b\": 2\n}"_s << u"这一行末尾缺少逗号"_s << 2 << 9;
        QTest::newRow("pretty array") << u"[\n  1\n  2\n]"_s << u"这一行末尾缺少逗号"_s << 2 << 4;
        QTest::newRow("trailing") << u"12 abc"_s << u"多余的内容"_s << 1 << 4;
    }

    void errorsSayWhere()
    {
        QFETCH(QString, text);
        QFETCH(QString, words);
        QFETCH(int, line);
        QFETCH(int, column);
        const Reading r = read(text);
        QVERIFY(r.document.isEmpty());
        QVERIFY(r.error);
        QVERIFY2(r.error->message.contains(words), qPrintable(r.error->message));
        QCOMPARE(r.error->line, line);
        QCOMPARE(r.error->column, column);
    }

    void readsLeniently()
    {
        const Reading r = read(u"{a: 'x', // a comment\n b: [1, 2,], /* more */ c: True, d: None,}"_s);
        QVERIFY2(!r.error, qPrintable(r.error ? r.error->message : QString()));
        const unsigned want = Comments | SingleQuotes | BareKeys | TrailingCommas | PythonWords;
        QCOMPARE(r.document.relaxed & want, want);
        QCOMPARE(write(r.document, 0, Layout::Compact), uR"({"a":"x","b":[1,2],"c":true,"d":null})"_s);
        QVERIFY(r.notes.join(u' ').contains(u"按宽松写法读取"_s));
    }

    void readsWhatLogsHold()
    {
        // After a log line's time and level.
        Reading r = read(uR"(2026-11-18 10:00:01 INFO {"user":"x","id":3} done)"_s);
        QVERIFY(!r.error);
        QCOMPARE(write(r.document, 0, Layout::Compact), uR"({"user":"x","id":3})"_s);
        QVERIFY(r.notes.join(u' ').contains(u"跳过了前面"_s));
        QVERIFY(r.notes.join(u' ').contains(u"没有读"_s));
        // A time in brackets first: not an array that breaks.
        r = read(uR"([2026-11-18 10:00:01] WARN {"a":1})"_s);
        QVERIFY(!r.error);
        QCOMPARE(write(r.document, 0, Layout::Compact), uR"({"a":1})"_s);
        // JSON Lines.
        r = read(u"{\"a\":1}\n{\"a\":2}\n\n{\"a\":3}\n"_s);
        QVERIFY(!r.error);
        QCOMPARE(write(r.document, 0, Layout::Compact), uR"([{"a":1},{"a":2},{"a":3}])"_s);
        QVERIFY(r.notes.join(u' ').contains(u"每行一段 JSON"_s));
        // A log whose lines hold JSON, one that does not.
        r = read(u"10:00 INFO {\"a\":1}\n10:01 WARN {\"a\":2}\nplain words\n"_s);
        QVERIFY(!r.error);
        QCOMPARE(write(r.document, 0, Layout::Compact), uR"([{"a":1},{"a":2}])"_s);
        QVERIFY(r.notes.join(u' ').contains(u"日志"_s));
        QVERIFY(r.notes.join(u' ').contains(u"1 行不是 JSON"_s));
        // Escaped, as a log or a string literal has it.
        r = read(u"{"_s + bs() + u"\"a"_s + bs() + u"\":1,"_s + bs() + u"\"b"_s + bs() + u"\":"_s + bs() + u"\"x"_s + bs() + u"\"}"_s);
        QVERIFY(!r.error);
        QCOMPARE(write(r.document, 0, Layout::Compact), uR"({"a":1,"b":"x"})"_s);
        QVERIFY(r.notes.join(u' ').contains(u"去掉了转义"_s));
        // A JSON string holding JSON.
        r = read(u"\"{"_s + bs() + u"\"a"_s + bs() + u"\":[1]}\""_s);
        QVERIFY(!r.error);
        QCOMPARE(write(r.document, 0, Layout::Compact), uR"({"a":[1]})"_s);
        QVERIFY(r.notes.join(u' ').contains(u"去掉了外面的引号"_s));
        // Nothing, and not JSON.
        QVERIFY(read(u"  \n\t "_s).empty);
        QVERIFY(read(u"hello"_s).error);
        QVERIFY(!read(u"hello"_s).empty);
    }

    void opensJsonInStrings()
    {
        const QString text = u"{\"body\":\"{"_s + bs() + u"\"x"_s + bs() + u"\":[1,2]}\",\"n\":\"{not json}\",\"s\":\"[]\"}"_s;
        const Reading r = read(text);
        QVERIFY(!r.error);
        const Document& d = r.document;
        int body = -1;
        for (int c = d.nodes[0].first; c >= 0; c = d.nodes[std::size_t(c)].next) {
            if (d.nodes[std::size_t(c)].key == u"body")
                body = c;
        }
        QVERIFY(body >= 0);
        QVERIFY(d.nodes[std::size_t(body)].embedded);
        QCOMPARE(d.shape(body), Kind::Object);
        QVERIFY(d.expandable(body));
        QVERIFY(!childNamed(d, 0, u"n"_s).embedded);
        QVERIFY(!childNamed(d, 0, u"s"_s).embedded); // an empty one has nothing to show
        // Written back as the string it is.
        QCOMPARE(write(d, 0, Layout::Compact), text);
    }

    void writesForms()
    {
        const Reading r = read(uR"({"a":[1,{"b":null}],"e":{},"f":[]})"_s);
        QVERIFY(!r.error);
        QCOMPARE(write(r.document, 0, Layout::Pretty),
            u"{\n  \"a\": [\n    1,\n    {\n      \"b\": null\n    }\n  ],\n  \"e\": {},\n  \"f\": []\n}"_s);
        QCOMPARE(write(r.document, 0, Layout::Pretty, u"\t"_s).section(u'\n', 1, 1), u"\t\"a\": ["_s);
        QCOMPARE(write(r.document, 0, Layout::Pretty, u"    "_s).section(u'\n', 2, 2), u"        1,"_s);
        QCOMPARE(escaped(write(r.document, 0, Layout::Compact)),
            u"{"_s + bs() + u"\"a"_s + bs() + u"\":[1,{"_s + bs() + u"\"b"_s + bs() + u"\":null}],"_s + bs() + u"\"e"_s + bs()
                + u"\":{},"_s + bs() + u"\"f"_s + bs() + u"\":[]}"_s);
        // Pretty, read again, is the same document.
        const Reading again = read(write(r.document, 0, Layout::Pretty));
        QCOMPARE(write(again.document, 0, Layout::Compact), write(r.document, 0, Layout::Compact));
        QCOMPARE(describe(r.document, 0), u"对象 · 3 个键"_s);
    }

    void treeOpensAndWalks()
    {
        JsonTree tree;
        tree.setDocument(documentOf(uR"({"data":{"items":[{"id":1},{"id":2,"a b":true}]},"n":5})"_s));
        // Small: all of it open; the root's members at depth 0.
        QCOMPARE(shownKeys(tree), QStringList({u"0:data"_s, u"1:items"_s, u"2:0"_s, u"3:id"_s, u"2:1"_s, u"3:id"_s,
                                      u"3:a b"_s, u"0:n"_s}));
        QCOMPARE(tree.levels(), 4);
        QCOMPARE(tree.path(5), u"data.items[1].id"_s);
        QCOMPARE(tree.path(6), u"data.items[1][\"a b\"]"_s);
        QCOMPARE(tree.key(6), u"a b"_s);
        QCOMPARE(tree.value(5), u"2"_s);
        QCOMPARE(tree.json(4, false), uR"({"id":2,"a b":true})"_s);
        QCOMPARE(tree.parentRow(5), 4);
        QCOMPARE(tree.firstChildRow(4), 5);
        tree.toggle(1); // items closed
        QCOMPARE(shownKeys(tree), QStringList({u"0:data"_s, u"1:items"_s, u"0:n"_s}));
        QVERIFY(!tree.data(tree.index(1), JsonTree::ExpandedRole).toBool());
        QCOMPARE(tree.data(tree.index(1), JsonTree::CountRole).toInt(), 2);
        QCOMPARE(tree.data(tree.index(1), JsonTree::PreviewRole).toString(), u"[ {…}, {…} ]"_s);
        QCOMPARE(tree.firstChildRow(1), -1);
        tree.setSubtreeExpanded(1, true);
        QCOMPARE(tree.rowCount(), 8);
        tree.collapseAll();
        QCOMPARE(shownKeys(tree), QStringList({u"0:data"_s, u"0:n"_s}));
        tree.expandToLevel(2);
        QCOMPARE(shownKeys(tree), QStringList({u"0:data"_s, u"1:items"_s, u"0:n"_s}));
        tree.expandAll();
        QCOMPARE(tree.rowCount(), 8);
        // A lone value is its own row.
        tree.setDocument(documentOf(u"42"_s));
        QCOMPARE(tree.rowCount(), 1);
        QVERIFY(!tree.data(tree.index(0), JsonTree::HasKeyRole).toBool());
        QCOMPARE(tree.data(tree.index(0), JsonTree::ValueRole).toString(), u"42"_s);
    }

    void treeOpensWhatFits()
    {
        QStringList items;
        for (int i = 0; i < 2000; ++i)
            items << u"{\"id\":%1,\"name\":\"n%1\",\"tags\":[1,2]}"_s.arg(i);
        JsonTree tree;
        tree.setDocument(documentOf(u"["_s + items.join(u',') + u"]"_s));
        // 2000 elements already more than a first look: only they, closed.
        QCOMPARE(tree.rowCount(), 2000);
        QVERIFY(!tree.data(tree.index(0), JsonTree::ExpandedRole).toBool());
        QCOMPARE(tree.data(tree.index(0), JsonTree::PreviewRole).toString(), u"{ id: 0, name: \"n0\", tags: […] }"_s);
        // 300: they and their members (1200 rows), not the tags inside.
        tree.setDocument(documentOf(u"["_s + items.mid(0, 300).join(u',') + u"]"_s));
        QCOMPARE(tree.rowCount(), 1200);
        QVERIFY(tree.data(tree.index(0), JsonTree::ExpandedRole).toBool());
        QVERIFY(!tree.data(tree.index(3), JsonTree::ExpandedRole).toBool());
    }

    void treeSearches()
    {
        JsonTree tree;
        const QString body = u"\"{"_s + bs() + u"\"needle"_s + bs() + u"\":1}\""_s;
        tree.setDocument(documentOf(u"{\"a\":{\"b\":{\"c\":\"Needle here\"}},\"x\":"_s + body + u",\"y\":[\"no\",\"needle\"]}"_s));
        tree.collapseAll();
        QCOMPARE(tree.rowCount(), 3);
        QSignalSpy changed(&tree, &JsonTree::matchesChanged);
        // Case aside, in names and values, inside a string's own JSON too;
        // the first match is opened up to and returned.
        const int first = tree.search(u"needle"_s);
        QCOMPARE(tree.matchCount(), 3);
        QCOMPARE(tree.matchNumber(), 1);
        QVERIFY(changed.count() >= 1);
        QCOMPARE(tree.data(tree.index(first), JsonTree::KeyRole).toString(), u"c"_s);
        QCOMPARE(tree.data(tree.index(first), JsonTree::MatchRole).toInt(), 2);
        QVERIFY(tree.data(tree.index(first), JsonTree::CurrentRole).toBool());
        QCOMPARE(tree.currentMatchRow(), first);
        const int second = tree.step(1);
        QCOMPARE(tree.data(tree.index(second), JsonTree::KeyRole).toString(), u"needle"_s);
        QCOMPARE(tree.data(tree.index(second), JsonTree::MatchRole).toInt(), 1);
        QCOMPARE(tree.path(second), u"x.needle"_s);
        const int third = tree.step(1);
        QCOMPARE(tree.path(third), u"y[1]"_s);
        QCOMPARE(tree.matchNumber(), 3);
        QCOMPARE(tree.step(1), first); // round again
        QCOMPARE(tree.step(-1), third);
        QCOMPARE(tree.search(u"nothing"_s), -1);
        QCOMPARE(tree.matchCount(), 0);
        QCOMPARE(tree.currentMatchRow(), -1);
        // A new text is searched again for the same words.
        tree.search(u"needle"_s);
        tree.setDocument(documentOf(uR"({"needle":0})"_s));
        QCOMPARE(tree.matchCount(), 1);
        QCOMPARE(tree.currentMatchRow(), 0);
    }

    void toolReadsAndCopies()
    {
        JsonTool tool;
        QVERIFY(tool.empty());
        QVERIFY(!tool.valid());
        QSignalSpy read(&tool, &JsonTool::readChanged);
        tool.open(uR"({"a":[1,2]})"_s, u"x.json"_s);
        QVERIFY(tool.valid());
        QCOMPARE(tool.source(), u"x.json"_s);
        QCOMPARE(tool.summary(), u"对象 · 1 个键"_s);
        QCOMPARE(tool.tree()->rowCount(), 3);
        // Typing reads again after a pause.
        tool.edit(uR"({"a":[1,2)"_s);
        QVERIFY(tool.valid());
        QTRY_VERIFY(!tool.valid());
        QVERIFY(tool.error().contains(u"数组没有结束"_s));
        QCOMPARE(tool.errorLine(), 1);
        tool.open(uR"({"a":[1,2]})"_s);
        tool.reformat(true);
        QCOMPARE(tool.input(), u"{\n  \"a\": [\n    1,\n    2\n  ]\n}"_s);
        tool.setIndent(u"\t"_s);
        tool.reformat(true);
        QCOMPARE(tool.input(), u"{\n\t\"a\": [\n\t\t1,\n\t\t2\n\t]\n}"_s);
        tool.reformat(false);
        QCOMPARE(tool.input(), uR"({"a":[1,2]})"_s);
        // Not editable past what the box takes; a big text (past 1M
        // characters) let go on close.
        tool.open(QString(JsonTool::kMostEditable + 1, u' ') + u"[1]"_s);
        QVERIFY(!tool.editable());
        QVERIFY(tool.valid());
        tool.release();
        QVERIFY(tool.valid());
        tool.open(QString(1024 * 1024 + 1, u' ') + u"[1]"_s);
        tool.release();
        QVERIFY(tool.empty());
        tool.open(u"[1]"_s);
        tool.release();
        QVERIFY(tool.valid()); // a small one stays
    }

    void readsFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile utf8(dir.filePath(u"a.json"_s));
        QVERIFY(utf8.open(QIODevice::WriteOnly));
        utf8.write("\xEF\xBB\xBF{\"name\":\"\xE4\xB8\xAD\"}");
        utf8.close();
        QString why;
        QCOMPARE(readTextFile(utf8.fileName(), &why).value_or(QString()), u"{\"name\":\"中\"}"_s);
        QFile utf16(dir.filePath(u"b.json"_s));
        QVERIFY(utf16.open(QIODevice::WriteOnly));
        const QString text = u"[\"中\"]"_s;
        utf16.write("\xFF\xFE");
        utf16.write(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
        utf16.close();
        QCOMPARE(readTextFile(utf16.fileName(), &why).value_or(QString()), text);
        QVERIFY(!readTextFile(dir.filePath(u"missing.json"_s), &why));
        QVERIFY(why.contains(u"missing.json"_s));
    }

    void searchBoxRows()
    {
        const QString text = uR"({"a":[1,2],"b":"x"})"_s;
        QJsonArray out = rows(text, From::Clipboard, read(text), true, u"  "_s);
        QCOMPARE(out.size(), 4);
        QStringList ids;
        for (const QJsonValue& v : out)
            ids << v.toObject().value(u"id"_s).toString();
        QCOMPARE(ids, QStringList({u"view"_s, u"pretty"_s, u"compact"_s, u"escaped"_s}));
        const QJsonObject view = out[0].toObject();
        QVERIFY(view.value(u"subtitle"_s).toString().contains(u"剪贴板"_s));
        QVERIFY(view.value(u"subtitle"_s).toString().contains(u"对象 · 2 个键"_s));
        QVERIFY(view.value(u"details"_s).toBool());
        QCOMPARE(view.value(u"data"_s).toObject().value(u"do"_s).toString(), u"view"_s);
        QCOMPARE(out[2].toObject().value(u"copy"_s).toString(), text);
        QCOMPARE(out[1].toObject().value(u"copy"_s).toString(), copied(read(text), u"pretty"_s, u"  "_s));
        QCOMPARE(copied(read(text), u"escaped"_s, u"  "_s), escaped(text));
        // Pasted into the box: only the window's row, and nothing for what
        // only looks like JSON.
        out = rows(text, From::Pasted, read(text), false, u"  "_s);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].toObject().value(u"score"_s).toInt(), 90);
        QVERIFY(rows(u"{\"a\": oops"_s, From::Pasted, read(u"{\"a\": oops"_s), false, u"  "_s).isEmpty());
        // After the keyword: what is wrong, and where.
        out = rows(u"{\"a\":1 \"b\":2}"_s, From::Typed, read(u"{\"a\":1 \"b\":2}"_s), true, u"  "_s);
        QCOMPARE(out.size(), 1);
        QVERIFY(out[0].toObject().value(u"title"_s).toString().contains(u"缺少逗号或 }"_s));
        QVERIFY(out[0].toObject().value(u"subtitle"_s).toString().contains(u"第 1 行第 8 列"_s));
        out = rows(u"hello"_s, From::Clipboard, read(u"hello"_s), true, u"  "_s);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].toObject().value(u"title"_s).toString(), u"剪贴板里不是 JSON"_s);
        out = rows(QString(), From::Clipboard, read(QString()), true, u"  "_s);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].toObject().value(u"id"_s).toString(), u"view"_s);
    }

    void manifestIsTaken()
    {
        QFile file(QString::fromUtf8(WINSHUN_DEVTOOLS_MANIFEST));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QString error;
        const std::optional<ws::plugin::Manifest> manifest = ws::plugin::Manifest::parse(bytes, &error);
        QVERIFY2(manifest, qPrintable(error));
        QCOMPARE(manifest->id, u"winshun.devtools"_s);
        QVERIFY(manifest->resident());
        // The regex trigger, as Win顺 compiles it: JSON pasted in, a log
        // line with JSON, escaped JSON; not `json …` (the keyword's) nor
        // words with a bracket.
        const QString pattern = QJsonDocument::fromJson(bytes)
                                    .object()
                                    .value(u"commands"_s)
                                    .toArray()
                                    .first()
                                    .toObject()
                                    .value(u"triggers"_s)
                                    .toArray()
                                    .first()
                                    .toObject()
                                    .value(u"regex"_s)
                                    .toString();
        const QRegularExpression regex(pattern);
        QVERIFY2(regex.isValid(), qPrintable(regex.errorString()));
        const QStringList yes {uR"({"a":1})"_s, uR"([{"a":1}])"_s, uR"(["a","b"])"_s,
            uR"(2026-11-18 10:00:01 INFO {"user":"x"})"_s, u"{"_s + bs() + u"\"a"_s + bs() + u"\":1}"_s};
        const QStringList no {uR"(json {"a":1})"_s, uR"(JSON {"a":1})"_s, u"a[1]"_s, u"hello world"_s, u"[1,2,3]"_s};
        for (const QString& t : yes)
            QVERIFY2(regex.match(t).hasMatch(), qPrintable(t));
        for (const QString& t : no)
            QVERIFY2(!regex.match(t).hasMatch(), qPrintable(t));
    }
};

QTEST_MAIN(TestDevTools)
#include "tst_devtools.moc"
