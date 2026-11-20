#include "PastePlainProbe.h"

#include "App.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::preview {

namespace {

constexpr UINT kModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
constexpr UINT kKey = VK_F22;
constexpr wchar_t kTargetTitle[] = L"WinShun paste-plain target";

// The text box of another process (a paste never goes into Win顺's own
// windows): Windows PowerShell's WinForms RichTextBox, which pastes the RTF
// of a copy when there is one. Once something is in it, what it holds is
// written down and it closes; after 30 s it closes empty.
constexpr char kTargetScript[] = R"PS(param([string]$result)
Add-Type -AssemblyName System.Windows.Forms
$form = New-Object System.Windows.Forms.Form
$form.Text = 'WinShun paste-plain target'
$form.TopMost = $true
$form.StartPosition = 'CenterScreen'
$form.Width = 460
$form.Height = 200
$box = New-Object System.Windows.Forms.RichTextBox
$box.Dock = 'Fill'
$form.Controls.Add($box)
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 300
$script:waited = 0
$script:seen = 0
$timer.Add_Tick({
    $script:waited += 300
    if ($box.TextLength -gt 0) { $script:seen += 1 }
    if ($script:seen -ge 2 -or $script:waited -ge 30000) {
        $timer.Stop()
        $o = [ordered]@{ text = $box.Text; rtf = $box.Rtf; timedOut = ($box.TextLength -eq 0) }
        [IO.File]::WriteAllText($result, ($o | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding $false))
        $form.Close()
    }
})
$form.Add_Shown({ $form.Activate(); [void]$box.Focus(); $timer.Start() })
[void]$form.ShowDialog()
)PS";

const QString kPlain = u"WinShun #408 粘贴为纯文本 plain"_s;
constexpr char kRichText[] = R"({\rtf1\ansi\deff0{\fonttbl{\f0 Arial;}}\f0\b RICH bold copy\b0\par})";

void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    do
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    while (timer.elapsed() < ms);
}

void waitForQuietInput()
{
    QElapsedTimer waited;
    waited.start();
    for (;;) {
        LASTINPUTINFO last {};
        last.cbSize = sizeof last;
        if (!::GetLastInputInfo(&last) || ::GetTickCount() - last.dwTime >= 3000 || waited.elapsed() > 120'000)
            return;
        pump(200);
    }
}

bool openClipboard()
{
    for (int i = 0; i < 40; ++i) {
        if (::OpenClipboard(nullptr))
            return true;
        ::Sleep(25);
    }
    return false;
}

struct Item {
    UINT format = 0;
    QByteArray data;
};

// Everything on the clipboard that is memory (not a GDI handle), to put back.
std::vector<Item> saveClipboard(bool& ok)
{
    std::vector<Item> items;
    ok = openClipboard();
    if (!ok)
        return items;
    for (UINT format = ::EnumClipboardFormats(0); format; format = ::EnumClipboardFormats(format)) {
        if (format == CF_BITMAP || format == CF_ENHMETAFILE || format == CF_METAFILEPICT || format == CF_PALETTE
            || format == CF_OWNERDISPLAY || format == CF_DSPBITMAP || format == CF_DSPENHMETAFILE || format == CF_DSPMETAFILEPICT
            || (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST))
            continue;
        const HANDLE handle = ::GetClipboardData(format);
        const void* data = handle ? ::GlobalLock(handle) : nullptr;
        if (!data)
            continue;
        items.push_back({format, QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(::GlobalSize(handle)))});
        ::GlobalUnlock(handle);
    }
    ::CloseClipboard();
    return items;
}

// Put on the clipboard with the formats that keep it out of every history
// (Windows' own, Win顺's that runs): ClipboardWatcher's WINSHUN_TEST_NO_CLIP_HISTORY.
bool putOnClipboard(const std::vector<Item>& items)
{
    if (!openClipboard())
        return false;
    ::EmptyClipboard();
    bool ok = true;
    const auto add = [&](UINT format, const void* data, std::size_t size) {
        const HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, std::max<std::size_t>(size, 1));
        void* to = memory ? ::GlobalLock(memory) : nullptr;
        if (!to) {
            ok = false;
            return;
        }
        std::memcpy(to, data, size);
        ::GlobalUnlock(memory);
        if (!::SetClipboardData(format, memory)) {
            ::GlobalFree(memory);
            ok = false;
        }
    };
    for (const Item& item : items)
        add(item.format, item.data.constData(), static_cast<std::size_t>(item.data.size()));
    const DWORD zero = 0;
    add(::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), &zero, sizeof zero);
    add(::RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), &zero, sizeof zero);
    ::CloseClipboard();
    return ok;
}

