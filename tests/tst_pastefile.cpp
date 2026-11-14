// 粘贴为文件 (core/PasteFile): which formats Ctrl+V takes from Explorer, what
// the clipboard's words or picture make, the CSV of spreadsheet cells, the
// names and how the text is written. The clipboard itself is never touched.
#include "ExplorerLanguage.h"
#include "PasteFile.h"

#include <QtTest>

#include <string>

using namespace ws::pastefile;

namespace {

QString q(const std::wstring& text)
{
    return QString::fromStdWString(text);
}

} // namespace

class TestPasteFile : public QObject {
    Q_OBJECT

private slots:
    void ctrlV();
    void chooses();
    void kinds();
    void json();
    void csv();
    void textFiles();
    void cleanNames();
    void baseNames();
    void titles();
    void fits();
    void utf8Bytes();
    void explorerLanguage();
};

void TestPasteFile::ctrlV()
{
    QVERIFY(takesCtrlV({.text = true}));
    QVERIFY(takesCtrlV({.image = true}));
    QVERIFY(takesCtrlV({.text = true, .image = true}));
    QVERIFY(!takesCtrlV({}));
    // Explorer pastes these itself: never taken from it.
    QVERIFY(!takesCtrlV({.files = true, .text = true}));
    QVERIFY(!takesCtrlV({.virtualFiles = true, .text = true, .image = true}));
    QVERIFY(!takesCtrlV({.files = true, .image = true}));
}

void TestPasteFile::chooses()
{
    QCOMPARE(choose({.text = L"你好"}), Kind::Text);
    QCOMPARE(choose({.text = L" \r\n\u3000"}), std::nullopt); // blank
    QCOMPARE(choose({}), std::nullopt);
    QCOMPARE(choose({.text = L"  \r\n", .image = true}), Kind::Picture);
    QCOMPARE(choose({.image = true}), Kind::Picture);
    // A browser's picture with its address alongside: the picture.
    QCOMPARE(choose({.text = L"https://example.com/a.png", .image = true}), Kind::Picture);
    QCOMPARE(choose({.text = L"data:image/png;base64,iVBOR", .image = true}), Kind::Picture);
    // Words with a picture of them (Word, PowerPoint): the words.
    QCOMPARE(choose({.text = L"第一段\r\n第二段", .image = true}), Kind::Text);
    QCOMPARE(choose({.text = L"see https://example.com", .image = true}), Kind::Text);
    // An address alone, no picture: a text file of it.
    QCOMPARE(choose({.text = L"https://example.com/"}), Kind::Text);
    // Excel's cells come with a picture of them: the table.
    QCOMPARE(choose({.text = L"a\tb\r\n1\t2\r\n", .spreadsheet = true, .image = true}), Kind::Table);
}

