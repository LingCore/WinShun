#include "PastePlainProbe.h"

#include "App.h"
#include "ClipImage.h"
#include "ClipStore.h"
#include "Clipboard.h"
#include "Launcher.h"
#include "Settings.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGuiApplication>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>
#include <shellapi.h>

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
void pressChord(UINT key = kKey)
{
    INPUT keys[8] {};
    const WORD order[] {VK_CONTROL, VK_MENU, VK_SHIFT, static_cast<WORD>(key)};
    for (int i = 0; i < 4; ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wVk = order[i];
        keys[7 - i].type = INPUT_KEYBOARD;
        keys[7 - i].ki.wVk = order[i];
        keys[7 - i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    ::SendInput(8, keys, sizeof(INPUT));
}

void pressKey(WORD key)
{
    INPUT keys[2] {};
    keys[0].type = keys[1].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[1].ki.wVk = key;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, keys, sizeof(INPUT));
}

bool chordFree(UINT key)
{
    constexpr int id = 0xBF08;
    if (!::RegisterHotKey(nullptr, id, kModifiers | MOD_NOREPEAT, key))
        return false;
    ::UnregisterHotKey(nullptr, id);
    return true;
}

// Registered by this process (the App's hotkey): another registration of it fails so.
bool chordOurs(UINT key)
{
    constexpr int id = 0xBF09;
    if (::RegisterHotKey(nullptr, id, kModifiers | MOD_NOREPEAT, key)) {
        ::UnregisterHotKey(nullptr, id);
        return false;
    }
    return ::GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED;
}

} // namespace

bool pastePlainChordFree()
{
    return chordFree(kKey);
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
    const bool ours = chordOurs(kKey);
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

namespace {
constexpr UINT kEntryKey = VK_F20;
const QString kEntryText = u"WinShun 条目快捷键 entry hotkey 7731"_s;
const QString kNewerText = u"WinShun newer entry, not this one"_s;
} // namespace

bool entryHotkeyChordFree()
{
    return chordFree(kEntryKey);
}

bool seedEntryHotkey(const QString& clipboardFolder)
{
    ClipStore store(clipboardFolder);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!store.open(now))
        return false;
    ClipCapture entry;
    entry.text = kEntryText;
    entry.source = u"Entry hotkey probe"_s;
    const qint64 id = store.add(entry, now - 2000);
    ClipCapture newer = entry;
    newer.text = kNewerText;
    const bool seeded = id != 0 && store.add(newer, now - 1000) != 0 && store.setHotkey(id, QString(kEntryHotkeyProbeChord));
    store.flush();
    return seeded;
}

// What the App holds, for the check (a friend of App).
struct EntryHotkeyProbe {
    static const ClipStore& store(App& app) { return *app.m_clipStore; }
    static bool duplicatesKept(App& app) { return app.m_clipStore->keepsDuplicates(); }
};