QString clipboardText(bool* rich)
{
    QString text;
    if (!openClipboard())
        return text;
    if (const HANDLE handle = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* data = static_cast<const wchar_t*>(::GlobalLock(handle))) {
            text = QString::fromWCharArray(data);
            ::GlobalUnlock(handle);
        }
    }
    *rich = ::IsClipboardFormatAvailable(::RegisterClipboardFormatW(L"Rich Text Format"))
        || ::IsClipboardFormatAvailable(::RegisterClipboardFormatW(L"HTML Format"));
    ::CloseClipboard();
    return text;
}

HWND findTarget(qint64 pid)
{
    struct Find { DWORD pid; HWND found; } find {static_cast<DWORD>(pid), nullptr};
    ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto* f = reinterpret_cast<Find*>(data);
        DWORD pid = 0;
        ::GetWindowThreadProcessId(window, &pid);
        wchar_t title[64] {};
        ::GetWindowTextW(window, title, 64);
        if (pid == f->pid && std::wcscmp(title, kTargetTitle) == 0) {
            f->found = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&find));
    return find.found;
}

bool inFront(HWND target)
{
    return ::GetAncestor(::GetForegroundWindow(), GA_ROOT) == target;
}

void bringToFront(HWND target)
{
    ::ShowWindow(target, SW_SHOW);
    if (inFront(target))
        return;
    const DWORD front = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
    const DWORD ours = ::GetCurrentThreadId();
    const bool attached = front && front != ours && ::AttachThreadInput(ours, front, TRUE);
    ::SetForegroundWindow(target);
    ::BringWindowToTop(target);
    if (attached)
        ::AttachThreadInput(ours, front, FALSE);
}

