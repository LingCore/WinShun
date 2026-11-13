// The user's rules for file windows programs draw themselves
// (platform/DialogRules, GitHub #272): how dialogs.json is read, which
// windows a rule names, and the driving itself through UI Automation --
// against a native window with an edit box and buttons on a thread of its
// own, and against a WPF window in another process (PowerShell), the kind
// ArcGIS Pro shows. Neither is ArcGIS: none is installed here.

#include "platform/DialogRules.h"
#include "platform/FileDialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

using namespace Qt::StringLiterals;
namespace rules = ws::dialogrules;
namespace fd = ws::filedialog;
using fd::Kind;

namespace {

// Match the production foreground handoff without a key or mouse movement.
// Windows may otherwise refuse a noninteractive test's SetForegroundWindow.
void bringToFront(HWND window)
{
    INPUT nothing {};
    nothing.type = INPUT_MOUSE;
    ::SendInput(1, &nothing, sizeof nothing);
    ::SetForegroundWindow(window);
}

std::wstring ownProgram()
{
    wchar_t path[MAX_PATH] {};
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::wstring_view full(path, length);
    return std::wstring(full.substr(full.find_last_of(L'\\') + 1));
}

// A program's own file window, as a native one: a label, a box for the name,
// "Add" (the default button: Enter in the box presses it) and "Cancel". A
// folder typed with a trailing backslash is gone to and the box emptied; any
// other text is taken for the answer.
class NativeWindow {
public:
    static constexpr int kBox = 0x4D2; // 1234
    static constexpr int kAdd = 0x4D3;

    NativeWindow()
    {
        std::promise<void> ready;
        m_thread = std::thread([&] {
            WNDCLASSW wc {};
            wc.lpfnWndProc = &proc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = L"WsRuleFixture";
            ::RegisterClassW(&wc);
            m_window = ::CreateWindowExW(0, wc.lpszClassName, L"Add Data (fixture)", WS_POPUP | WS_VISIBLE, -20000, -20000, 420, 200,
                nullptr, nullptr, wc.hInstance, this);
            const auto child = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w) {
                return ::CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, 24, m_window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
            };
            child(L"Static", L"Name:", 0, -1, 10, 100, 60);
            m_box = child(L"Edit", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, kBox, 80, 100, 300);
            child(L"Button", L"Add", WS_TABSTOP | BS_DEFPUSHBUTTON, kAdd, 200, 140, 80);
            child(L"Button", L"Cancel", WS_TABSTOP, IDCANCEL, 300, 140, 80);
            ready.set_value();
            MSG msg;
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (!::IsDialogMessageW(m_window, &msg)) {
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
            }
        });
        ready.get_future().wait();
    }
    ~NativeWindow()
    {
        ::PostMessageW(m_window, WM_CLOSE, 0, 0);
        m_thread.join();
    }

    HWND handle() const { return m_window; }
    std::wstring box() const
    {
        wchar_t text[512] {};
        DWORD_PTR copied = 0;
        ::SendMessageTimeoutW(m_box, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(text), SMTO_ABORTIFHUNG, 2000, &copied);
        return text;
    }
    void setBox(const wchar_t* text) { ::SendMessageW(m_box, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text)); }
    void keepFolderInBox() { m_keep = true; } // a window that does not empty its box
    std::vector<std::wstring> wentTo() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_wentTo;
    }
    std::vector<std::wstring> answers() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_answers;
    }

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<NativeWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
        case DM_GETDEFID:
            return MAKELRESULT(kAdd, DC_HASDEFID);
        case WM_COMMAND:
            if (self && LOWORD(wParam) == kAdd) {
                wchar_t text[512] {};
                ::GetWindowTextW(self->m_box, text, 512);
                const std::wstring typed = text;
                const std::scoped_lock lock(self->m_mutex);
                if (typed.ends_with(L'\\')) {
                    self->m_wentTo.push_back(typed);
                    if (!self->m_keep)
                        ::SetWindowTextW(self->m_box, L"");
                } else {
                    self->m_answers.push_back(typed);
                }
            }
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(window, message, wParam, lParam);
        }
    }

    std::thread m_thread;
    HWND m_window = nullptr;
    HWND m_box = nullptr;
    std::atomic<bool> m_keep {false};
    mutable std::mutex m_mutex;
    std::vector<std::wstring> m_wentTo;
    std::vector<std::wstring> m_answers;
};

