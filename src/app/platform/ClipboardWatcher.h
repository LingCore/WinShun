#pragma once

#include "ClipImage.h"
#include "ClipStore.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ws {

// What a paste puts on the clipboard.
struct ClipWrite {
    QString text; // CF_UNICODETEXT
    QByteArray html; // "HTML Format", as it was copied
    QByteArray rtf; // "Rich Text Format"
    QStringList files; // CF_HDROP, always as a copy (never moving files that were cut)
    QString imagePath; // original encoded image, plus PNG/DIB compatibility formats
    // ... or the image itself, for a picture with no file yet (a screen capture:
    // the history writes its file later).
    QByteArray imageData;
    QString imageOrigin; // opaque identity written with these pixels, even without history
    // An equation's or an embedded object's own formats (packClipFormats()),
    // put first, in the order the program that copied it put them: Word and
    // WPS paste from these what was copied, the picture is for the rest.
    QByteArray formats;
    // The history entry this is: when it reaches the clipboard, that entry
    // moves to the top instead of a new one being added. 0 for several
    // entries joined, which are not recorded at all.
    qint64 id = 0;

    bool empty() const
    {
        return text.isEmpty() && files.isEmpty() && imagePath.isEmpty() && imageData.isEmpty() && formats.isEmpty();
    }
};

// Watches the clipboard for the history, and writes it for pastes.
//
// Runs on a thread of its own with a message-only window that listens for
// clipboard changes (AddClipboardFormatListener). Reading what another
// program copied can make that program render it first (Excel draws a
// picture of the cells, Word converts to RTF); that, and turning pictures
// into PNG, never holds up the GUI.
//
// Skipped: what password managers and Windows mark as not for clipboard
// history or monitoring (ExcludeClipboardContentFromMonitorProcessing,
// CanIncludeInClipboardHistory = 0, Clipboard Viewer Ignore), copies from
// the excluded programs, and what this class wrote itself (see
// ClipWrite::id).
//
// An equation (Word offers MathML for one copied alone) or an embedded
// object (Equation 3.0, MathType, WPS's 公式编辑器) is recorded as a
// picture, drawn from its metafile if there is no other, together with the
// formats that paste it back as one (ClipCapture::formats; GitHub #58) and
// its text, if it has some, which the search finds it by.
class ClipboardWatcher {
public:
    struct Options {
        bool record = false; // off: nothing is read (writing still works)
        bool images = true;
        QStringList excludedApps; // program file names, "KeePass.exe"
    };
    // Both run on the watcher's thread; post to the GUI thread from them.
    struct Callbacks {
        std::function<void(ClipCapture)> captured;
        std::function<void(qint64 id)> reused; // our write of history entry `id` is on the clipboard
        std::function<void(qint64 bytes)> skippedTooBig; // a copy too big to keep
    };

    ClipboardWatcher(Options options, Callbacks callbacks);
    ~ClipboardWatcher();

    ClipboardWatcher(const ClipboardWatcher&) = delete;
    ClipboardWatcher& operator=(const ClipboardWatcher&) = delete;

    void setOptions(Options options); // any thread
    bool running() const noexcept { return m_running.load(); }
    bool listening() const noexcept { return m_listening.load(); }
    // Bounded in-memory metadata, copied into an explicitly exported diagnostic
    // bundle. No clipboard payload, content hash, paths or arbitrary format names.
    QJsonObject diagnosticStatus() const; // any thread, never opens the clipboard
    // Puts `data` on the clipboard, on the watcher's thread; `done` runs
    // there with whether that worked.
    void write(ClipWrite data, std::function<void(bool)> done);

private:
    struct WriteJob;
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void read();
    bool writeNow(const ClipWrite& data);
    bool openClipboard(DWORD* lastError = nullptr, int* attempts = nullptr);
    void startWorker(bool captureInitial); // m_lifecycleMutex held
    void configureWatching(HWND window); // on the worker
    bool recordingEnabled();
    QString programName(const std::wstring& path); // its description, "微信"

    Callbacks m_callbacks;
    std::mutex m_mutex;
    Options m_options; // guarded by m_mutex
    mutable std::mutex m_diagnosticsMutex;
    QJsonArray m_recentReads; // guarded by m_diagnosticsMutex
    qint64 m_readAttempts = 0; // guarded by m_diagnosticsMutex
    std::mutex m_lifecycleMutex; // starting, posting writes, and retiring an idle writer
    std::atomic<HWND> m_window {nullptr};
    std::atomic<bool> m_running {false};
    std::atomic<bool> m_listening {false};
    std::atomic<unsigned> m_pendingWrites {0};
    std::thread m_thread;
    // Only touched on the watcher's thread:
    DWORD m_lastSequence = 0;
    DWORD m_startSequence = 0; // the clipboard's when watching began: what was there already
    int m_readRetries = 0;
    std::unordered_map<std::wstring, QString> m_programNames;
};

} // namespace ws