void TestPasteFile::kinds()
{
    QCOMPARE(textKind(L"Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software]", false), Kind::Registry);
    QCOMPARE(textKind(L"\r\nREGEDIT4\r\n", false), Kind::Registry);
    QCOMPARE(textKind(L"@echo off\r\nchcp 65001", false), Kind::Batch);
    QCOMPARE(textKind(L"\r\n  @ECHO OFF  \r\ndel *.tmp", false), Kind::Batch);
    QCOMPARE(textKind(L"@echo offline\r\n", false), Kind::Text);
    QCOMPARE(textKind(L"#!/usr/bin/env python3\nprint(1)", false), Kind::Python);
    QCOMPARE(textKind(L"#!/bin/bash\necho hi", false), Kind::Shell);
    QCOMPARE(textKind(L"#!/bin/sh\n", false), Kind::Shell);
    QCOMPARE(textKind(L"#!/usr/bin/env node\n", false), Kind::Text);
    QCOMPARE(textKind(L"<?xml version=\"1.0\"?><root/>", false), Kind::Xml);
    QCOMPARE(textKind(L"<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>", false), Kind::Svg);
    QCOMPARE(textKind(L"<svg viewBox=\"0 0 24 24\"><path d=\"M0 0\"/></svg>", false), Kind::Svg);
    QCOMPARE(textKind(L"<svgx>", false), Kind::Text);
    QCOMPARE(textKind(L"<!DOCTYPE html><html><body>x</body></html>", false), Kind::Web);
    QCOMPARE(textKind(L"<html lang=\"zh\">", false), Kind::Web);
    QCOMPARE(textKind(L"<div>a part of a page</div>", false), Kind::Text);
    QCOMPARE(textKind(L"{\"name\": \"winshun\", \"tags\": [1, 2.5e3, true, null]}", false), Kind::Json);
    QCOMPARE(textKind(L"[1, 2]", false), Kind::Json);
    QCOMPARE(textKind(L"{ name: 1 }", false), Kind::Text); // JavaScript, not JSON
    QCOMPARE(textKind(L"[1, 2,]", false), Kind::Text);
    QCOMPARE(textKind(L"会议纪要\r\n1. 讨论", false), Kind::Text);
    // Spreadsheet cells: more than one make a table, a lone cell is words.
    QCOMPARE(textKind(L"a\tb\r\n", true), Kind::Table);
    QCOMPARE(textKind(L"a\r\nb\r\n", true), Kind::Table);
    QCOMPARE(textKind(L"just one cell\r\n", true), Kind::Text);
    QCOMPARE(extension(Kind::Table), std::wstring_view(L".csv"));
    QCOMPARE(extension(Kind::Picture), std::wstring_view(L".png"));
    QCOMPARE(extension(Kind::Text), std::wstring_view(L".txt"));
}

void TestPasteFile::json()
{
    QVERIFY(isJson(L"{}"));
    QVERIFY(isJson(L" \r\n[ ]\r\n"));
    QVERIFY(isJson(L"{\"a\":{\"b\":[{\"c\":\"\\u4f60\\n\\\"\"}]}, \"d\": -0.5E-2}"));
    QVERIFY(isJson(L"\uFEFF{\"bom\": 1}"));
    QVERIFY(!isJson(L"{\"a\": 01}"));
    QVERIFY(!isJson(L"{\"a\": 1} x"));
    QVERIFY(!isJson(L"{\"a\" 1}"));
    QVERIFY(!isJson(L"{\"a\": \"line\nbreak\"}")); // a control character in a string
    QVERIFY(!isJson(L"{\"a\": \"\\x\"}"));
    QVERIFY(!isJson(L"{\"a\": \"\\u12\"}"));
    QVERIFY(!isJson(L"[1, 2"));
    QVERIFY(!isJson(L"\"just a string\"")); // not an object or array
    QVERIFY(!isJson(L"-"));
    // Deep nesting is refused, not a crash.
    QVERIFY(!isJson(std::wstring(100000, L'[') + std::wstring(100000, L']')));
    QVERIFY(isJson(std::wstring(500, L'[') + std::wstring(500, L']')));
}

void TestPasteFile::csv()
{
    QCOMPARE(q(csvFromCells(L"a\tb\r\n1\t2\r\n", L',')), u"a,b\r\n1,2\r\n");
    // A cell with a line break, as Excel copies it, stays one cell.
    QCOMPARE(q(csvFromCells(L"\"two\nlines\"\tx\r\n", L',')), u"\"two\nlines\",x\r\n");
    // Doubled quotes inside quotes; a quote in the middle as it stands.
    QCOMPARE(q(csvFromCells(L"\"\"\"hi\"\" there\"\tsay \"hi\"\r\n", L',')), u"\"\"\"hi\"\" there\",\"say \"\"hi\"\"\"\r\n");
    // The separator inside a cell; the user's own separator.
    QCOMPARE(q(csvFromCells(L"1,5\tx\r\n", L',')), u"\"1,5\",x\r\n");
    QCOMPARE(q(csvFromCells(L"1,5\tx\r\n", L';')), u"1,5;x\r\n");
    // Empty cells, at the end of a row too, and a last row without a line break.
    QCOMPARE(q(csvFromCells(L"a\t\tc\r\nd\t\r\ne", L',')), u"a,,c\r\nd,\r\ne\r\n");
    QCOMPARE(q(csvFromCells(L"a\t", L',')), u"a,\r\n");
    // A quote that does not end the cell: the cell as it stands.
    QCOMPARE(q(csvFromCells(L"\"abc\" def\tx\r\n", L',')), u"\"\"\"abc\"\" def\",x\r\n");
    QCOMPARE(q(csvFromCells(L"中文\t数据\r\n", L',')), u"中文,数据\r\n");
    QCOMPARE(q(csvFromCells(L"", L',')), u"");
}

