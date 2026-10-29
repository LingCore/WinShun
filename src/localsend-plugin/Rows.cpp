#include "Rows.h"

#include "Util.h"

#include <windows.h>

namespace localsend {

namespace {

std::string pick(bool chinese, const char* zh, const char* en)
{
    return chinese ? zh : en;
}

struct Kind {
    const char* type;
    const char* zh;
    const char* en;
    const char* repeats[3]; // models that only say the kind again
};

// LocalSend's device types; one it does not know is a computer (its default).
constexpr Kind kKinds[] = {
    {"mobile", "手机", "Phone", {"mobile", "phone", nullptr}},
    {"desktop", "电脑", "Computer", {"desktop", "computer", "pc"}},
    {"web", "网页", "Web", {"web", "browser", nullptr}},
    {"headless", "命令行工具", "Command line", {"cli", "headless", nullptr}},
    {"server", "服务器", "Server", {"server", nullptr, nullptr}},
};

std::wstring withoutSlash(std::wstring folder)
{
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/'))
        folder.pop_back();
    return folder;
}

} // namespace

std::string filesWords(std::size_t files, bool chinese)
{
    return chinese ? std::to_string(files) + " 个文件" : std::to_string(files) + (files == 1 ? " file" : " files");
}

std::string excerpt(const std::string& text, std::size_t chars)
{
    std::wstring w = wide(text);
    for (wchar_t& c : w) {
        if (c < 0x20 || c == 0x7F)
            c = L' ';
        else if (directionMark(c))
            c = L'_';
    }
    if (w.size() <= chars)
        return narrow(w);
    std::wstring t = w.substr(0, chars);
    if (!t.empty() && IS_HIGH_SURROGATE(t.back()))
        t.pop_back();
    return narrow(t + L"…");
}

std::string titled(const std::string& words, const std::string& who)
{
    return words + " · " + excerpt(trimmed(who), kNameInTitle);
}

bool toastOpens(const std::wstring& folder)
{
    if (folder.size() < 3 || folder[1] != L':' || (folder[2] != L'\\' && folder[2] != L'/'))
        return false;
    const wchar_t drive = folder[0];
    if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')))
        return false;
    return folder.find(L':', 2) == std::wstring::npos;
}

std::string textKey(const std::string& text)
{
    return text.empty() ? std::string() : sha256Hex(text.data(), text.size()).substr(0, 16);
}

std::string folderWords(const std::wstring& folder, const std::wstring& downloads, bool chinese)
{
    const std::wstring f = withoutSlash(folder);
    const std::wstring d = withoutSlash(downloads);
    if (f.empty() || (!d.empty() && ::CompareStringOrdinal(f.c_str(), -1, d.c_str(), -1, TRUE) == CSTR_EQUAL))
        return pick(chinese, "下载", "Downloads");
    if (f.size() <= 3 && f.size() >= 2 && f[1] == L':')
        return narrow(f.substr(0, 2)); // a drive's root: "D:"
    const std::size_t slash = f.find_last_of(L"\\/");
    return narrow(slash == std::wstring::npos ? f : f.substr(slash + 1));
}

std::string deviceWords(const DeviceInfo& device, bool chinese)
{
    const Kind* kind = &kKinds[1];
    for (const Kind& k : kKinds) {
        if (device.deviceType == k.type)
            kind = &k;
    }
    const std::string words = chinese ? kind->zh : kind->en;
    const std::string model = cutUtf8(trimmed(device.deviceModel), 40);
    if (model.empty())
        return words;
    for (const char* repeat : kind->repeats) {
        if (repeat && equalsNoCase(model, repeat))
            return words;
    }
    return words + " · " + model;
}

void LastReceived::finished(const ReceiveOutcome& outcome, Clock::time_point now)
{
    if (outcome.saved.empty())
        return; // nothing came of it: the row before stays
    Row r;
    r.age = Age::Fresh;
    r.files = outcome.saved.size();
    r.sender = outcome.sender.alias;
    r.folder = outcome.folder;
    r.at = now;
    m_last = std::move(r);
}

LastReceived::Row LastReceived::row(Clock::time_point now) const
{
    if (!m_last || now - m_last->at >= kKeepOutcome)
        return {};
    Row r = *m_last;
    r.age = now - r.at < kFresh ? Age::Fresh : Age::Older;
    return r;
}

std::optional<SentRow> sentRow(const SendOutcome& o, bool chinese)
{
    using Kind = SendOutcome::Kind;
    const std::string& who = o.device;
    SentRow r;
    // Not taken, for now: Enter sends the same again.
    const auto again = [&](std::string title, const char* zhWhy, const char* enWhy) {
        r.title = std::move(title);
        r.subtitle = chinese ? std::string(zhWhy) + " · Enter 重发" : std::string(enWhy) + " · Enter sends it again";
        r.enterTitle = pick(chinese, "重发", "Send again");
        return std::optional<SentRow>(std::move(r));
    };
    switch (o.kind) {
    case Kind::Sent:
    case Kind::TextSent:
        r.title = o.kind == Kind::TextSent ? (chinese ? who + " 收到了文字" : who + " got the text")
                                           : (chinese ? who + " 收下了 " + filesWords(o.files, true) : who + " took " + filesWords(o.files, false));
        r.subtitle = (o.kind == Kind::Sent && o.bytes ? sizeText(o.bytes) + " · " : std::string())
            + pick(chinese, "Enter 再发一次", "Enter sends it once more");
        r.enterTitle = pick(chinese, "再发一次", "Send once more");
        return r;
    case Kind::Declined:
        return again(chinese ? who + " 没有接收" : who + " declined", "对方点了拒绝", "They said no");
    case Kind::Busy:
        return again(chinese ? who + " 正忙" : who + " is busy", "对方正在收别的文件", "It is receiving something else");
    case Kind::Paused:
        // Win顺 there: a no a moment ago (it waits a while before asking again), or a text just now.
        if (o.text)
            return again(chinese ? who + " 刚收到过文字，等几秒再发" : who + " just got a text; wait a few seconds", "过几秒", "In a few seconds");
        return again(chinese ? who + " 刚拒绝过，等一会儿再发" : who + " just said no; wait a moment", "过一会儿", "In a moment");
    case Kind::NoAnswer:
        return again(chinese ? who + " 没有回应" : "No answer from " + who, "3 分钟没人点接收", "Nobody accepted in 3 minutes");
    case Kind::PinRefused:
        return again(chinese ? "没有发给 " + who + "：对方要密码" : "Not sent to " + who + ": it wants a PIN", "没有输入密码",
            "No PIN was typed");
    case Kind::TooManyPins:
        return again(chinese ? who + " 说密码错了太多次" : who + " says too many PINs were wrong", "等一会儿", "Wait a while");
    case Kind::Unreachable:
        return again(chinese ? "连不上 " + who : "Could not reach " + who, "对方还开着 LocalSend 吗？", "Is LocalSend still open there?");
    case Kind::Interrupted:
        return again(chinese ? "没发完：发了 " + std::to_string(o.files) + " 个就断了" : "Not all sent: " + std::to_string(o.files) + " went, then it stopped",
            "对方取消了，或者网络断了", "Cancelled there, or the network dropped");
    case Kind::NotTheDevice:
        r.title = chinese ? "没有发给 " + who + "：那个地址上现在是别的设备" : "Not sent to " + who + ": another device is there now";
        r.subtitle = pick(chinese, "为了安全没有发 · Enter 再找一次", "Nothing was sent, to be safe · Enter looks again");
        r.enter = SentRow::Enter::Search;
        r.enterTitle = pick(chinese, "再找一次", "Look again");
        return r;
    case Kind::NoRoom:
        // The disk there may be freed or plugged back in: the same again is worth a try.
        return again(chinese ? who + " 存不下" : "No room on " + who, "对方空间不够，或者保存位置不可用",
            "Not enough space there, or where it saves cannot be reached");
    case Kind::TooMany:
        // The same files again would be refused again: Enter only puts the row away.
        r.title = chinese ? who + " 一次收不了这么多文件" : who + " takes fewer files at once";
        r.subtitle = pick(chinese, "分几次发，每次少选一些 · Enter 知道了", "Send them in a few goes, fewer each time · Enter: OK");
        r.enter = SentRow::Enter::Dismiss;
        r.enterTitle = pick(chinese, "知道了", "OK");
        return r;
    case Kind::Nothing:
    case Kind::Cancelled:
        break;
    }
    return std::nullopt;
}

} // namespace localsend
