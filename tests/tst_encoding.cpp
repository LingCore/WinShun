// Mojibake (乱码) guards over the source tree: text stays UTF-8 from file to
// screen, and the few places that do meet the ANSI or OEM code page say why.
// Run on demand like any test: ctest -R ^tst_encoding$

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QtTest>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

const QString kRoot = QStringLiteral(WS_SOURCE_DIR);

// Folders kept as they are: other people's code, build output, fixtures
// that are not UTF-8 on purpose (the content search's GBK/UTF-16 samples).
const QStringList kSkippedFolders {u"third_party"_s, u"build"_s, u"dist"_s, u".git"_s, u".worktrees"_s,
    u"node_modules"_s, u"tools/screenshots/demo"_s};

const QSet<QString> kTextSuffixes {u"cpp"_s, u"h"_s, u"hpp"_s, u"c"_s, u"qml"_s, u"js"_s, u"mjs"_s, u"ts"_s,
    u"md"_s, u"txt"_s, u"json"_s, u"ps1"_s, u"psm1"_s, u"psd1"_s, u"iss"_s, u"isl"_s, u"rc"_s, u"cmake"_s, u"py"_s,
    u"bat"_s, u"cmd"_s, u"yml"_s, u"yaml"_s, u"xml"_s, u"html"_s, u"css"_s, u"svg"_s, u"ini"_s, u"toml"_s, u"in"_s,
    u"qrc"_s, u"manifest"_s, u"def"_s};

// The source files that meet the ANSI or OEM code page, and why. A new one
// belongs here only with a reason; one no longer doing so leaves the list.
const QHash<QString, QString> kCodePageUsers {
    {u"src/app/ClipFiles.cpp"_s, u"a .bat is written in the OEM code page cmd reads"_s},
    {u"src/stub/PasteAsFile.cpp"_s, u"a .bat is written in the OEM code page cmd reads"_s},
    {u"src/capture/RecordingWorker.cpp"_s, u"a recording worker's stderr that is not UTF-8: Qt's own log, in the ANSI code page"_s},
    {u"src/app/DisplayDiagnostics.cpp"_s, u"the diagnostics report which code page the PC has"_s},
    {u"src/app/UpdatePackage.cpp"_s, u"zip names without the UTF-8 flag are in the OEM code page"_s},
    {u"src/plugin/Install.cpp"_s, u"zip names without the UTF-8 flag are in the OEM code page"_s},
    {u"src/archive-plugin/Lister.cpp"_s, u"names in an archive that are not UTF-8 are in the code page of the PC that packed it"_s},
    {u"src/plugin/runner/main.cpp"_s, u"a plugin's stderr in the code page its manifest names"_s},
    {u"src/cli/Encoding.cpp"_s, u"ws writes to a console or a pipe in the encoding it expects"_s},
    {u"src/core/ClipMisread.h"_s, u"tells a remote-desktop sync's ANSI garbling apart (#181)"_s},
    {u"src/core/Mojibake.h"_s, u"undoes UTF-8 read in the ANSI code page (#346)"_s},
    {u"src/core/ContentScanner.cpp"_s, u"text files without a BOM that are not UTF-8 are in the legacy code page"_s},
    {u"src/extract/Extract.cpp"_s, u"text files without a BOM that are not UTF-8 are in the legacy code page"_s},
    {u"src/core/EverythingIpc.cpp"_s, u"Everything's IPC has an ANSI variant that clients still use"_s},
};

QString relative(const QString& path)
{
    return path.mid(kRoot.size() + 1);
}

bool skipped(const QString& rel)
{
    const QString name = rel.section(u'/', -1);
    return std::ranges::any_of(kSkippedFolders, [&](const QString& folder) {
        return folder.contains(u'/') ? rel == folder : name == folder;
    });
}