void TestPasteFile::textFiles()
{
    TextFile reg = textFile(Kind::Registry, L"REGEDIT4\n[HKEY_CURRENT_USER\\X]\n");
    QCOMPARE(q(reg.text), u"REGEDIT4\r\n[HKEY_CURRENT_USER\\X]\r\n");
    QCOMPARE(reg.encoding, Encoding::Utf16Bom);
    TextFile bat = textFile(Kind::Batch, L"@echo off\n:loop\r\ngoto loop\r");
    QCOMPARE(q(bat.text), u"@echo off\r\n:loop\r\ngoto loop\r\n");
    QCOMPARE(bat.encoding, Encoding::OemOrUtf8);
    TextFile sh = textFile(Kind::Shell, L"#!/bin/sh\r\necho hi\r\n");
    QCOMPARE(q(sh.text), u"#!/bin/sh\necho hi\n");
    TextFile table = textFile(Kind::Table, L"a\tb\r\n", L';');
    QCOMPARE(q(table.text), u"a;b\r\n");
    QCOMPARE(table.encoding, Encoding::Utf8Bom);
    TextFile text = textFile(Kind::Text, L"as\ncopied\r\n");
    QCOMPARE(q(text.text), u"as\ncopied\r\n");
    QCOMPARE(text.encoding, Encoding::Utf8);
}

void TestPasteFile::cleanNames()
{
    QCOMPARE(q(cleanName(L"Q: what is a/b? <x> \"y\" *z* |w|")), u"Q what is a b x y z w");
    QCOMPARE(q(cleanName(L"  a\t\u3000b\u00A0 c  ")), u"a b c");
    QCOMPARE(q(cleanName(L"Wait for it...")), u"Wait for it");
    QCOMPARE(q(cleanName(L"\x01\x02")), u"");
    QCOMPARE(q(cleanName(L"???")), u"");
    // Cut whole: at a space near the end, else at the limit.
    QCOMPARE(q(cleanName(L"the quick brown fox jumps over the lazy dog", 20)), u"the quick brown fox");
    QCOMPARE(q(cleanName(L"一二三四五六七八九十一二三四五", 10)), u"一二三四五六七八九十");
    // A surrogate pair is one character, never split.
    const std::wstring emoji = L"ab\U0001F600\U0001F600cd";
    QCOMPARE(cleanName(emoji, 3).size(), std::size_t(4));
    QCOMPARE(cleanName(emoji, 4).size(), std::size_t(6));
    // Device names.
    QCOMPARE(q(cleanName(L"CON")), u"CON_");
    QCOMPARE(q(cleanName(L"nul.tar")), u"nul_.tar");
    QCOMPARE(q(cleanName(L"com1")), u"com1_");
    QCOMPARE(q(cleanName(L"COM10")), u"COM10");
    QCOMPARE(q(cleanName(L"Conference")), u"Conference");
}

void TestPasteFile::baseNames()
{
    const Words zh;
    const std::wstring when = stamp(2026, 10, 24, 15, 7, 3);
    QCOMPARE(q(when), u"2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Text, L"# 会议纪要 10/24\r\n- 第一项", zh, when)), u"会议纪要 10 24");
    QCOMPARE(q(baseName(Kind::Text, L"\r\n-----\r\n  > 引用的话\r\n", zh, when)), u"引用的话");
    QCOMPARE(q(baseName(Kind::Text, L"【】……\r\n——", zh, when)), u"文本 2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Text, L"**加粗的标题**", zh, when)), u"加粗的标题");
    QCOMPARE(q(baseName(Kind::Web, L"<html><title>A &amp; B &#x4f60;</title>", zh, when)), u"A & B 你");
    QCOMPARE(q(baseName(Kind::Web, L"<html><body>", zh, when)), u"网页 2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Table, L"a\tb", zh, when)), u"表格 2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Picture, {}, zh, when)), u"图片 2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Svg, L"<svg/>", zh, when)), u"图片 2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Json, L"{}", zh, when)), u"数据 2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Registry, L"REGEDIT4", zh, when)), u"注册表 2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Batch, L"@echo off", zh, when)), u"脚本 2026-10-24 150703");
    QCOMPARE(q(baseName(Kind::Picture, {}, englishWords(), when)), u"Image 2026-10-24 150703");
}