// --probe-runtime <folder> --entry-hotkey-check (WINSHUN_TEST_NO_CLIP_HISTORY set)
// The production App beside the Win顺 that runs, its history seeded with two
// entries (seedEntryHotkey), the older one's own shortcut Ctrl+Alt+Shift+F20
// (free before it started, its own after: registered from the history at
// start) and 保留重复项 on. A text box of another process is brought to the
// front and the chord pressed for real (SendInput, only while that box is in
// front). Passed: the box got the older entry's words (not the newer one's),
// and the history still has two entries (a paste from it is no copy, even
// keeping duplicates) with the shortcut on the same one. The clipboard as it
// was is saved first and put back after, marked so that no history records it.
// Into entry-hotkey.json.
bool checkEntryHotkey(App& app, const QStringList&, const QDir& output)
{
    QJsonObject report;
    bool ok = true;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            ok = false;
            std::fprintf(stderr, "entry hotkey: FAILED: %s\n", what);
        }
    };
    const auto finish = [&] {
        report.insert(u"passed"_s, ok);
        QFile file(output.filePath(u"entry-hotkey.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        std::fprintf(stderr, "entry hotkey: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    };
    check(qEnvironmentVariableIsSet("WINSHUN_TEST_NO_CLIP_HISTORY"), "WINSHUN_TEST_NO_CLIP_HISTORY not set: the paste would be in the histories");
    pump(1500); // started, its hotkeys registered
    const bool ours = chordOurs(kEntryKey);
    report.insert(u"hotkey_registered"_s, ours);
    check(ours, "the entry's hotkey is not registered");
    const bool duplicates = EntryHotkeyProbe::duplicatesKept(app);
    report.insert(u"duplicates_kept"_s, duplicates);
    check(duplicates, "保留重复项 is not on in the history");
    const auto entries = [&app] { return static_cast<int>(EntryHotkeyProbe::store(app).clips().size()); };
    report.insert(u"entries_before"_s, entries());
    check(entries() == 2, "the history is not the two seeded entries");
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
        waitForQuietInput();
        bringToFront(target);
        pump(600);
        const bool front = inFront(target);
        report.insert(u"target_in_front"_s, front);
        check(front, "the target could not be brought to the front (no key sent)");
        if (ok && inFront(target)) // checked again just before the keys
            pressChord(kEntryKey);
        limit.restart();
        while (ok && !QFile::exists(resultPath) && limit.elapsed() < 12'000)
            pump(100);
        pump(800); // the paste's own clipboard change, read back by the history
        QFile result(resultPath);
        const QJsonObject got = result.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(result.readAll()).object() : QJsonObject();
        const QString text = got.value(u"text"_s).toString();
        report.insert(u"target_text"_s, text);
        report.insert(u"entries_after"_s, entries());
        const qint64 holder = EntryHotkeyProbe::store(app).hotkeyEntry(QString(kEntryHotkeyProbeChord));
        const Clip* entry = EntryHotkeyProbe::store(app).clip(holder);
        report.insert(u"hotkey_entry_text"_s, entry ? entry->text : QString());
        if (ok) {
            check(!got.isEmpty() && !got.value(u"timedOut"_s).toBool(), "nothing was pasted into the target");
            check(text == kEntryText, "the target did not get the entry's words");
            check(entries() == 2, "the paste added an entry to the history");
            check(entry && entry->text == kEntryText, "the shortcut is no longer on its entry");
        }
        std::fprintf(stderr, "entry hotkey: target got \"%s\", %d entries\n", qPrintable(text), entries());
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
        report.insert(u"clipboard_restored"_s, restored);
        check(restored, "the clipboard as it was could not be put back");
    }
    QFile::remove(scriptPath);
    return finish();
}

namespace {
const QString kRepeatedText = u"WinShun 保留重复项 duplicates 5524"_s;

// A copy as a program makes it, which only the Win顺 under test records
// (ClipboardWatcher::read's test copy; putOnClipboard keeps it from the rest).
bool copyAsProgram(const QString& text)
{
    const QByteArray words(reinterpret_cast<const char*>(text.utf16()), (text.size() + 1) * static_cast<qsizetype>(sizeof(char16_t)));
    return putOnClipboard({{CF_UNICODETEXT, words}, {::RegisterClipboardFormatW(L"WinShun.Test.Copy"), QByteArray(1, '\1')}});
}
} // namespace

// --probe-runtime <folder> --duplicates-check [--keep-place] (WINSHUN_TEST_NO_CLIP_HISTORY set)
// The production App beside the Win顺 that runs, 保留重复项 on (粘贴后保持位置
// too for --keep-place), its history seeded as for --entry-hotkey-check.
// On the real clipboard, read by the App's own watcher at the times they
// come: the same words copied, copied again over a second later (two
// entries), then written twice 150 ms apart (one more, not two: one copy of
// a program that writes twice). The older seeded entry is then pasted into a
// text box of another process by its shortcut, pressed for real (no entry
// added; on top unless its place is kept), and its words copied over a second
// after: an entry of its own on top either way, the pasted one still there.
// The clipboard as it was is put back, nothing of this in any other history.
// Into duplicates.json.
bool checkDuplicates(App& app, const QStringList& args, const QDir& output)
{
    QJsonObject report;
    bool ok = true;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            ok = false;
            std::fprintf(stderr, "duplicates: FAILED: %s\n", what);
        }
    };
    const auto finish = [&] {
        report.insert(u"passed"_s, ok);
        QFile file(output.filePath(u"duplicates.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        std::fprintf(stderr, "duplicates: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    };
    const bool keepPlace = args.contains(u"--keep-place"_s);
    report.insert(u"keep_place"_s, keepPlace);
    check(qEnvironmentVariableIsSet("WINSHUN_TEST_NO_CLIP_HISTORY"), "WINSHUN_TEST_NO_CLIP_HISTORY not set: the copies would be in the histories");
    pump(1500); // started, its hotkeys registered
    check(chordOurs(kEntryKey), "the entry's hotkey is not registered");
    check(EntryHotkeyProbe::duplicatesKept(app), "保留重复项 is not on in the history");
    const ClipStore& store = EntryHotkeyProbe::store(app);
    check(store.keepsPlaceOnPaste() == keepPlace, "粘贴后保持位置 is not as asked");
    const auto entries = [&store] { return static_cast<int>(store.clips().size()); };
    const auto count = [&store](const QString& text) {
        return static_cast<int>(std::ranges::count(store.clips(), text, &Clip::text));
    };
    const auto top = [&store] { return store.clips().empty() ? QString() : store.clips().front().text; };
    // The seeded two, and what was on the clipboard as it started if it is recorded.
    const int seeded = entries();
    report.insert(u"entries_at_start"_s, seeded);
    check(seeded >= 2 && seeded <= 3 && count(kEntryText) == 1 && count(kRepeatedText) == 0, "the history is not the seeded entries");
    if (!ok)
        return finish();

    bool savedOk = false;
    const std::vector<Item> before = saveClipboard(savedOk);
    check(savedOk, "the clipboard could not be read to be put back");
    if (!ok)
        return finish();
    bool restored = false;
    const auto putBack = qScopeGuard([&] {
        if (!restored)
            putOnClipboard(before);
    });

    // Copied, and copied again over a second later: two entries.
    check(copyAsProgram(kRepeatedText), "the first copy could not be written");
    pump(1200);
    report.insert(u"after_first_copy"_s, count(kRepeatedText));
    check(count(kRepeatedText) == 1 && top() == kRepeatedText, "the first copy is not in the history");
    check(copyAsProgram(kRepeatedText), "the second copy could not be written");
    pump(1200);
    report.insert(u"after_second_copy"_s, count(kRepeatedText));
    check(count(kRepeatedText) == 2, "the same words copied twice running are not two entries");
    // Written twice at once: one copy.
    check(copyAsProgram(kRepeatedText), "the third copy could not be written");
    pump(150);
    check(copyAsProgram(kRepeatedText), "the third copy could not be written again");
    pump(1200);
    report.insert(u"after_double_write"_s, count(kRepeatedText));
    check(count(kRepeatedText) == 3, "a copy written twice at once is not one more entry");
    check(entries() == seeded + 3, "the history is not three entries more before the paste");
    if (!ok)
        return finish();

    // The older seeded entry pasted by its shortcut.
    const QString resultPath = output.filePath(u"target.json"_s);
    const QString scriptPath = output.filePath(u"target.ps1"_s);
    QFile::remove(resultPath);
    {
        QFile script(scriptPath);
        check(script.open(QIODevice::WriteOnly) && script.write(kTargetScript) > 0, "the target's script not written");
    }
    const auto scriptGone = qScopeGuard([&] { QFile::remove(scriptPath); });
    qint64 pid = 0;
    const bool started = ok && QProcess::startDetached(u"powershell.exe"_s,
        {u"-NoProfile"_s, u"-STA"_s, u"-ExecutionPolicy"_s, u"Bypass"_s, u"-File"_s, QDir::toNativeSeparators(scriptPath),
            QDir::toNativeSeparators(resultPath)}, {}, &pid);
    check(started, "the target did not start");
    const auto targetGone = qScopeGuard([&] {
        if (pid <= 0)
            return;
        if (const HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid))) {
            if (::WaitForSingleObject(process, 3000) == WAIT_TIMEOUT)
                ::TerminateProcess(process, 1);
            ::CloseHandle(process);
        }
    });
    HWND target = nullptr;
    QElapsedTimer limit;
    limit.start();
    while (started && !(target = findTarget(pid)) && limit.elapsed() < 20'000)
        pump(100);
    check(target, "the target's window never came");
    if (ok) {
        waitForQuietInput();
        bringToFront(target);
        pump(600);
        check(inFront(target), "the target could not be brought to the front (no key sent)");
        if (ok && inFront(target)) // checked again just before the keys
            pressChord(kEntryKey);
        limit.restart();
        while (ok && !QFile::exists(resultPath) && limit.elapsed() < 12'000)
            pump(100);
        pump(1200); // the paste's own clipboard change read back, and past the time of one copy
        QFile result(resultPath);
        const QJsonObject got = result.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(result.readAll()).object() : QJsonObject();
        report.insert(u"target_text"_s, got.value(u"text"_s).toString());
        report.insert(u"entries_after_paste"_s, entries());
        report.insert(u"top_after_paste"_s, top());
        if (ok) {
            check(got.value(u"text"_s).toString() == kEntryText, "the target did not get the entry's words");
            check(entries() == seeded + 3, "the paste added an entry to the history");
            check((top() == kEntryText) != keepPlace, keepPlace ? "the pasted entry left its place" : "the pasted entry is not on top");
        }
    }
    // What was pasted, copied after: an entry of its own, on top.
    if (ok) {
        check(copyAsProgram(kEntryText), "the copy after the paste could not be written");
        pump(1200);
        report.insert(u"pasted_words_entries"_s, count(kEntryText));
        report.insert(u"entries_after_copy"_s, entries());
        check(count(kEntryText) == 2, "what was pasted, copied again, is not an entry of its own");
        check(entries() == seeded + 4 && top() == kEntryText, "the copy after the paste is not on top");
    }
    restored = putOnClipboard(before) || putOnClipboard(before);
    report.insert(u"clipboard_restored"_s, restored);
    check(restored, "the clipboard as it was could not be put back");
    return finish();
}

namespace {
constexpr UINT kCommandKey = VK_F19;
constexpr UINT kCommandSpareKey = VK_F18; // of the command with no keyword: never registered
const QString kCommandPlugin = u"test.keys"_s;
} // namespace

struct CommandHotkeyProbe {
    static QQuickWindow* window(App& app) { return app.m_window.data(); }
    static QString typed(App& app) { return app.m_launcher->query(); }
    static qsizetype registered(App& app) { return app.m_commandHotkeys.size(); }
    static QString error(App& app, const QString& command) { return app.m_commandHotkeyErrors.value(kCommandPlugin + u'/' + command); }
};

bool commandHotkeyChordFree()
{
    return chordFree(kCommandKey) && chordFree(kCommandSpareKey);
}

bool seedCommandHotkey(Settings& settings)
{
    const QString folder = Settings::profile() + u"/plugins/"_s + kCommandPlugin;
    const auto command = [](const QString& id, const QString& keyword) {
        return QJsonObject {{u"id"_s, id}, {u"title"_s, QString(u"按键 "_s + id)}, {u"keyword"_s, keyword},
            {u"run"_s, QJsonObject {{u"url"_s, u"https://example.invalid/?q=%s"_s}}}};
    };
    const QJsonObject plugin {{u"manifest"_s, 1}, {u"id"_s, kCommandPlugin}, {u"version"_s, u"1.0.0"_s},
        {u"name"_s, QJsonObject {{u"zh"_s, u"按键测试"_s}, {u"en"_s, u"Key test"_s}}},
        {u"commands"_s, QJsonArray {command(u"main"_s, u"wsk"_s), command(u"same"_s, u"wso"_s), command(u"bare"_s, u"wst"_s)}}};
    QFile manifest(folder + u"/plugin.json"_s);
    if (!QDir().mkpath(folder) || !manifest.open(QIODevice::WriteOnly) || manifest.write(QJsonDocument(plugin).toJson()) < 0)
        return false;
    PluginConfig& config = settings.pluginConfig(kCommandPlugin);
    config.enabled = true;
    config.keywords = {{u"bare"_s, QString()}}; // the user took its keyword away
    config.hotkeys = {{u"main"_s, QString(kCommandHotkeyProbeChord)}, {u"same"_s, u"ctrl+alt+shift+f19"_s},
        {u"bare"_s, u"Ctrl+Alt+Shift+F18"_s}};
    return true;
}

// --probe-runtime <folder> --command-hotkey-check
// The production App beside the Win顺 that runs, with a plugin of three
// commands that start nothing (seedCommandHotkey): `main` has the shortcut
// Ctrl+Alt+Shift+F19 (free before the App started), `same` the same keys
// spelled otherwise, `bare` keys of its own but no keyword. Passed: only
// main's keys are registered and the other two say why on their rows; the
// chord pressed for real (SendInput, once the keyboard has been quiet)
// brings the launcher up, in front, "wsk " typed; pressed again, it goes.
// Into command-hotkey.json.
bool checkCommandHotkey(App& app, const QStringList&, const QDir& output)
{
    QJsonObject report;
    bool ok = true;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            ok = false;
            std::fprintf(stderr, "command hotkey: FAILED: %s\n", what);
        }
    };
    const auto finish = [&] {
        report.insert(u"passed"_s, ok);
        QFile file(output.filePath(u"command-hotkey.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        std::fprintf(stderr, "command hotkey: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    };
    const auto waitFor = [](const auto& done, int ms) {
        QElapsedTimer limit;
        limit.start();
        while (!done() && limit.elapsed() < ms)
            pump(50);
        return done();
    };
    QQuickWindow* window = CommandHotkeyProbe::window(app);
    waitFor([&] { return chordOurs(kCommandKey); }, 15'000); // the plugins scanned, their shortcuts registered
    report.insert(u"hotkey_registered"_s, chordOurs(kCommandKey));
    report.insert(u"registered_count"_s, CommandHotkeyProbe::registered(app));
    report.insert(u"same_keys_note"_s, CommandHotkeyProbe::error(app, u"same"_s));
    report.insert(u"no_keyword_note"_s, CommandHotkeyProbe::error(app, u"bare"_s));
    check(chordOurs(kCommandKey), "the command's hotkey is not registered");
    check(CommandHotkeyProbe::registered(app) == 1, "not exactly one command's keys are registered");
    check(CommandHotkeyProbe::error(app, u"main"_s).isEmpty(), "the command whose keys work has a note");
    check(CommandHotkeyProbe::error(app, u"same"_s).contains(u"按键 main"_s), "the command with the same keys does not name who has them");
    check(!CommandHotkeyProbe::error(app, u"bare"_s).isEmpty() && chordFree(kCommandSpareKey),
        "the command with no keyword has its keys registered, or no note");
    check(window && !window->isVisible(), "the launcher is up before any key");
    if (!ok)
        return finish();

    waitForQuietInput();
    pressChord(kCommandKey);
    const bool shown = waitFor([&] { return window->isVisible() && window->isActive(); }, 5000);
    pump(600); // revealed, the caret after the keyword
    report.insert(u"launcher_shown"_s, window->isVisible());
    report.insert(u"launcher_active"_s, window->isActive());
    report.insert(u"typed"_s, CommandHotkeyProbe::typed(app));
    check(shown, "the launcher did not come to the front on the chord");
    check(CommandHotkeyProbe::typed(app) == u"wsk ", "the command's keyword is not what is typed");
    std::fprintf(stderr, "command hotkey: launcher shown %d active %d, typed \"%s\"\n", window->isVisible(), window->isActive(),
        qPrintable(CommandHotkeyProbe::typed(app)));
    if (ok && window->isActive()) { // only into our own window
        pressChord(kCommandKey);
        const bool gone = waitFor([&] { return !window->isVisible(); }, 5000);
        report.insert(u"closed_by_second_press"_s, gone);
        check(gone, "pressed again with its keyword typed, the launcher stayed");
    }
    if (window->isVisible())
        app.hideLauncher();
    return finish();
}

namespace {
constexpr UINT kActivationKey = VK_F21;
constexpr QRgb kProbePicture = qRgb(255, 0, 0); // what the harness looks for in the program
} // namespace

bool activationPasteChordFree()
{
    return chordFree(kActivationKey);
}

bool seedActivationPastePicture(const QString& clipboardFolder, int width)
{
    ClipStore store(clipboardFolder);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!store.open(now))
        return false;
    QImage picture(width, width * 2 / 3, QImage::Format_ARGB32);
    picture.fill(kProbePicture);
    const DecodedClipImage encoded = encodeClipImage(picture);
    ClipCapture clip;
    clip.kind = ClipKind::Image;
    clip.imageData = encoded.data;
    clip.imageFormat = encoded.format;
    clip.hash = encoded.hash;
    clip.width = picture.width();
    clip.height = picture.height();
    clip.source = u"Activation paste probe"_s;
    const bool added = !encoded.data.isEmpty() && store.add(clip, now) != 0;
    store.flush();
    return added;
}

// --probe-runtime <folder> --activation-paste-check <window> (WINSHUN_TEST_NO_CLIP_HISTORY set)
// The production App beside the Win顺 that runs, its clipboard's hotkey set to
// Ctrl+Alt+Shift+F21 (free before it started, its own after), one red picture
// in its history (seedActivationPastePicture). <window> (a number, the HWND)
// is brought to the front, the chord pressed for real (SendInput, only while
// <window> is in front): the clipboard opens over it without the focus, its
// keys routed, the way it opens over any program. Enter pastes the picture.
// Passed here: the keys went while <window> was in front, and it is in front
// after. Whether the red picture is what went in is the harness's to read off
// the program (its document's colour), against the picture the program read
// as it came to the front. The clipboard as it was is put back after, marked
// so that no history records it. Into activation-paste.json; the App's
// journal ("clipboard-paste", reactivated) is in this run's profile.
bool checkActivationPaste(App&, const QStringList& args, const QDir& output)
{
    QJsonObject report;
    bool ok = true;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            ok = false;
            std::fprintf(stderr, "activation paste: FAILED: %s\n", what);
        }
    };
    const auto finish = [&] {
        report.insert(u"passed"_s, ok);
        QFile file(output.filePath(u"activation-paste.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        std::fprintf(stderr, "activation paste: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    };
    check(qEnvironmentVariableIsSet("WINSHUN_TEST_NO_CLIP_HISTORY"), "WINSHUN_TEST_NO_CLIP_HISTORY not set: the paste would be in the histories");
    const qsizetype at = args.indexOf(u"--activation-paste-check"_s);
    const auto target = reinterpret_cast<HWND>(static_cast<quintptr>(at >= 0 && at + 1 < args.size() ? args[at + 1].toULongLong() : 0));
    check(target && ::IsWindow(target), "no window to paste into (--activation-paste-check <HWND>)");
    pump(1500); // started, its hotkeys registered
    const bool ours = chordOurs(kActivationKey);
    report.insert(u"hotkey_registered"_s, ours);
    check(ours, "the clipboard's hotkey is not registered");
    if (!ok)
        return finish();

    bool savedOk = false;
    const std::vector<Item> before = saveClipboard(savedOk);
    report.insert(u"clipboard_saved_formats"_s, static_cast<int>(before.size()));
    check(savedOk, "the clipboard could not be read to be put back");
    if (!ok)
        return finish();
    waitForQuietInput();
    bringToFront(target);
    pump(600);
    report.insert(u"target_in_front"_s, inFront(target));
    check(inFront(target), "the window could not be brought to the front (no key sent)");
    if (ok)
        pressChord(kActivationKey); // the clipboard over it, its keys routed
    pump(900);
    const bool frontAtEnter = inFront(target);
    report.insert(u"target_in_front_at_enter"_s, frontAtEnter);
    check(frontAtEnter, "the window lost the front as the clipboard opened");
    if (ok && inFront(target))
        pressKey(VK_RETURN);
    pump(2500); // the paste: the window back in front, a moment, the keys
    report.insert(u"target_in_front_after"_s, inFront(target));
    const bool picture = openClipboard() && (::IsClipboardFormatAvailable(CF_DIB) || ::IsClipboardFormatAvailable(CF_DIBV5));
    if (picture)
        ::CloseClipboard();
    report.insert(u"clipboard_picture"_s, picture);
    if (ok)
        check(picture, "the picture never went on the clipboard (nothing was pasted)");
    const bool restored = putOnClipboard(before) || putOnClipboard(before);
    report.insert(u"clipboard_restored"_s, restored);
    check(restored, "the clipboard as it was could not be put back");
    return finish();
}

namespace {

const QString kFocusEntry = u"WinShun 点搜索框用输入法 field focus 7741"_s;
const QString kFocusOther = u"WinShun another entry 7742"_s;
const QString kFocusTyped = u"输入法"_s;

// As an input method hands its words over: characters, no keys (VK_PACKET).
// They reach only the window with the keyboard focus.
void typeText(const QString& text)
{
    std::vector<INPUT> keys;
    for (const QChar c : text) {
        for (const bool up : {false, true}) {
            INPUT key {};
            key.type = INPUT_KEYBOARD;
            key.ki.wScan = c.unicode();
            key.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
            keys.push_back(key);
        }
    }
    ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
}

// A left click there, the pointer put back after. Only onto `window`.
bool clickAt(HWND window, POINT at)
{
    POINT was {};
    ::GetCursorPos(&was);
    ::SetCursorPos(at.x, at.y);
    pump(120);
    const bool onIt = ::GetAncestor(::WindowFromPoint(at), GA_ROOT) == window;
    if (onIt) {
        INPUT buttons[2] {};
        buttons[0].type = buttons[1].type = INPUT_MOUSE;
        buttons[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        buttons[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        ::SendInput(2, buttons, sizeof(INPUT));
        pump(120);
    }
    ::SetCursorPos(was.x, was.y);
    return onIt;
}

} // namespace

bool seedFieldFocus(const QString& clipboardFolder)
{
    ClipStore store(clipboardFolder);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!store.open(now))
        return false;
    ClipCapture entry;
    entry.text = kFocusEntry;
    entry.source = u"Field focus probe"_s;
    ClipCapture newer = entry;
    newer.text = kFocusOther;
    const bool seeded = store.add(entry, now - 2000) != 0 && store.add(newer, now - 1000) != 0;
    store.flush();
    return seeded;
}

// --probe-runtime <folder> --field-focus-check (WINSHUN_TEST_NO_CLIP_HISTORY set)
// The production App beside the Win顺 that runs, its clipboard's hotkey
// Ctrl+Alt+Shift+F21 and two entries in its history (seedFieldFocus). A text
// box of another process is brought to the front and the chord pressed for
// real: the clipboard opens over it without the focus, its keys routed. Then
// its search field is clicked for real (only while the pointer is on the
// clipboard's window). Passed: the window is in front with the field in
// focus and no keys routed; 输入法, typed as an input method hands words over
// (characters, which go to the focus alone), is the search and leaves the one
// entry that has it; Enter pastes that entry into the box. The clipboard as it
// was is saved first and put back after, marked so that no history records it.
// Into field-focus.json.
bool checkFieldFocus(App&, const QStringList&, const QDir& output)
{
    QJsonObject report;
    bool ok = true;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            ok = false;
            std::fprintf(stderr, "field focus: FAILED: %s\n", what);
        }
    };
    const auto finish = [&] {
        report.insert(u"passed"_s, ok);
        QFile file(output.filePath(u"field-focus.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        std::fprintf(stderr, "field focus: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    };
    check(qEnvironmentVariableIsSet("WINSHUN_TEST_NO_CLIP_HISTORY"), "WINSHUN_TEST_NO_CLIP_HISTORY not set: the paste would be in the histories");
    pump(1500); // started, its hotkeys registered
    const bool ours = chordOurs(kActivationKey);
    report.insert(u"hotkey_registered"_s, ours);
    check(ours, "the clipboard's hotkey is not registered");
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

    bool changed = false; // the user's clipboard may be replaced (the paste): to be put back
    if (ok) {
        waitForQuietInput();
        bringToFront(target);
        pump(600);
        check(inFront(target), "the target could not be brought to the front (no key sent)");
        if (ok && inFront(target))
            pressChord(kActivationKey); // the clipboard over it, its keys routed
        pump(900);
        QQuickWindow* window = nullptr;
        for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
            if (candidate->isVisible() && QByteArray(candidate->metaObject()->className()).startsWith("ClipboardWindow_"))
                window = qobject_cast<QQuickWindow*>(candidate);
        }
        auto* clipboard = window ? qobject_cast<Clipboard*>(window->property("clipboard").value<QObject*>()) : nullptr;
        auto* field = window ? window->findChild<QQuickItem*>(u"clipboardSearchField"_s) : nullptr;
        check(window && clipboard && field, "the clipboard's window did not open");
        if (ok) {
            const auto hwnd = reinterpret_cast<HWND>(window->winId());
            report.insert(u"opened_without_focus"_s, inFront(target));
            report.insert(u"opened_keys_routed"_s, clipboard->keysRouted());
            check(inFront(target) && clipboard->keysRouted(), "the clipboard did not open over the target with its keys routed");

            // What a Shift alone under a Chinese layout brings up: pinyin in the
            // field, and below it the click that brings the input method, each in full.
            emit clipboard->pinyinHintRequested();
            pump(400);
            const auto shownInFull = [&](const QString& name, QStringView word) {
                auto* text = window->findChild<QQuickItem*>(name);
                const QString said = text ? text->property("text").toString() : QString();
                report.insert(name, said);
                return text && text->isVisible() && said.contains(word) && !text->property("truncated").toBool();
            };
            check(shownInFull(u"clipboardSearchPlaceholder"_s, u"拼音"), "the field does not say pinyin finds Chinese, or is cut off");
            check(shownInFull(u"clipboardInputMethodNote"_s, u"输入法"), "nothing says how to use the input method, or it is cut off");
            window->grabWindow().save(output.filePath(u"field-hint.png"_s));

            // The field's middle, in the screen's pixels.
            const QPointF middle = field->mapToScene(QPointF(field->width() / 2, field->height() / 2)) * window->devicePixelRatio();
            POINT at {qRound(middle.x()), qRound(middle.y())};
            ::ClientToScreen(hwnd, &at);
            const bool clicked = ok && clickAt(hwnd, at);
            check(clicked, "the search field is not under the pointer (no click made)");
            pump(500);
            const bool front = ::GetForegroundWindow() == hwnd;
            report.insert(u"window_in_front_after_click"_s, front);
            report.insert(u"keys_routed_after_click"_s, clipboard->keysRouted());
            report.insert(u"field_has_focus"_s, field->hasActiveFocus());
            check(front, "the clipboard's window did not take the focus on the click");
            check(!clipboard->keysRouted(), "the keys are still routed after the click");
            check(field->hasActiveFocus(), "the search field does not have the focus");
            if (ok && ::GetForegroundWindow() == hwnd) { // checked again just before the keys
                typeText(kFocusTyped);
                pump(500);
                report.insert(u"query"_s, clipboard->query());
                report.insert(u"rows"_s, clipboard->items()->count());
                check(clipboard->query() == kFocusTyped, "what was typed as an input method does is not the search");
                check(clipboard->items()->count() == 1, "the search did not leave the one entry that has the words");
                window->grabWindow().save(output.filePath(u"field-typed.png"_s));
            }
            if (ok && ::GetForegroundWindow() == hwnd) {
                changed = true;
                pressKey(VK_RETURN);
                limit.restart();
                while (!QFile::exists(resultPath) && limit.elapsed() < 12'000)
                    pump(100);
                pump(500);
                QFile result(resultPath);
                const QJsonObject got = result.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(result.readAll()).object() : QJsonObject();
                const QString text = got.value(u"text"_s).toString();
                report.insert(u"target_text"_s, text);
                check(text == kFocusEntry, "Enter did not paste the entry into the target");
                std::fprintf(stderr, "field focus: target got \"%s\"\n", qPrintable(text));
            }
        }
    }
    if (pid > 0) {
        if (const HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid))) {
            if (::WaitForSingleObject(process, 3000) == WAIT_TIMEOUT)
                ::TerminateProcess(process, 1);
            ::CloseHandle(process);
        }
    }
    if (changed) {
        const bool restored = putOnClipboard(before) || putOnClipboard(before);
        report.insert(u"clipboard_restored"_s, restored);
        check(restored, "the clipboard as it was could not be put back");
    }
    QFile::remove(scriptPath);
    return finish();
}

