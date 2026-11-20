#pragma once

// This PC's clipboard for the engine: what is copied here (a message-only
// window told by AddClipboardFormatListener, on a thread of its own), and
// what comes from the other devices put on it.
//
// Not sent on: what the engine itself wrote (the private format
// "ShunSync.Origin" is on it), and what a password manager marks as not for
// clipboard watchers (ExcludeClipboardContentFromMonitorProcessing,
// CanIncludeInClipboardHistory = 0, CanUploadToCloudClipboard = 0,
// "Clipboard Viewer Ignore").
//
// Read in this order: files copied in Explorer (CF_HDROP), text
// (CF_UNICODETEXT), a picture ("PNG" as browsers and Office put it, else the
// bitmap encoded as PNG). Written: text as CF_UNICODETEXT with "\r\n"; a
// picture as "PNG" and a CF_DIB on white (for programs that know no
// transparency); files as CF_HDROP with "Preferred DropEffect" copy.

#include "Engine.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace shunsync {

struct Copied {
    enum class Kind { Text, Image, Files } kind = Kind::Text;
    std::string text; // UTF-8
    Bytes png;
    int width = 0;
    int height = 0;
    std::vector<std::wstring> files;
};

class ClipboardWin final : public ClipboardSink {
public:
    using CopiedHandler = std::function<void(Copied)>;
    ClipboardWin(CopiedHandler copied, std::function<void(const std::string&)> log);
    ~ClipboardWin() override;
    ClipboardWin(const ClipboardWin&) = delete;
    ClipboardWin& operator=(const ClipboardWin&) = delete;

    bool start();
    void stop();
    void setWatching(bool on); // off: nothing copied here is told (paused)

    // What the clipboard holds now (Enter on a member: "send it this"),
    // the engine's own writes too; nothing when it holds nothing to send or
    // a password manager's secret.
    std::optional<Copied> current();

    bool putText(const std::string& text, const std::string& originDeviceId) override;
    bool putImage(const Bytes& png, const std::string& originDeviceId) override;
    bool putFiles(const std::vector<std::wstring>& paths, const std::string& originDeviceId) override;
    // The invite code: pasted as text anywhere, but kept out of clipboard
    // histories, the cloud clipboard and every watcher, this one too.
    bool putSecret(const std::string& text);

private:
    struct Job {
        std::function<bool()> work;
        std::promise<bool> done;
    };
    void run(std::promise<bool> started);
    bool onWindow(std::function<bool()> work); // on the window's thread, waited for
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void changedNow();
    std::optional<Copied> read(bool skipOurs, bool* busy);
    bool write(const std::vector<std::pair<UINT, HGLOBAL>>& formats, const std::string& originDeviceId);

    CopiedHandler m_copied;
    std::function<void(const std::string&)> m_log;
    std::thread m_thread;
    HWND m_hwnd = nullptr;
    DWORD m_threadId = 0;
    UINT m_originFormat = 0;
    UINT m_pngFormat = 0;
    UINT m_dropEffectFormat = 0;
    UINT m_excludeFormat = 0;
    UINT m_historyFormat = 0;
    UINT m_cloudFormat = 0;
    UINT m_ignoreFormat = 0;
    std::mutex m_mutex;
    std::deque<std::shared_ptr<Job>> m_jobs;
    bool m_watching = true;
    DWORD m_ownSequence = 0; // after our last write
    DWORD m_lastSequence = 0; // the last one read
    int m_retries = 0;
};

// The size in a PNG's header; {0, 0} when it is not one.
std::pair<int, int> pngSize(std::string_view png);

} // namespace shunsync
