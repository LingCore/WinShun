// ws::pathbox: windows of other programs that take a folder in a box of
// their own (PathBoxDialog.h), against stand-ins made of real controls
// (FakePathBoxDialog.h). 7-Zip's and WinRAR's are known only in those
// programs: copies of this test named 7zG.exe and WinRAR.exe run sevenZip and
// winRar (ctest -R tst_pathbox_), and under its own name the same windows are
// left alone. realBandizip drives the real program, when told where it is.
#include "FakePathBoxDialog.h"
#include "PathBoxDialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <commctrl.h>

namespace pb = ws::pathbox;
using fakepathbox::make;
using fakepathbox::SevenZipDialog;
using fakepathbox::TreeBoxDialog;
using fakepathbox::WinRarDialog;

namespace {

bool runsAs(const char* program)
{
    return QFileInfo(QCoreApplication::applicationFilePath()).fileName().compare(QLatin1String(program), Qt::CaseInsensitive) == 0;
}

HWND visibleWindowOf(DWORD process)
{
    struct Search {
        DWORD process;
        HWND found = nullptr;
    } search {process};
    ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto* s = reinterpret_cast<Search*>(data);
        DWORD pid = 0;
        ::GetWindowThreadProcessId(window, &pid);
        if (pid != s->process || !::IsWindowVisible(window))
            return TRUE;
        s->found = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

} // namespace

class TestPathBox : public QObject {
    Q_OBJECT
private slots:
    void looksLikePath_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("path");
        QTest::newRow("drive root") << QStringLiteral("C:\\") << true;
        QTest::newRow("folder") << QStringLiteral("d:\\新 项目\\a b") << true;
        QTest::newRow("unc") << QStringLiteral("\\\\server\\share\\x") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("words") << QStringLiteral("search here") << false;
        QTest::newRow("drive alone") << QStringLiteral("C:") << false;
        QTest::newRow("drive relative") << QStringLiteral("C:folder") << false;
        QTest::newRow("rooted") << QStringLiteral("\\folder") << false;
        QTest::newRow("server alone") << QStringLiteral("\\\\server") << false;
        QTest::newRow("server and slash") << QStringLiteral("\\\\server\\") << false;
        QTest::newRow("device") << QStringLiteral("\\\\.\\C:\\x") << false;
        QTest::newRow("url") << QStringLiteral("https://example.com/a") << false;
    }

    void looksLikePath()
    {
        QFETCH(QString, text);
        QFETCH(bool, path);
        QCOMPARE(pb::looksLikePath(text.toStdWString()), path);
    }

    // Bandizip's kind: the folder goes into the box, the program hears of the
    // edit (its tree follows), an entry of the list chosen before is no
    // longer, and nothing is confirmed.
    void treeAndBox_data()
    {
        QTest::addColumn<bool>("plainEdit");
        QTest::addColumn<QString>("folder");
        QTest::newRow("combo") << false << QStringLiteral("D:\\target");
        QTest::newRow("combo, named like a file") << false << QStringLiteral("D:\\a.zip");
        QTest::newRow("combo, non-ascii and space") << false << QStringLiteral("E:\\目标 文件夹\\子 目录");
        QTest::newRow("combo, share") << false << QStringLiteral("\\\\server\\share\\工具");
        QTest::newRow("edit") << true << QStringLiteral("D:\\目标 文件夹");
    }

    void treeAndBox()
    {
        QFETCH(bool, plainEdit);
        QFETCH(QString, folder);
        TreeBoxDialog dialog(L"C:\\Users", plainEdit);
        QVERIFY(dialog.valid());
        if (dialog.combo()) {
            ::SendMessageW(dialog.combo(), CB_SETCURSEL, 1, 0);
            QCOMPARE(dialog.path(), std::wstring(L"D:\\earlier two"));
        }
        const pb::Box box = pb::find(dialog.handle());
        QVERIFY(box);
        QCOMPARE(box.edit, dialog.edit());
        QCOMPARE(box.combo, dialog.combo());
        QVERIFY(!box.show);
        QCOMPARE(pb::folder(box), dialog.path());

        QVERIFY(pb::setFolder(box, folder.toStdWString()));
        QCOMPARE(dialog.path(), folder.toStdWString());
        QCOMPARE(pb::folder(box), folder.toStdWString());
        QTRY_COMPARE(dialog.shown(), folder.toStdWString()); // told of the edit: posted
        QVERIFY(dialog.edits() >= 1);
        if (dialog.combo())
            QCOMPARE(::SendMessageW(dialog.combo(), CB_GETCURSEL, 0, 0), LRESULT(CB_ERR));
        QVERIFY(dialog.answers().empty());
        QVERIFY(::IsWindow(dialog.handle()));
    }

