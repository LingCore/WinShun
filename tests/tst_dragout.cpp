// What a drop target gets when results are dragged out (dragout::dataObject):
// the files as CF_HDROP and as the shell's item list, a copy preferred.
#include "DragOut.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <shellapi.h>
#include <shlobj.h>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

QStringList dropped(IDataObject* data)
{
    FORMATETC format {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium {};
    if (FAILED(data->GetData(&format, &medium)))
        return {u"(no CF_HDROP)"_s};
    QStringList out;
    const auto drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH] {};
        ::DragQueryFileW(drop, i, path, MAX_PATH);
        out.append(QString::fromWCharArray(path));
    }
    ::ReleaseStgMedium(&medium);
    return out;
}

DWORD preferredEffect(IDataObject* data)
{
    FORMATETC format {static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)), nullptr,
        DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium {};
    if (FAILED(data->GetData(&format, &medium)))
        return 0xFFFFFFFF;
    const DWORD effect = *static_cast<DWORD*>(::GlobalLock(medium.hGlobal));
    ::GlobalUnlock(medium.hGlobal);
    ::ReleaseStgMedium(&medium);
    return effect;
}

// The formats it offers, by name.
QStringList formats(IDataObject* data)
{
    QStringList out;
    IEnumFORMATETC* list = nullptr;
    if (FAILED(data->EnumFormatEtc(DATADIR_GET, &list)))
        return out;
    FORMATETC f {};
    while (list->Next(1, &f, nullptr) == S_OK) {
        wchar_t name[128] {};
        out.append(::GetClipboardFormatNameW(f.cfFormat, name, 128) ? QString::fromWCharArray(name)
                                                                    : QString::number(f.cfFormat));
        if (f.ptd)
            ::CoTaskMemFree(f.ptd);
    }
    list->Release();
    return out;
}

} // namespace

class TestDragOut : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(SUCCEEDED(::OleInitialize(nullptr))); }
    void cleanupTestCase() { ::OleUninitialize(); }

    void itemsOfSeveralFolders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u"甲/文件夹"_s));
        QVERIFY(QDir(dir.path()).mkpath(u"乙"_s));
        const QString a = QDir::toNativeSeparators(dir.filePath(u"甲/报告 2026.pdf"_s));
        const QString b = QDir::toNativeSeparators(dir.filePath(u"乙/notes.txt"_s));
        const QString folder = QDir::toNativeSeparators(dir.filePath(u"甲/文件夹"_s));
        for (const QString& f : {a, b}) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        const auto data = dragout::dataObject({a, b, folder});
        QVERIFY(data);
        QCOMPARE(dropped(data.Get()), (QStringList {a, b, folder})); // as picked, from different folders
        const QStringList offered = formats(data.Get());
        QVERIFY2(offered.contains(QString::fromWCharArray(CFSTR_SHELLIDLIST)), qPrintable(offered.join(u", "))); // what Explorer reads
        QCOMPARE(preferredEffect(data.Get()), DWORD(DROPEFFECT_COPY));
    }

    void qtImagePath()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u"图片 export"_s));
        // Clipboard exports start as Qt paths, including spaces and Unicode.
        const QString path = dir.filePath(u"图片 export/image_261.png"_s);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("image bytes");
        file.close();
        const auto data = dragout::dataObject({path});
        QVERIFY(data);
        QCOMPARE(dropped(data.Get()), QStringList {QDir::toNativeSeparators(path)});
        QCOMPARE(preferredEffect(data.Get()), DWORD(DROPEFFECT_COPY));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("image bytes")); // data object never changes the source
    }

    void missingOnes()
    {
        QTemporaryDir dir;
        const QString kept = QDir::toNativeSeparators(dir.filePath(u"kept.txt"_s));
        QFile file(kept);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        // Deleted since the search: left out, the rest still goes.
        const auto data = dragout::dataObject({QDir::toNativeSeparators(dir.filePath(u"gone.txt"_s)), kept});
        QVERIFY(data);
        QCOMPARE(dropped(data.Get()), QStringList {kept});
        QVERIFY(!dragout::dataObject({QDir::toNativeSeparators(dir.filePath(u"gone.txt"_s))}));
        QVERIFY(!dragout::dataObject({}));
    }

    // Copied (Ctrl+C): the files and nothing a program could paste as text
    // instead. WPS pasted a copied picture's path from the text (GitHub #130).
    void copiedFilesOnly()
    {
        // Never looked up: a server that went away must not hold the copy up.
        const QString offline = u"\\\\192.0.2.1\\share\\照片 1.jpg"_s;
        const QString local = u"C:/no such folder/b.png"_s;
        const auto data = dragout::fileNamesObject({offline, local});
        QVERIFY(data);
        QCOMPARE(dropped(data.Get()), (QStringList {offline, QDir::toNativeSeparators(local)}));
        QCOMPARE(preferredEffect(data.Get()), DWORD(DROPEFFECT_COPY));
        const QStringList offered = formats(data.Get());
        QCOMPARE(offered.size(), 2); // CF_HDROP and Preferred DropEffect
        QVERIFY2(offered.contains(QString::number(CF_HDROP)), qPrintable(offered.join(u", ")));
        for (const UINT text : {CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT}) {
            FORMATETC f {static_cast<CLIPFORMAT>(text), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            QVERIFY(data->QueryGetData(&f) != S_OK);
        }
        QVERIFY(!dragout::fileNamesObject({}));
    }
};

QTEST_GUILESS_MAIN(TestDragOut)
#include "tst_dragout.moc"