// The project's text files: by suffix, CMakeLists.txt included. Skipped
// folders are not entered at all (a build folder holds many thousands).
QStringList textFiles()
{
    static const QStringList files = [] {
        QStringList found;
        QStringList folders {kRoot};
        while (!folders.isEmpty()) {
            const QDir folder(folders.takeLast());
            for (const QFileInfo& entry :
                folder.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
                if (skipped(relative(entry.filePath())))
                    continue;
                if (entry.isDir() && !entry.isSymLink())
                    folders.append(entry.filePath());
                else if (entry.isFile() && kTextSuffixes.contains(entry.suffix().toLower()))
                    found.append(entry.filePath());
            }
        }
        std::ranges::sort(found);
        return found;
    }();
    return files;
}

QByteArray bytesOf(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool ascii(const QByteArray& bytes)
{
    return std::ranges::all_of(bytes, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// Code without comments and string literals: a pattern named in a comment
// or a message is not a call.
QString codeOf(const QString& line)
{
    QString code;
    bool inString = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (inString) {
            if (c == u'\\')
                ++i;
            else if (c == u'"')
                inString = false;
            continue;
        }
        if (c == u'"') {
            inString = true;
            continue;
        }
        if (c == u'/' && i + 1 < line.size() && (line[i + 1] == u'/' || line[i + 1] == u'*'))
            break;
        code += c;
    }
    const QString trimmed = code.trimmed();
    return trimmed.startsWith(u'*') ? QString() : code; // inside a block comment
}

QString report(const QStringList& problems)
{
    return u"\n  "_s + problems.join(u"\n  "_s);
}

} // namespace

class EncodingTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY2(QFile::exists(kRoot + u"/CMakeLists.txt"_s), qPrintable(kRoot));
    }

    // Every text file is UTF-8: what an editor, the compiler, Qt's
    // resources and the translation tools all read it as.
    void textIsUtf8()
    {
        QStringList problems;
        int count = 0;
        for (const QString& path : textFiles()) {
            const QString rel = relative(path);
            if (rel.startsWith(u"docs/validation/"_s) && rel.endsWith(u".txt"_s))
                continue; // test logs as captured (QtTest writes the ANSI code page)
            ++count;
            QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
            const QString text = utf8.decode(bytesOf(path));
            if (utf8.hasError())
                problems.append(rel);
        }
        QVERIFY(count > 500);
        QVERIFY2(problems.isEmpty(), qPrintable(u"not UTF-8:"_s + report(problems)));
    }

    // Windows PowerShell 5.1 reads a script without a BOM in the ANSI code
    // page: a Chinese string can swallow its closing quote and the code after.
    void powershellWithChineseHasBom()
    {
        QStringList problems;
        for (const QString& path : textFiles()) {
            const QString suffix = QFileInfo(path).suffix().toLower();
            if (suffix != u"ps1" && suffix != u"psm1" && suffix != u"psd1")
                continue;
            const QByteArray bytes = bytesOf(path);
            if (!ascii(bytes) && !bytes.startsWith("\xEF\xBB\xBF"))
                problems.append(relative(path));
        }
        QVERIFY2(problems.isEmpty(), qPrintable(u"non-ASCII without a UTF-8 BOM:"_s + report(problems)));
    }

    // An icon written as a private-use character can be lost by a tool that
    // rewrites the file: `glyph: "" // ChromeClose` was the 公式识别 window's
    // close button, there and not to be seen (a chat report, 2026-11-27). An
    // empty glyph that a comment names is such a loss.
    void namedGlyphsAreNotEmpty()
    {
        static const QRegularExpression lost(uR"(\b\w*[gG]lyph\w*:\s*""\s*//\s*\w)"_s);
        QStringList problems;
        int count = 0;
        for (const QString& path : textFiles()) {
            if (!path.endsWith(u".qml"_s, Qt::CaseInsensitive))
                continue;
            ++count;
            const QStringList lines = QString::fromUtf8(bytesOf(path)).split(u'\n');
            for (qsizetype i = 0; i < lines.size(); ++i) {
                if (lost.match(lines[i]).hasMatch())
                    problems.append(u"%1:%2"_s.arg(relative(path)).arg(i + 1));
            }
        }
        QVERIFY(count > 100);
        QVERIFY2(problems.isEmpty(), qPrintable(u"an icon's character is gone:"_s + report(problems)));
    }

    // cmd reads a batch file in the OEM code page and chokes on a BOM.
    void batchFilesAreAscii()
    {
        QStringList problems;
        for (const QString& path : textFiles()) {
            const QString suffix = QFileInfo(path).suffix().toLower();
            if ((suffix == u"bat" || suffix == u"cmd") && !ascii(bytesOf(path)))
                problems.append(relative(path));
        }
        QVERIFY2(problems.isEmpty(), qPrintable(u"not ASCII:"_s + report(problems)));
    }

    // rc.exe reads a resource script in the ANSI code page unless told.
    void resourceScriptsSayUtf8()
    {
        QStringList problems;
        for (const QString& path : textFiles()) {
            if (QFileInfo(path).suffix().toLower() != u"rc")
                continue;
            const QByteArray bytes = bytesOf(path);
            if (!ascii(bytes) && !bytes.contains("#pragma code_page(65001)"))
                problems.append(relative(path));
        }
        QVERIFY2(problems.isEmpty(), qPrintable(u"non-ASCII without #pragma code_page(65001):"_s + report(problems)));
    }

    // Text goes through QString and the W functions; the code page only
    // where the data really is in it (kCodePageUsers). No ANSI (A) calls.
    void codePagesStayWhereTheyBelong()
    {
        static const QRegularExpression codePage(uR"(\b(fromLocal8Bit|toLocal8Bit|CP_ACP|CP_OEMCP|GetACP|GetOEMCP|QStringDecoder::System|QStringConverter::System|QTextCodec)\b)"_s);
        static const QRegularExpression ansiCall(uR"(\b(?:Get|Set|Create|Open|Find|Load|Reg|Shell|Send|Post|Message|Copy|Move|Delete|Write|Read|Expand|Format|Output|Char|Draw|Insert|Append|Modify|Def|Call|Dispatch|Peek|Translate|Register|Unregister|Enum|Query|Lookup|Search|Compare|lstr|wsprintf|wvsprintf)\w*A\s*\()"_s);
        QStringList problems;
        QSet<QString> used;
        for (const QString& path : textFiles()) {
            const QString rel = relative(path);
            const QString suffix = QFileInfo(path).suffix();
            if (!rel.startsWith(u"src/"_s) || !(suffix == u"cpp" || suffix == u"h" || suffix == u"hpp" || suffix == u"c"))
                continue;
            const QStringList lines = QString::fromUtf8(bytesOf(path)).split(u'\n');
            for (qsizetype n = 0; n < lines.size(); ++n) {
                const QString code = codeOf(lines[n]);
                if (const auto call = ansiCall.match(code); call.hasMatch())
                    problems.append(u"%1:%2: %3 (use the W function)"_s.arg(rel).arg(n + 1).arg(call.captured(0)));
                if (const auto page = codePage.match(code); page.hasMatch()) {
                    if (kCodePageUsers.contains(rel))
                        used.insert(rel);
                    else
                        problems.append(u"%1:%2: %3 (UTF-8 / UTF-16, or add the file to kCodePageUsers with the reason)"_s
                                .arg(rel).arg(n + 1).arg(page.captured(0)));
                }
            }
        }
        for (auto it = kCodePageUsers.cbegin(); it != kCodePageUsers.cend(); ++it) {
            if (!used.contains(it.key()))
                problems.append(u"%1: listed in kCodePageUsers but no longer uses a code page"_s.arg(it.key()));
        }
        QVERIFY2(problems.isEmpty(), qPrintable(report(problems)));
    }
};

QTEST_GUILESS_MAIN(EncodingTest)
#include "tst_encoding.moc"