    // Known by the path in its box; once known, the caller may say so, and it
    // stays one whatever the user is typing there.
    void treeAndBoxByItsText()
    {
        TreeBoxDialog dialog(L"type to search");
        QVERIFY(dialog.valid());
        QVERIFY(!pb::find(dialog.handle()));
        QVERIFY(pb::find(dialog.handle(), true));
        ::SetWindowTextW(dialog.edit(), L"\\\\nas\\media");
        QVERIFY(pb::find(dialog.handle()));
        ::SetWindowTextW(dialog.edit(), L"");
        QVERIFY(!pb::find(dialog.handle()));
        QVERIFY(pb::find(dialog.handle(), true));
    }

    void treeAndBoxRefusals()
    {
        const pb::Box none;
        QVERIFY(!pb::find(nullptr));
        QVERIFY(!pb::setFolder(none, L"C:\\"));
        {
            TreeBoxDialog dialog;
            QVERIFY(pb::find(dialog.handle()));
            QVERIFY(!pb::setFolder(pb::find(dialog.handle()), L""));
            QCOMPARE(dialog.path(), std::wstring(L"C:\\Users"));
        }
        { // lists the user picks from, labels and check boxes change nothing
            TreeBoxDialog dialog;
            make(L"ComboBox", dialog.handle(), CBS_DROPDOWNLIST, 50);
            make(L"Static", dialog.handle(), 0, 51, L"Path:");
            make(L"Button", dialog.handle(), BS_AUTOCHECKBOX, 52, L"Remember");
            make(L"Edit", dialog.handle(), ES_READONLY, 53, L"C:\\shown only");
            QVERIFY(pb::find(dialog.handle()));
        }
        { // a second box to type in
            TreeBoxDialog dialog;
            make(L"Edit", dialog.handle(), ES_AUTOHSCROLL, 50, L"C:\\other");
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // a second combo box to type in
            TreeBoxDialog dialog;
            make(L"ComboBox", dialog.handle(), CBS_DROPDOWN, 50);
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // a list: a file window of some kind, or settings
            TreeBoxDialog dialog;
            make(WC_LISTVIEWW, dialog.handle(), LVS_REPORT, 50);
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // pages
            TreeBoxDialog dialog;
            make(WC_TABCONTROLW, dialog.handle(), 0, 50);
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // two trees
            TreeBoxDialog dialog;
            make(WC_TREEVIEWW, dialog.handle(), 0, 50);
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // no tree
            TreeBoxDialog dialog;
            ::DestroyWindow(dialog.tree());
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // a hidden tree
            TreeBoxDialog dialog;
            ::ShowWindow(dialog.tree(), SW_HIDE);
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // SHBrowseForFolder with an edit box (BIF_EDITBOX): its tree is DialogNav's and FileDialog's Browse kind
            TreeBoxDialog dialog;
            ::SetWindowLongPtrW(dialog.tree(), GWLP_ID, 0x3741);
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // no Cancel
            TreeBoxDialog dialog;
            ::DestroyWindow(dialog.cancel());
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // no OK
            TreeBoxDialog dialog;
            ::DestroyWindow(dialog.ok());
            QVERIFY(!pb::find(dialog.handle(), true));
        }
        { // the box greyed, hidden, read-only
            TreeBoxDialog greyed;
            ::EnableWindow(greyed.combo(), FALSE);
            QVERIFY(!pb::find(greyed.handle(), true));
            TreeBoxDialog hidden;
            ::ShowWindow(hidden.combo(), SW_HIDE);
            QVERIFY(!pb::find(hidden.handle(), true));
            TreeBoxDialog readOnly;
            ::SendMessageW(readOnly.edit(), EM_SETREADONLY, TRUE, 0);
            QVERIFY(!pb::find(readOnly.handle(), true));
        }
        { // a password, a number
            TreeBoxDialog dialog(L"C:\\Users", true);
            QVERIFY(pb::find(dialog.handle()));
            ::SetWindowLongPtrW(dialog.edit(), GWL_STYLE, ::GetWindowLongPtrW(dialog.edit(), GWL_STYLE) | ES_NUMBER);
            QVERIFY(!pb::find(dialog.handle(), true));
        }
    }

    // 7-Zip's OK answers with the list's selected entry before the text: after
    // setFolder it answers with the folder. Only in 7zG.exe / 7zFM.exe.
    void sevenZip()
    {
        SevenZipDialog dialog;
        const std::wstring before = dialog.answer();
        QCOMPARE(before, std::wstring(L"C:\\extracted before\\"));
        const pb::Box box = pb::find(dialog.handle(), true);
        if (!runsAs("7zG.exe")) {
            QVERIFY(!box);
            QCOMPARE(dialog.answer(), before);
            return;
        }
        QVERIFY(box);
        QCOMPARE(box.combo, dialog.path());
        QVERIFY(!box.show);
        QCOMPARE(pb::folder(box), before);
        const std::wstring folder = L"E:\\解压 到这里\\a.7z";
        QVERIFY(pb::setFolder(box, folder));
        QCOMPARE(dialog.answer(), folder);
        QCOMPARE(pb::folder(box), folder);
        QVERIFY(dialog.answers().empty());
        // Not the Extract window: one of its lists missing.
        ::DestroyWindow(::GetDlgItem(dialog.handle(), 103));
        QVERIFY(!pb::find(dialog.handle(), true));
    }

