#include "Mojibake.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

constexpr UINT kGbk = 936;

// What a program does to `text` when it reads its UTF-8 in `codePage`.
QString garbled(const QString& text, UINT codePage = kGbk)
{
    const QByteArray utf8 = text.toUtf8();
    const int size = ::MultiByteToWideChar(codePage, 0, utf8.constData(), static_cast<int>(utf8.size()), nullptr, 0);
    QString wide(size, Qt::Uninitialized);
    ::MultiByteToWideChar(codePage, 0, utf8.constData(), static_cast<int>(utf8.size()),
        reinterpret_cast<LPWSTR>(wide.data()), size);
    return wide;
}

} // namespace

class MojibakeTest : public QObject {
    Q_OBJECT
private slots:
    void reportedPath() // GitHub #346
    {
        const QString path = u"D:\\BaiduSyncdisk\\tourbox预设\\Winshun设置备份"_s;
        const QString shown = garbled(path);
        QVERIFY(shown.contains(u"棰勮"_s) && shown.contains(u"璁剧疆澶囦唤"_s)); // as the screenshot shows it
        QCOMPARE(utf8ReadAsAnsi(shown, kGbk), std::optional<QString>(path));
    }

    void neverAWrongAnswer()
    {
        // A garbling that lost bytes cannot be undone: none, never another text.
        const QStringList texts {u"预设"_s, u"设置备份"_s, u"文件夹"_s, u"我的文档 2026"_s, u"日本語のフォルダ"_s,
            u"한국어"_s, u"Ünïcödé"_s, u"😀 表情"_s, u"a预b设c"_s, u"项目资料\\合同.docx"_s};
        for (const QString& text : texts) {
            const std::optional<QString> back = utf8ReadAsAnsi(garbled(text), kGbk);
            QVERIFY2(!back || *back == text, qPrintable(text));
        }
    }

    void cleanTextStays()
    {
        for (const QString& text : {u""_s, u"C:\\Users\\Public"_s, u"中文路径"_s, u"D:\\百度网盘\\工作\\报告.docx"_s,
                 u"设置 - Win顺"_s, u"收藏夹"_s, u"最近的文件夹"_s, u"桌面"_s, u"下载"_s})
            QVERIFY2(!utf8ReadAsAnsi(text, kGbk), qPrintable(text));
        QVERIFY(!utf8ReadAsAnsi(garbled(u"预设"_s), CP_UTF8)); // nothing to undo where ANSI is UTF-8
    }

    void ungarbledPathNeedsTheFolder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString real = QDir::toNativeSeparators(dir.filePath(u"tourbox预设"_s));
        QVERIFY(QDir().mkpath(real));
        if (::GetACP() == kGbk) { // ungarbledPath() uses this machine's code page
            const QString shown = garbled(real);
            QCOMPARE(ungarbledPath(shown), real);
            QVERIFY(QDir().mkpath(shown)); // made by a backup to the garbled path: the real one still wins
            QCOMPARE(ungarbledPath(shown), real);
            QVERIFY(QDir(real).removeRecursively());
            QCOMPARE(ungarbledPath(shown), shown); // nothing better on disk: as it was
        }
        QCOMPARE(ungarbledPath(real), real);
        const QString missing = dir.filePath(u"不存在的文件夹"_s);
        QCOMPARE(ungarbledPath(missing), missing);
        QCOMPARE(ungarbledPath(QString()), QString());
    }
};

QTEST_GUILESS_MAIN(MojibakeTest)
#include "tst_mojibake.moc"