void TestPasteFile::titles()
{
    QCOMPARE(q(htmlTitle(L"<head><TITLE lang=\"en\">Hello</TITLE></head>")), u"Hello");
    QCOMPARE(q(htmlTitle(L"<titlebar>no</titlebar>")), u"");
    QCOMPARE(q(htmlTitle(L"<title>unclosed")), u"");
    QCOMPARE(q(htmlTitle(L"<title>&lt;b&gt; &quot;q&quot; &#20320;&nbsp;x &bogus;</title>")), u"<b> \"q\" 你 x &bogus;");
}

void TestPasteFile::fits()
{
    QCOMPARE(q(fitName(L"会议纪要", L".txt", 20)), u"会议纪要");
    // Folder 240 long: 259 - 240 - 1 - 4 - 5 = 9 left.
    QCOMPARE(q(fitName(L"abcdefghijklmnop", L".txt", 240)), u"abcdefghi");
    QCOMPARE(q(fitName(L"abcdefgh \U0001F600xyz", L".txt", 241)), u"abcdefgh"); // the pair is not cut in half, the space goes
    QCOMPARE(q(fitName(L"abc", L".txt", 300)), u"a"); // one character at least
}

void TestPasteFile::utf8Bytes()
{
    QCOMPARE(QByteArray::fromStdString(utf8(L"aé你\U0001F600")), QByteArray("a\xC3\xA9\xE4\xBD\xA0\xF0\x9F\x98\x80"));
    QCOMPARE(QByteArray::fromStdString(utf8(std::wstring(1, wchar_t(0xD800)) + L"x")), QByteArray("\xEF\xBF\xBDx"));
}

// What Explorer's verbs and the stub say follows Windows' language (GitHub #190).
void TestPasteFile::explorerLanguage()
{
    using namespace Qt::StringLiterals;
    namespace el = ws::explorerlang;
    QVERIFY(!el::isEnglish(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)));
    QVERIFY(!el::isEnglish(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)));
    QVERIFY(!el::isEnglish(0x0804)); // zh-CN
    QVERIFY(el::isEnglish(0x0409)); // en-US
    QVERIFY(el::isEnglish(0x0809)); // en-GB
    QVERIFY(el::isEnglish(0x0411)); // ja-JP
    QVERIFY(el::isEnglish(0x0407)); // de-DE
    QVERIFY(el::isEnglish(0));
    QVERIFY(el::isChinese(0x0804) && !el::isChinese(0x0409));
    QCOMPARE(q(std::wstring(el::searchHereTitle(true))), u"Search here with WinShun"_s);
    QCOMPARE(q(std::wstring(el::searchHereTitle(false))), u"用 Win顺 搜索此处"_s);
    QCOMPARE(q(std::wstring(el::pasteFileTitle(true))), u"Paste as file"_s);
    QCOMPARE(q(std::wstring(el::pasteFileTitle(false))), u"粘贴为文件"_s);
    QCOMPARE(q(std::wstring(el::commandsTitle(true))), u"WinShun commands"_s);
    QCOMPARE(q(std::wstring(el::commandsTitle(false))), u"Win顺 命令"_s);
    QCOMPARE(q(std::wstring(el::pinPictureTitle(true))), u"Pin to screen (WinShun)"_s);
    QCOMPARE(q(std::wstring(el::pinPictureTitle(false))), u"贴到屏幕（Win顺）"_s);
}

QTEST_APPLESS_MAIN(TestPasteFile)
#include "tst_pastefile.moc"