    // WinRAR's: the box on the first page, then "Display" pressed for the
    // folder just written; on the other page (the first one hidden) as well.
    // Only in WinRAR.exe.
    void winRar()
    {
        WinRarDialog dialog;
        const std::wstring before = dialog.text();
        const pb::Box box = pb::find(dialog.handle(), true);
        if (!runsAs("WinRAR.exe")) {
            QVERIFY(!box);
            return;
        }
        QVERIFY(box);
        QCOMPARE(box.combo, dialog.path());
        QVERIFY(box.show);
        QCOMPARE(pb::folder(box), before);
        const std::wstring folder = L"E:\\解压 到这里";
        QVERIFY(pb::setFolder(box, folder));
        QCOMPARE(dialog.text(), folder);
        QTRY_COMPARE(dialog.displayed().size(), std::size_t(1)); // posted
        QCOMPARE(dialog.displayed().front(), folder);
        ::ShowWindow(dialog.page(), SW_HIDE);
        const pb::Box hidden = pb::find(dialog.handle());
        QVERIFY(hidden);
        QVERIFY(pb::setFolder(hidden, L"D:\\other"));
        QCOMPARE(dialog.text(), std::wstring(L"D:\\other"));
        QTRY_COMPARE(dialog.displayed().size(), std::size_t(2));
        QCOMPARE(dialog.answers(), 0);
        ::EnableWindow(dialog.path(), FALSE);
        QVERIFY(!pb::find(dialog.handle()));
    }

    // The real Bandizip, started here on an archive made here, extracting
    // into a folder made here: WINSHUN_TEST_BANDIZIP=<path of Bandizip.exe>.
    // The window is found, takes the folder, its tree follows, and its own OK
    // then extracts there.
    void realBandizip()
    {
        const QString program = qEnvironmentVariable("WINSHUN_TEST_BANDIZIP");
        if (program.isEmpty())
            QSKIP("Set WINSHUN_TEST_BANDIZIP to Bandizip.exe to drive the real window");
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QDir dir(work.path());
        QVERIFY(dir.mkpath(QStringLiteral("目标 文件夹")));
        const QString target = QDir::toNativeSeparators(dir.filePath(QStringLiteral("目标 文件夹")));
        {
            QFile file(dir.filePath(QStringLiteral("inside.txt")));
            QVERIFY(file.open(QIODevice::WriteOnly) && file.write("winshun") == 7);
        }
        QProcess pack;
        pack.setWorkingDirectory(work.path());
        pack.start(QStringLiteral("tar.exe"), {QStringLiteral("-a"), QStringLiteral("-c"), QStringLiteral("-f"),
            QStringLiteral("probe.zip"), QStringLiteral("inside.txt")});
        QVERIFY(pack.waitForFinished(20000) && pack.exitCode() == 0);

        QProcess bandizip;
        bandizip.start(program, {QStringLiteral("x"), QStringLiteral("-target:dlg"),
            QStringLiteral("-o:") + QDir::toNativeSeparators(work.path()), QDir::toNativeSeparators(dir.filePath(QStringLiteral("probe.zip")))});
        QVERIFY(bandizip.waitForStarted(10000));
        const auto stop = qScopeGuard([&] {
            if (bandizip.state() != QProcess::NotRunning) {
                bandizip.kill();
                bandizip.waitForFinished(5000);
            }
        });
        HWND dialog = nullptr;
        pb::Box box;
        QTRY_VERIFY_WITH_TIMEOUT((dialog = visibleWindowOf(static_cast<DWORD>(bandizip.processId())))
            && (box = pb::find(dialog)), 15000);
        QVERIFY(box.combo);
        QCOMPARE(QDir::cleanPath(QString::fromStdWString(pb::folder(box))).toLower(), QDir::cleanPath(work.path()).toLower());

        const HWND tree = ::FindWindowExW(dialog, nullptr, WC_TREEVIEWW, nullptr);
        QVERIFY(tree);
        const auto selected = [tree] { return ::SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CARET, 0); };
        const LRESULT before = selected();
        QVERIFY(pb::setFolder(box, target.toStdWString()));
        QCOMPARE(pb::folder(box), target.toStdWString());
        QTRY_VERIFY_WITH_TIMEOUT(selected() != before && selected() != 0, 5000);
        QVERIFY(::IsWindow(dialog)); // not confirmed for the user
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("目标 文件夹/inside.txt")))
            && !QFile::exists(dir.filePath(QStringLiteral("目标 文件夹/probe/inside.txt"))));

        // The user's OK: it extracts into the folder written (or a folder
        // named after the archive in it, by a setting of the program's).
        ::PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(::GetDlgItem(dialog, IDOK)));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dir.filePath(QStringLiteral("目标 文件夹/inside.txt")))
            || QFile::exists(dir.filePath(QStringLiteral("目标 文件夹/probe/inside.txt"))), 20000);
        QVERIFY(bandizip.waitForFinished(20000));
    }
};

QTEST_MAIN(TestPathBox)
#include "tst_pathbox.moc"
