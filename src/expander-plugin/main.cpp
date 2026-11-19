// WinShunExpander.exe: the winshun.expander plugin in the plugin library
// (package/, "缩写展开", GitHub #403; not in the setup). A resident C++
// plugin on the single-header SDK (src/plugins-sdk/winshun.h), started with
// Win顺 and kept running: type an abbreviation anywhere and the key after it
// puts its phrase in its place (Expander.h reads them, Engine.cpp types).
// The keyword 缩写 / expander lists them, says which lines are not used and
// why, and pauses.

#include "winshun.h"

#include "Engine.h"

#include <memory>
#include <string>

using winshun::Item;
using winshun::Items;
using winshun::Json;
using winshun::Query;

namespace {

constexpr const char* kIcon = "ph:textbox";
constexpr const char* kPauseIcon = "ph:pause";
constexpr const char* kPlayIcon = "ph:play";
constexpr const char* kEditIcon = "ph:pencil-simple";
constexpr const char* kProblemIcon = "ph:warning";
constexpr std::size_t kTitleMost = 80; // UTF-16 units of a phrase shown on its row

expander::Phrases phrasesOf(const Json& s)
{
    return expander::parse(winshun::wide(s["phrases"].toString()));
}

expander::Settings settingsFrom(const Json& s)
{
    expander::Settings out;
    out.phrases = phrasesOf(s).list;
    out.instant = s["instant"].isBool() && s["instant"].toBool();
    out.excluded = expander::programList(winshun::wide(s["exclude"].toString()));
    return out;
}

// The text on one line: its lines with ↵ between them, cut short.
std::wstring oneLine(std::wstring_view text)
{
    std::wstring out;
    for (const wchar_t c : text) {
        if (out.size() >= kTitleMost) {
            out += L'…';
            break;
        }
        if (c == L'\n')
            out += L" ↵ ";
        else
            out += c == L'\t' ? L' ' : c;
    }
    return out;
}

std::wstring lowered(std::wstring_view text)
{
    std::wstring out(text);
    if (!out.empty())
        ::CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

std::string problemText(const expander::Problem& p, bool zh)
{
    using Kind = expander::Problem::Kind;
    const std::string line = std::to_string(p.line);
    const std::string a = winshun::utf8(p.abbreviation);
    const std::string where = zh ? "第 " + line + " 行没用上：" : "Line " + line + " is not used: ";
    switch (p.kind) {
    case Kind::NoEquals:
        return where + (zh ? "没有“=”。每行写一条：缩写 = 内容；没有“=”的行接在上一条后面，算它的下一行"
                           : "no “=”. One a line: abbreviation = text; a line without “=” goes on the one above as its next line");
    case Kind::NoAbbreviation:
        return where + (zh ? "“=”前面没有缩写" : "nothing before the “=”");
    case Kind::NoText:
        return where + (zh ? a + " 后面没有要换成的内容" : a + " has no text to become");
    case Kind::Spaces:
        return where + (zh ? "缩写 " + a + " 里有空格，打的时候空格就让它展开了" : "the abbreviation " + a + " has a space, which ends it as it is typed");
    case Kind::TooLong:
        return where + (zh ? "缩写最长 " + std::to_string(expander::kLongest) + " 个字符"
                           : "an abbreviation has " + std::to_string(expander::kLongest) + " characters at most");
    case Kind::NotTyped:
        return where + (zh ? "缩写 " + a + " 里有中文或全角字符，输入法打出来的字这里看不到；用字母、数字或符号"
                           : "the abbreviation " + a + " has Chinese, Japanese, Korean or full-width characters, which an input method types out of sight; use letters, digits or symbols");
    case Kind::Repeated:
        return where + (zh ? a + " 前面已经有了，用前面那条" : a + " is there already; the first one counts");
    }
    return where;
}

class Expander : public winshun::Plugin {
protected:
    std::string name() const override { return "winshun.expander"; }

    void initialized() override
    {
        expander::EngineOptions o;
        o.log = [this](const std::string& text) { log("warning", text); };
        m_engine = std::make_unique<expander::Engine>(std::move(o));
        m_engine->configure(settingsFrom(settings()));
        if (!dataDir().empty()) {
            ::CreateDirectoryW(winshun::wide(dataDir()).c_str(), nullptr);
            m_pauseFile = winshun::wide(dataDir()) + L"\\paused";
            if (::GetFileAttributesW(m_pauseFile.c_str()) != INVALID_FILE_ATTRIBUTES)
                m_engine->setPaused(true);
        }
        m_engine->start();
    }

    void settingsChanged(const Json& s) override
    {
        if (m_engine)
            m_engine->configure(settingsFrom(s));
    }

    void shuttingDown() override
    {
        if (m_engine)
            m_engine->stop();
    }

    Items query(const Query& q) override
    {
        const bool zh = chinese();
        const expander::Phrases phrases = phrasesOf(settings());
        const bool paused = m_engine && m_engine->paused();
        const bool instant = settings()["instant"].isBool() && settings()["instant"].toBool();
        Items items;

        Item status("status", "");
        if (paused) {
            status.title = zh ? "缩写展开已暂停" : "Text expander paused";
            status.subtitle = zh ? "Enter 恢复" : "Enter to resume";
            status.icon = kPlayIcon;
        } else if (phrases.list.empty()) {
            status.title = zh ? "还没有缩写" : "No abbreviations yet";
            status.subtitle = zh ? "Enter 去写几条：每行一条，如 cmd = command" : "Enter to write some: one a line, such as cmd = command";
            status.icon = kEditIcon;
            status.open = Json::Object {{"url", "winshun:settings"}};
        } else {
            const std::string count = std::to_string(phrases.list.size());
            const std::string done = std::to_string(m_engine ? m_engine->expanded() : 0);
            status.title = zh ? (instant ? "打完缩写就换成内容 · " : "打出缩写再按空格或标点，就换成内容 · ") + count + " 条"
                              : (instant ? "An abbreviation becomes its text as it is typed · " : "Type an abbreviation, then a space or punctuation · ") + count;
            status.subtitle = zh ? "Win顺 启动后展开了 " + done + " 次 · 展开后马上按退格可以撤回 · Enter 暂停"
                                 : "Expanded " + done + " time(s) since WinShun started · Backspace right after takes it back · Enter to pause";
            status.icon = kPauseIcon;
        }
        items.push_back(status);

        const std::wstring filter = lowered(winshun::wide(q.text));
        if (filter.empty() && !phrases.list.empty()) {
            Item edit("edit", zh ? "编辑缩写" : "Edit the abbreviations", zh ? "设置 › 插件 › 缩写展开" : "Settings › Plugins › Text expander");
            edit.icon = kEditIcon;
            edit.open = Json::Object {{"url", "winshun:settings"}};
            items.push_back(edit);
        }
        for (const expander::Problem& p : phrases.problems) {
            Item row("problem-" + std::to_string(p.line), problemText(p, zh), zh ? "Enter 去改" : "Enter to change it");
            row.icon = kProblemIcon;
            row.open = Json::Object {{"url", "winshun:settings"}};
            items.push_back(row);
        }
        for (const expander::Phrase& p : phrases.list) {
            if (!filter.empty() && lowered(p.abbreviation).find(filter) == std::wstring::npos && lowered(p.text).find(filter) == std::wstring::npos)
                continue;
            Item row("phrase-" + std::to_string(p.line), winshun::utf8(p.abbreviation + L"  →  " + oneLine(p.text)),
                     zh ? "Enter 复制内容" : "Enter to copy the text");
            row.icon = kIcon;
            row.data = Json::Object {{"text", winshun::utf8(p.text)}};
            row.copy = winshun::utf8(p.text); // Ctrl+C as well
            items.push_back(row);
        }
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        const bool zh = chinese();
        winshun::Effects effects;
        const std::string id = a.item["id"].toString();
        if (id == "status" && m_engine) {
            const bool pause = !m_engine->paused();
            m_engine->setPaused(pause);
            if (!m_pauseFile.empty()) {
                if (pause) {
                    if (const HANDLE f = ::CreateFileW(m_pauseFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        f != INVALID_HANDLE_VALUE)
                        ::CloseHandle(f);
                } else {
                    ::DeleteFileW(m_pauseFile.c_str());
                }
            }
            effects.push_back(winshun::effect::notify(pause ? (zh ? "缩写展开已暂停" : "Text expander paused")
                                                            : (zh ? "缩写展开已恢复" : "Text expander resumed")));
        } else if (id.rfind("phrase-", 0) == 0) {
            effects.push_back(winshun::effect::copy(a.item["data"]["text"].toString()));
            effects.push_back(winshun::effect::notify(zh ? "已复制" : "Copied"));
        }
        effects.push_back(winshun::effect::hide());
        return effects;
    }

private:
    std::unique_ptr<expander::Engine> m_engine;
    std::wstring m_pauseFile; // <dataDir>\paused: there while paused (survives a restart)
};

} // namespace

int main()
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Expander plugin;
    return plugin.serve();
}