// --probe-runtime <folder> --full-screen-game-check <window> (WINSHUN_TEST_NO_CLIP_HISTORY set)
// The production App beside the Win顺 that runs, its clipboard's hotkey
// Ctrl+Alt+Shift+F21 and two entries in its history (seedFieldFocus).
// <window> (a number, the HWND) is a game in Direct3D's exclusive full screen
// (docs/validation/issue-452/fsgame.cpp, which takes full screen again each
// time it comes to the front). Twice it is brought to the front and the chord
// pressed for real: the clipboard takes the focus over it, the game leaves
// full screen for it, and the clipboard is still there, in front, two and a
// half seconds on. The first time Esc closes it, the second Enter pastes;
// both give the game the front back. What the game got (its keys) is in its
// own log, for the harness. The clipboard as it was is saved first and put
// back after, marked so that no history records it. Into full-screen-game.json.
bool checkFullScreenGame(App&, const QStringList& args, const QDir& output)
{
    QJsonObject report;
    bool ok = true;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            ok = false;
            std::fprintf(stderr, "full-screen game: FAILED: %s\n", what);
        }
    };
    const auto finish = [&] {
        report.insert(u"passed"_s, ok);
        QFile file(output.filePath(u"full-screen-game.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0;
        std::fprintf(stderr, "full-screen game: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    };
    const auto waitFor = [](const auto& done, int ms) {
        QElapsedTimer limit;
        limit.start();
        while (!done() && limit.elapsed() < ms)
            pump(50);
        return done();
    };
    check(qEnvironmentVariableIsSet("WINSHUN_TEST_NO_CLIP_HISTORY"), "WINSHUN_TEST_NO_CLIP_HISTORY not set: the paste would be in the histories");
    const qsizetype at = args.indexOf(u"--full-screen-game-check"_s);
    const auto target = reinterpret_cast<HWND>(static_cast<quintptr>(at >= 0 && at + 1 < args.size() ? args[at + 1].toULongLong() : 0));
    check(target && ::IsWindow(target), "no game to open over (--full-screen-game-check <HWND>)");
    pump(1500); // started, its hotkeys registered
    const bool ours = chordOurs(kActivationKey);
    report.insert(u"hotkey_registered"_s, ours);
    check(ours, "the clipboard's hotkey is not registered");
    if (!ok)
        return finish();

    bool savedOk = false;
    const std::vector<Item> before = saveClipboard(savedOk);
    check(savedOk, "the clipboard could not be read to be put back");
    if (!ok)
        return finish();
    bool changed = false; // the user's clipboard replaced by the paste: to be put back
    waitForQuietInput();
    for (const WORD key : {static_cast<WORD>(VK_ESCAPE), static_cast<WORD>(VK_RETURN)}) {
        QJsonObject round;
        const auto done = qScopeGuard([&] { report.insert(key == VK_ESCAPE ? u"escape"_s : u"enter"_s, round); });
        bringToFront(target);
        QUERY_USER_NOTIFICATION_STATE state {};
        const bool exclusive = waitFor([&] {
            return inFront(target) && SUCCEEDED(::SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
        }, 6000);
        pump(500); // the change of display mode over
        round.insert(u"game_in_front"_s, inFront(target));
        round.insert(u"game_exclusive"_s, exclusive);
        check(exclusive, "the game is not in front in exclusive full screen (no key sent)");
        if (!ok)
            break;
        pressChord(kActivationKey);
        QQuickWindow* window = nullptr;
        const auto panel = [&] {
            for (QWindow* candidate : QGuiApplication::topLevelWindows()) {
                if (candidate->isVisible() && QByteArray(candidate->metaObject()->className()).startsWith("ClipboardWindow_"))
                    window = qobject_cast<QQuickWindow*>(candidate);
            }
            return window && ::GetForegroundWindow() == reinterpret_cast<HWND>(window->winId());
        };
        QElapsedTimer opened;
        opened.start();
        const bool tookFocus = waitFor(panel, 4000);
        round.insert(u"focus_taken_ms"_s, opened.elapsed());
        round.insert(u"focus_taken"_s, tookFocus);
        check(tookFocus, "the clipboard did not take the focus over the game");
        if (!ok)
            break;
        const auto hwnd = reinterpret_cast<HWND>(window->winId());
        auto* clipboard = qobject_cast<Clipboard*>(window->property("clipboard").value<QObject*>());
        pump(2500); // the game out of full screen meanwhile, the display mode changed
        const bool stayed = window->isVisible() && ::GetForegroundWindow() == hwnd;
        round.insert(u"stayed_in_front"_s, stayed);
        round.insert(u"keys_routed"_s, clipboard && clipboard->keysRouted());
        round.insert(u"game_minimized"_s, static_cast<bool>(::IsIconic(target)));
        ::SHQueryUserNotificationState(&state);
        round.insert(u"game_left_exclusive"_s, state != QUNS_RUNNING_D3D_FULL_SCREEN);
        check(stayed, "the clipboard did not stay up in front as the game left full screen");
        check(clipboard && !clipboard->keysRouted(), "the keys are routed although it has the focus");
        if (!ok)
            break;
        window->grabWindow().save(output.filePath(key == VK_ESCAPE ? u"game-escape.png"_s : u"game-enter.png"_s));
        if (::GetForegroundWindow() != hwnd) // checked again just before the key
            break;
        changed = changed || key == VK_RETURN;
        pressKey(key);
        const bool back = waitFor([&] { return !window->isVisible() && inFront(target) && !::IsIconic(target); }, 6000);
        round.insert(u"panel_hidden"_s, !window->isVisible());
        round.insert(u"game_back_in_front"_s, back);
        check(back, key == VK_ESCAPE ? "Esc did not give the game the front back" : "Enter did not give the game the front back");
        pump(1500); // the paste keys after it is in front
        if (key == VK_RETURN) {
            const QString text = QGuiApplication::clipboard()->text();
            round.insert(u"clipboard_text"_s, text);
            check(text == kFocusOther, "Enter did not put the entry on the clipboard to paste");
        }
        if (!ok)
            break;
    }
    if (changed) {
        const bool restored = putOnClipboard(before) || putOnClipboard(before);
        report.insert(u"clipboard_restored"_s, restored);
        check(restored, "the clipboard as it was could not be put back");
    }
    return finish();
}

} // namespace ws::preview