// The chord pressed and let go, as a person would (the keys then are up for the paste).
void pressChord()
{
    INPUT keys[8] {};
    const WORD order[] {VK_CONTROL, VK_MENU, VK_SHIFT, static_cast<WORD>(kKey)};
    for (int i = 0; i < 4; ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wVk = order[i];
        keys[7 - i].type = INPUT_KEYBOARD;
        keys[7 - i].ki.wVk = order[i];
        keys[7 - i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    ::SendInput(8, keys, sizeof(INPUT));
}

} // namespace

bool pastePlainChordFree()
{
    constexpr int id = 0xBF08;
    if (!::RegisterHotKey(nullptr, id, kModifiers | MOD_NOREPEAT, kKey))
        return false;
    ::UnregisterHotKey(nullptr, id);
    return true;
}

// --probe-runtime <folder> --paste-plain-check (WINSHUN_TEST_NO_CLIP_HISTORY set)
// The production App beside the Win顺 that runs, 粘贴为纯文本's hotkey set to
// Ctrl+Alt+Shift+F22 (free before it started, its own after). A rich copy is put
// on the clipboard: plain text and RTF that say different things, so that what
// lands shows which was pasted. A RichTextBox of another process is brought to
// the front and the chord pressed for real (SendInput, only while that box is
// in front). Passed: the box got the plain text, not the RTF, and the clipboard
// holds that plain text without its RTF. The clipboard as it was is saved first
// (every format that is memory) and put back after, marked as the test's copies
// are so that no history records it. Into paste-plain.json. Count Windows'
// clipboard history before and after (docs/validation/issue287-20261111/harness/history-count.ps1).
bool checkPastePlain(App&, const QStringList&, const QDir& output)
{
    QJsonObject report;
    bool ok = true;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            ok = false;
            std::fprintf(stderr, "paste plain: FAILED: %s\n", what);
        }
    };
    const auto finish = [&] {
        report.insert(u"passed"_s, ok);
        QFile file(output.filePath(u"paste-plain.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        std::fprintf(stderr, "paste plain: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    };
    check(qEnvironmentVariableIsSet("WINSHUN_TEST_NO_CLIP_HISTORY"), "WINSHUN_TEST_NO_CLIP_HISTORY not set: the paste would be in the histories");
    pump(1500); // started, its hotkeys registered
    const bool ours = !::RegisterHotKey(nullptr, 0xBF09, kModifiers | MOD_NOREPEAT, kKey) && ::GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED;
    if (!ours)
        ::UnregisterHotKey(nullptr, 0xBF09);
    report.insert(u"hotkey_registered"_s, ours);
    check(ours, "the hotkey is not registered");
    if (!ok)
        return finish();

    bool savedOk = false;
    const std::vector<Item> before = saveClipboard(savedOk);
    report.insert(u"clipboard_saved_formats"_s, static_cast<int>(before.size()));
    check(savedOk, "the clipboard could not be read to be put back");
    if (!ok)
        return finish();
    const QString resultPath = output.filePath(u"target.json"_s);
    const QString scriptPath = output.filePath(u"target.ps1"_s);
    QFile::remove(resultPath);
    {
        QFile script(scriptPath);
        check(script.open(QIODevice::WriteOnly) && script.write(kTargetScript) > 0, "the target's script not written");
    }
    qint64 pid = 0;
    const bool started = ok && QProcess::startDetached(u"powershell.exe"_s,
        {u"-NoProfile"_s, u"-STA"_s, u"-ExecutionPolicy"_s, u"Bypass"_s, u"-File"_s, QDir::toNativeSeparators(scriptPath),
            QDir::toNativeSeparators(resultPath)}, {}, &pid);
    check(started, "the target did not start");
    HWND target = nullptr;
    QElapsedTimer limit;
    limit.start();
    while (started && !(target = findTarget(pid)) && limit.elapsed() < 20'000)
        pump(100);
    check(target, "the target's window never came");

    bool changed = false; // the user's clipboard replaced: to be put back
    bool restored = false;
    if (ok) {
        changed = true;
        // A rich copy: its plain text and its RTF say different things.
        const std::wstring plain = kPlain.toStdWString();
        const QByteArray rtf(kRichText);
        check(putOnClipboard({{CF_UNICODETEXT, QByteArray(reinterpret_cast<const char*>(plain.c_str()),
                                    static_cast<qsizetype>((plain.size() + 1) * sizeof(wchar_t)))},
                  {::RegisterClipboardFormatW(L"Rich Text Format"), QByteArray(rtf.constData(), rtf.size() + 1)}}),
            "the rich copy could not be put on the clipboard");
        waitForQuietInput();
        bringToFront(target);
        pump(600);
        const bool front = inFront(target);
        report.insert(u"target_in_front"_s, front);
        check(front, "the target could not be brought to the front (no key sent)");
        if (ok && inFront(target)) // checked again just before the keys
            pressChord();
        limit.restart();
        while (ok && !QFile::exists(resultPath) && limit.elapsed() < 12'000)
            pump(100);
        pump(300);
        QFile result(resultPath);
        const QJsonObject got = result.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(result.readAll()).object() : QJsonObject();
        const QString text = got.value(u"text"_s).toString();
        const QString boxRtf = got.value(u"rtf"_s).toString();
        bool rich = true;
        const QString onClipboard = clipboardText(&rich);
        report.insert(u"target_text"_s, text);
        report.insert(u"target_bold"_s, boxRtf.contains(u"\\b "_s) || boxRtf.contains(u"\\b\\"_s));
        report.insert(u"clipboard_text"_s, onClipboard);
        report.insert(u"clipboard_rich"_s, rich);
        if (ok) {
            check(!got.isEmpty() && !got.value(u"timedOut"_s).toBool(), "nothing was pasted into the target");
            check(text == kPlain, "the target did not get the plain text");
            check(!text.contains(u"RICH"_s), "the RTF was pasted");
            check(onClipboard == kPlain && !rich, "the clipboard does not hold the plain text alone");
        }
        std::fprintf(stderr, "paste plain: target got \"%s\", clipboard \"%s\" %s\n", qPrintable(text), qPrintable(onClipboard),
            rich ? "(still rich)" : "(plain)");
        restored = putOnClipboard(before);
    }
    if (pid > 0) {
        if (const HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid))) {
            if (::WaitForSingleObject(process, 3000) == WAIT_TIMEOUT)
                ::TerminateProcess(process, 1);
            ::CloseHandle(process);
        }
    }
    if (changed && !restored)
        restored = putOnClipboard(before); // once more
    if (changed) {
        bool savedAgain = false;
        report.insert(u"clipboard_restored"_s, restored);
        report.insert(u"clipboard_formats_after"_s, static_cast<int>(saveClipboard(savedAgain).size())); // + the two marks
        check(restored, "the clipboard as it was could not be put back");
    }
    QFile::remove(scriptPath);
    return finish();
}

} // namespace ws::preview