rules::Rule nativeRule(bool byButton)
{
    rules::Rule rule;
    rule.program = ownProgram();
    ::CharLowerBuffW(rule.program.data(), static_cast<DWORD>(rule.program.size()));
    rule.windowClass = L"WsRuleFixture";
    rule.title = L"Add Data*";
    rule.box.type = L"edit";
    rule.box.id = std::to_wstring(NativeWindow::kBox);
    if (byButton) {
        rule.confirm.type = L"button";
        rule.confirm.name = L"Add";
    }
    return rule;
}

} // namespace

class TestDialogRules : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))); }
    void cleanup() { rules::setRules({}); }

    void patterns()
    {
        QVERIFY(rules::matches(L"", L"anything"));
        QVERIFY(rules::matches(L"#32770", L"#32770"));
        QVERIFY(rules::matches(L"添加数据", L"添加数据"));
        QVERIFY(!rules::matches(L"#32770", L"#327700"));
        QVERIFY(rules::matches(L"hwndwrapper[arcgispro*", L"HwndWrapper[ArcGISPro;;f8c1]"));
        QVERIFY(!rules::matches(L"HwndWrapper[ArcGISPro*", L"HwndWrapper[Other;;f8c1]"));
        QVERIFY(rules::matches(L"*数据", L"添加数据"));
        QVERIFY(rules::matches(L"Add*Data*", L"Add Some Data (2)"));
        QVERIFY(!rules::matches(L"Add*Data", L"Add Data (2)"));
        QVERIFY(rules::matches(L"*", L""));
        QVERIFY(!rules::matches(L"a*a", L"a")); // the two ends may not share a letter
    }

    void readsRules()
    {
        QStringList problems;
        // (' for " here: moc stumbles over a raw string with this much in it.)
        const QByteArray json = QByteArray(
            "{'dialogs': ["
            "{'name': 'ArcMap', 'program': 'ArcMap.exe', 'class': '#32770', 'title': '添加数据',"
            " 'box': {'type': 'Edit', 'name': '名称:'}, 'confirm': {'type': 'button', 'id': 1}},"
            "{'program': 'ArcGISPro.exe', 'class': 'HwndWrapper[ArcGISPro*', 'kind': 'folder',"
            " 'box': {'id': 'NameBox', 'index': 1}, 'confirm': 'enter', 'seen': {'boxes': ['x']}},"
            "{'class': '#32770', 'box': {'type': 'edit'}},"
            "{'program': 'C:\\\\a\\\\b.exe', 'box': {'type': 'edit'}},"
            "{'program': 'a.exe'},"
            "{'program': 'a.exe', 'box': {'type': 'edit'}, 'kind': 'browse'}"
            "]}").replace('\'', '"');
        const std::vector<rules::Rule> read = rules::parse(json, &problems);
        QCOMPARE(read.size(), std::size_t(2));
        QCOMPARE(read[0].program, std::wstring(L"arcmap.exe"));
        QCOMPARE(read[0].kind, Kind::Open);
        QCOMPARE(read[0].box.type, std::wstring(L"edit"));
        QCOMPARE(read[0].box.name, std::wstring(L"名称:"));
        QCOMPARE(read[0].confirm.id, std::wstring(L"1")); // a number reads as well
        QCOMPARE(read[1].kind, Kind::Folder);
        QCOMPARE(read[1].box.index, 1);
        QVERIFY(!read[1].confirm.given()); // Enter in the box
        QCOMPARE(problems.size(), qsizetype(4));
        QVERIFY(problems[0].contains(u"#3"_s) && problems[0].contains(u"program"_s));
        QVERIFY(problems[2].contains(u"#5"_s) && problems[2].contains(u"box"_s));
        QVERIFY(problems[3].contains(u"kind"_s));
        // What is written reads back the same.
        for (const rules::Rule& rule : read) {
            const QByteArray again = QJsonDocument(QJsonObject {{u"dialogs"_s, QJsonArray {rules::toJson(rule)}}}).toJson();
            QCOMPARE(rules::parse(again), std::vector<rules::Rule> {rule});
        }
        // Not JSON: nothing, and a word why; an empty file is no problem.
        problems.clear();
        QVERIFY(rules::parse("{\"dialogs\": [", &problems).empty());
        QCOMPARE(problems.size(), qsizetype(1));
        problems.clear();
        QVERIFY(rules::parse("  ", &problems).empty() && problems.isEmpty());
    }

    void namesOnlyItsWindow()
    {
        const NativeWindow window;
        QCOMPARE(fd::kind(window.handle()), Kind::None); // without a rule it is nothing
        QVERIFY(!fd::byRule(window.handle()));
        rules::Rule rule = nativeRule(false);
        rule.kind = Kind::Save;
        rules::setRules({rule});
        QCOMPARE(fd::kind(window.handle()), Kind::Save);
        QVERIFY(fd::byRule(window.handle()));
        QVERIFY(fd::currentFolder(window.handle()).empty());
        QVERIFY(fd::fileType(window.handle()).empty());
        for (const auto& change : {&rules::Rule::program, &rules::Rule::windowClass, &rules::Rule::title}) {
            rules::Rule other = rule;
            other.*change = L"something-else";
            rules::setRules({other});
            QCOMPARE(fd::kind(window.handle()), Kind::None);
        }
        // A window Win顺 knows by itself is never a rule's.
        rule.windowClass.clear();
        rule.title.clear();
        rules::setRules({rule});
        QVERIFY(fd::byRule(window.handle()));
        QVERIFY(!fd::byRule(::GetDesktopWindow()));
    }

    void goesByEnterAndByButton_data()
    {
        QTest::addColumn<bool>("byButton");
        QTest::newRow("Enter in the box") << false;
        QTest::newRow("the Add button") << true;
    }

    void goesByEnterAndByButton()
    {
        QFETCH(bool, byButton);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString named = root.filePath(u"图层 data.gdb"_s); // a folder that looks like a file
        QVERIFY(QDir().mkpath(named));
        const std::wstring folder = QDir::toNativeSeparators(named).toStdWString();
        NativeWindow window;
        rules::setRules({nativeRule(byButton)});
        bringToFront(window.handle());
        QTRY_COMPARE(::GetForegroundWindow(), window.handle());
        window.setBox(L"roads.shp");

        // A folder: typed with a trailing backslash and confirmed, never an answer.
        QVERIFY(fd::goTo(window.handle(), folder));
        QTRY_COMPARE(window.wentTo(), std::vector<std::wstring> {folder + L'\\'});
        QVERIFY(window.answers().empty());
        // Missing folders and files are not typed anywhere.
        QVERIFY(!fd::goTo(window.handle(), folder + L"\\missing"));
        QCOMPARE(window.wentTo().size(), std::size_t(1));

        // A window that leaves the folder in its box gets back what it held.
        window.keepFolderInBox();
        window.setBox(L"roads.shp");
        QVERIFY(fd::goTo(window.handle(), folder));
        QTRY_COMPARE(window.wentTo().size(), std::size_t(2));
        QCOMPARE(window.box(), std::wstring(L"roads.shp"));

        // A file: its whole path in the box, confirmed only when asked.
        const std::wstring file = folder + L"\\roads.shp";
        QVERIFY(fd::setFileName(window.handle(), file));
        QCOMPARE(window.box(), file);
        QVERIFY(window.answers().empty());
        QVERIFY(fd::accept(window.handle()));
        QTRY_COMPARE(window.answers(), std::vector<std::wstring> {file});
    }

    void failsWithoutItsBox()
    {
        QTemporaryDir root;
        NativeWindow window;
        rules::Rule rule = nativeRule(false);
        rule.box.id = L"9999";
        rules::setRules({rule});
        bringToFront(window.handle());
        QVERIFY(!fd::goTo(window.handle(), QDir::toNativeSeparators(root.path()).toStdWString()));
        QVERIFY(!fd::setFileName(window.handle(), L"a.txt"));
        QVERIFY(window.wentTo().empty() && window.box().empty());
    }

    void describesAWindow()
    {
        NativeWindow window;
        bringToFront(window.handle());
        const rules::Seen seen = rules::describe(window.handle());
        std::wstring program = ownProgram();
        ::CharLowerBuffW(program.data(), static_cast<DWORD>(program.size()));
        QCOMPARE(seen.rule.program, program);
        QCOMPARE(seen.rule.windowClass, std::wstring(L"WsRuleFixture"));
        QCOMPARE(seen.rule.title, std::wstring(L"Add Data (fixture)"));
        QCOMPARE(seen.rule.box.type, std::wstring(L"edit"));
        QCOMPARE(seen.rule.box.id, std::to_wstring(NativeWindow::kBox));
        QVERIFY(!seen.rule.confirm.given());
        QCOMPARE(seen.boxes.size(), qsizetype(1));
        QVERIFY(seen.buttons.join(u'\n').contains(u"name=Add"_s));
        // The rule it made drives the window.
        QTemporaryDir root;
        const std::wstring folder = QDir::toNativeSeparators(root.path()).toStdWString();
        rules::setRules({seen.rule});
        QVERIFY(fd::goTo(window.handle(), folder));
        QTRY_COMPARE(window.wentTo(), std::vector<std::wstring> {folder + L'\\'});
    }

    // A WPF window in another process, as ArcGIS Pro's is: no child windows,
    // a class name with a part that changes each run, Enter handled by WPF.
    void wpfWindow()
    {
        const QString powershell = QStandardPaths::findExecutable(u"powershell.exe"_s);
        if (powershell.isEmpty())
            QSKIP("Windows PowerShell is not here");
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString went = root.filePath(u"went.txt"_s);
        const QString target = root.filePath(u"工程 数据"_s);
        QVERIFY(QDir().mkpath(target));
        // (__FILE__ is UTF-8 here; QFINDTESTDATA does not find this folder's Chinese name.)
        const QString script = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(u"fixtures/wpf-file-window.ps1"_s);
        QVERIFY2(QFileInfo::exists(script), qPrintable(script));
        QProcess fixture;
        fixture.setProgram(powershell);
        fixture.setArguments({u"-NoProfile"_s, u"-STA"_s, u"-ExecutionPolicy"_s, u"Bypass"_s, u"-File"_s,
            QDir::toNativeSeparators(script), u"-Result"_s, QDir::toNativeSeparators(went)});
        fixture.start();
        const auto stop = qScopeGuard([&] {
            fixture.kill();
            fixture.waitForFinished(3000);
        });
        HWND window = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((window = ::FindWindowW(nullptr, L"WsRuleFixture 添加数据")) != nullptr, 20000);

        bringToFront(window);
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), window, 5000);
        const rules::Seen seen = rules::describe(window);
        QCOMPARE(seen.rule.program, std::wstring(L"powershell.exe"));
        QVERIFY2(seen.rule.windowClass.starts_with(L"HwndWrapper[") && seen.rule.windowClass.ends_with(L'*'),
            qPrintable(QString::fromStdWString(seen.rule.windowClass)));
        QCOMPARE(seen.rule.box.id, std::wstring(L"NameBox")); // the lowest box, not the search box above it
        QCOMPARE(seen.boxes.size(), qsizetype(2));

        rules::setRules({seen.rule});
        QCOMPARE(fd::kind(window), Kind::Open);
        const std::wstring folder = QDir::toNativeSeparators(target).toStdWString();
        QVERIFY(fd::goTo(window, folder));
        const auto wentTo = [&] {
            QFile file(went);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
        };
        QTRY_COMPARE_WITH_TIMEOUT(wentTo(), QString::fromStdWString(folder + L'\\'), 5000);
    }
};

QTEST_GUILESS_MAIN(TestDialogRules)
#include "tst_dialogrules.moc"
