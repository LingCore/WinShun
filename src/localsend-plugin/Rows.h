#pragma once

// What the 传 rows say about transfers, apart from the plugin so that
// tests/tst_localsend.cpp can look at it: the last batch that came in (the
// 收到了 row), the words for a device and a folder, and the row once a send
// of ours ended, with what Enter does on it. Plain words, Chinese or English.

#include "Protocol.h"
#include "Sender.h"
#include "Server.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace localsend {

inline constexpr auto kFresh = std::chrono::minutes(2); // texts and outcomes this new come first (after what Enter sends)
inline constexpr auto kKeepOutcome = std::chrono::minutes(30); // the 收到了 row goes then

std::string filesWords(std::size_t files, bool chinese); // "3 个文件", "3 files"
// At most `chars` characters of a peer's words on one line, "…" when cut:
// nothing in them breaks the line or turns it around.
std::string excerpt(const std::string& text, std::size_t chars);
// A notification's title: our words first, " · ", then who (cut to
// kNameInTitle): a peer's name can never read as the start of a message
// from Windows or Win顺. Win顺 puts "局域网传输：" in front (inside the 63
// characters it keeps), so no second colon: "局域网传输：想发给你 1 个文件 · 手机".
inline constexpr std::size_t kNameInTitle = 20;
std::string titled(const std::string& words, const std::string& who);
// Whether a notification's click may open `folder`: Win顺 opens only a drive
// letter's path from one (src/plugin/ToastPolicy.h, plainLocalPath), never a
// share (\\server\…). For a share the click opens 传, whose 收到了 row opens
// the folder (Enter).
bool toastOpens(const std::wstring& folder);
// What the clipboard's text was when a row offered to send it (Ctrl+Enter
// sends it only while it is still that): "" for none.
std::string textKey(const std::string& text);
// "下载" for Downloads (or no folder chosen), else the folder's own name:
// never a whole path.
std::string folderWords(const std::wstring& folder, const std::wstring& downloads, bool chinese);
// What kind of device, and its model unless that only says the kind again
// (the LocalSend CLI is "headless", model "CLI"): "手机 · Pixel 8", "命令行工具".
std::string deviceWords(const DeviceInfo& device, bool chinese);

// The 收到了 row: the last transfer that left files here. One that left
// none (declined, unanswered, no space, cancelled before a file was whole)
// keeps the row before (the live test of GitHub #113 lost it so).
class LastReceived {
public:
    using Clock = std::chrono::steady_clock;
    enum class Age { None, Fresh, Older };
    struct Row {
        Age age = Age::None; // None: no row
        std::size_t files = 0;
        std::string sender; // the alias
        std::wstring folder;
        Clock::time_point at;
    };

    void finished(const ReceiveOutcome& outcome, Clock::time_point now);
    Row row(Clock::time_point now) const; // Fresh for kFresh, then Older, gone after kKeepOutcome

private:
    std::optional<Row> m_last;
};

// The row once a send of ours ended; none for one cancelled here or with
// nothing to send. Enter sends the same again, looks for devices, or (the
// same would be refused again) puts the row away.
struct SentRow {
    enum class Enter { Resend, Search, Dismiss };
    std::string title;
    std::string subtitle; // ends with what Enter does
    Enter enter = Enter::Resend;
    std::string enterTitle; // the action's own words: "重发", "再发一次", "再找一次", "知道了"
};
std::optional<SentRow> sentRow(const SendOutcome& outcome, bool chinese);

} // namespace localsend
