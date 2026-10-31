// WinShunBreak.exe: the winshun.break plugin in the plugin library (package/,
// "休息提醒", WinShun-plugins #7; not in the setup). A resident C++ plugin on the
// single-header SDK (src/plugins-sdk/winshun.h), started with Win顺 and kept
// running: every `interval` minutes a short break, every `longEvery`-th one
// long. The clock is Schedule.cpp, what it reads from Windows Sense.cpp, the
// notice and the break screen Ui.cpp (their own thread). The keyword 休息
// shows how long until the next break and offers a break now, a pause, a
// new count.

#include "winshun.h"

#include "Core.h"
#include "Sense.h"
#include "Ui.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

using winshun::Item;
using winshun::Items;
using winshun::Json;
using winshun::Query;

namespace {

constexpr const char* kStatusIcon = "glyph:U+E916"; // Stopwatch
constexpr const char* kEyeIcon = "glyph:U+E7B3"; // RedEye
constexpr const char* kWalkIcon = "glyph:U+E805"; // Walk
constexpr const char* kPauseIcon = "glyph:U+E769"; // Pause
constexpr const char* kPlayIcon = "glyph:U+E768"; // Play
constexpr const char* kRestartIcon = "glyph:U+E72C"; // Refresh

std::int64_t number(const Json& settings, const char* key, double fallback, double low, double high)
{
    const Json& v = settings[key];
    double d = v.isNumber() ? v.toNumber() : fallback;
    if (v.isString()) {
        char* end = nullptr;
        const double parsed = std::strtod(v.toString().c_str(), &end);
        if (end && end != v.toString().c_str())
            d = parsed;
    }
    return static_cast<std::int64_t>(std::clamp(d, low, high));
}

brk::Config configFrom(const Json& s)
{
    brk::Config c;
    c.intervalMs = number(s, "interval", 20, 1, 240) * 60'000;
    c.shortMs = number(s, "shortSeconds", 20, 5, 600) * 1'000;
    c.longEvery = static_cast<int>(number(s, "longEvery", 2, 0, 20));
    c.longMs = number(s, "longMinutes", 5, 1, 60) * 60'000;
    c.headsUpMs = number(s, "headsUp", 30, 0, 300) * 1'000;
    c.snoozeMs = number(s, "snooze", 5, 1, 60) * 60'000;
    c.resetIdleMs = number(s, "away", 5, 1, 120) * 60'000;
    const std::string strict = s["strict"].toString();
    c.strict = strict == "all" ? brk::Strict::All : strict == "long" ? brk::Strict::LongOnly : brk::Strict::Off;
    // For trying it out: WINSHUN_BREAK_FAST=1 turns minutes into seconds.
    if (winshun::env("WINSHUN_BREAK_FAST") == "1") {
        c.intervalMs /= 60;
        c.longMs /= 60;
        c.snoozeMs /= 60;
        c.resetIdleMs /= 60;
    }
    return c;
}

std::string span(std::int64_t ms, bool zh)
{
    const std::int64_t s = (ms + 999) / 1000;
    if (s < 60)
        return std::to_string(s) + (zh ? " 秒" : " s");
    const std::int64_t m = (s + 59) / 60;
    if (m < 60)
        return std::to_string(m) + (zh ? " 分钟" : " min");
    const std::int64_t h = m / 60;
    const std::int64_t rest = m % 60;
    if (zh)
        return std::to_string(h) + " 小时" + (rest ? " " + std::to_string(rest) + " 分钟" : "");
    return std::to_string(h) + " h" + (rest ? " " + std::to_string(rest) + " min" : "");
}

std::string clockTime(std::int64_t wallMs)
{
    const std::time_t t = static_cast<std::time_t>(wallMs / 1000);
    std::tm local {};
    localtime_s(&local, &t);
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%02d:%02d", local.tm_hour, local.tm_min);
    return buffer;
}

std::string lower(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool matches(const std::string& title, const std::string& words)
{
    return words.empty() || lower(title).find(lower(words)) != std::string::npos; // "break pause": Pause for 1 hour
}

class Break : public winshun::Plugin {
public:
    Break() : m_ui(m_core) {}

protected:
    std::string name() const override { return "winshun.break"; }

    void initialized() override
    {
        m_core.chinese = chinese();
        m_core.light = theme() != "dark";
        m_core.log = [this](const std::string& text) { log("info", text); };
        apply(settings());
        loadPause();
        m_ui.start();
    }

    void settingsChanged(const Json& s) override
    {
        apply(s);
        m_ui.wake();
    }

    void appearanceChanged() override
    {
        {
            const std::lock_guard lock(m_core.mutex);
            m_core.chinese = chinese();
            m_core.light = theme() != "dark";
        }
        m_ui.wake();
    }

    void shuttingDown() override { m_ui.stop(); }

    Json queryResult(const Query& q) override
    {
        // While the launcher shows the rows, the countdown in them moves.
        return Json::Object {{"items", winshun::itemsJson(query(q))}, {"rerun", 1}};
    }

    Items query(const Query& q) override
    {
        const bool zh = chinese();
        const std::int64_t now = brk::Core::now();
        const std::int64_t wall = brk::Core::wall();
        brk::State st;
        brk::Config c;
        {
            const std::lock_guard lock(m_core.mutex);
            st = m_core.schedule.state(now, wall);
            c = m_core.schedule.config();
        }
        const std::string shortLength = span(c.shortMs, zh);
        const std::string longLength = span(c.longMs, zh);
        const std::string nextName = st.next == brk::Kind::Long ? (zh ? "大休" : "long break") : (zh ? "小休" : "short break");

        Items items;
        Item status("status", "");
        status.icon = kStatusIcon;
        switch (st.phase) {
        case brk::Phase::Paused:
            status.title = zh ? "休息提醒已暂停，" + clockTime(st.pausedUntil) + " 恢复" : "Paused until " + clockTime(st.pausedUntil);
            status.subtitle = zh ? "Enter 现在恢复" : "Enter to resume now";
            break;
        case brk::Phase::OnBreak:
            status.title = zh ? "正在" + nextName + "，还剩 " + span(st.breakLeftMs, zh) : "On a " + nextName + ", " + span(st.breakLeftMs, zh) + " left";
            status.valid = false;
            break;
        case brk::Phase::Pending:
            status.title = zh ? "马上开始" + nextName : "A " + nextName + " is starting";
            status.valid = false;
            break;
        default:
            if (st.waiting)
                status.title = zh ? nextName + "等全屏程序或勿扰结束" : "The " + nextName + " waits for full screen or Do not disturb to end";
            else
                status.title = zh ? "下次" + nextName + "在 " + span(st.untilDueMs, zh) + "后" : "Next " + nextName + " in " + span(st.untilDueMs, zh);
            status.subtitle = zh ? "已经连续用了 " + span(st.workedMs, zh) + " · Enter 现在就休息"
                                 : "At the PC for " + span(st.workedMs, zh) + " · Enter for a break now";
            break;
        }
        items.push_back(status);

        const std::string& words = q.text;
        const auto add = [&](const char* id, const std::string& title, const std::string& subtitle, const char* icon) {
            if (!matches(title, winshun::trim(words)))
                return;
            Item item(id, title);
            item.subtitle = subtitle;
            item.icon = icon;
            items.push_back(item);
        };
        if (st.phase == brk::Phase::Paused) { // resumed first (Schedule: no break, no new count while paused)
            add("resume", zh ? "恢复休息提醒" : "Resume break reminders", "", kPlayIcon);
            return items;
        }
        if (st.phase != brk::Phase::OnBreak) {
            add("short", zh ? "现在小休 " + shortLength : "Short break now (" + shortLength + ")",
                zh ? "看看远处，眨眨眼" : "Look far away and blink", kEyeIcon);
            add("long", zh ? "现在大休 " + longLength : "Long break now (" + longLength + ")",
                zh ? "起来走走，之后重新算" : "Get up and walk; the count starts over after it", kWalkIcon);
        }
        add("pause1", zh ? "暂停 1 小时" : "Pause for 1 hour", zh ? "开会、看电影时用" : "For a meeting or a film", kPauseIcon);
        add("pause3", zh ? "暂停 3 小时" : "Pause for 3 hours", "", kPauseIcon);
        add("restart", zh ? "重新计时" : "Start the count again",
            zh ? "从现在起重新算 " + span(c.intervalMs, zh) : "The next break " + span(c.intervalMs, zh) + " from now", kRestartIcon);
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        const bool zh = chinese();
        const std::string id = a.item["id"].toString();
        const std::int64_t now = brk::Core::now();
        const std::int64_t wall = brk::Core::wall();
        std::string said;
        bool pauseChanged = false;
        {
            const std::lock_guard lock(m_core.mutex);
            brk::Schedule& s = m_core.schedule;
            if (id == "status") {
                if (s.phase() == brk::Phase::Paused) {
                    s.resume(now);
                    pauseChanged = true;
                } else
                    s.breakNow(s.state(now, wall).next, now);
            } else if (id == "short")
                s.breakNow(brk::Kind::Short, now);
            else if (id == "long")
                s.breakNow(brk::Kind::Long, now);
            else if (id == "pause1" || id == "pause3") {
                const std::int64_t hours = id == "pause1" ? 1 : 3;
                s.pause(wall + hours * 3'600'000);
                said = zh ? "休息提醒暂停到 " + clockTime(wall + hours * 3'600'000) : "Paused until " + clockTime(wall + hours * 3'600'000);
                pauseChanged = true;
            } else if (id == "resume") {
                s.resume(now);
                said = zh ? "休息提醒已恢复" : "Break reminders resumed";
                pauseChanged = true;
            } else if (id == "restart") {
                s.restart(now);
                said = zh ? "重新开始计时" : "The count starts again";
            }
        }
        if (pauseChanged)
            savePause();
        m_ui.wake();
        winshun::Effects effects;
        if (!said.empty())
            effects.push_back(winshun::effect::notify(said));
        effects.push_back(winshun::effect::hide());
        return effects;
    }

private:
    void apply(const Json& s)
    {
        const brk::Config c = configFrom(s);
        const std::lock_guard lock(m_core.mutex);
        m_core.quietRule = !s["quiet"].isBool() || s["quiet"].toBool();
        m_core.schedule.configure(c, brk::Core::now());
    }

    // A pause outlives a restart of Win顺 (and of the PC): it is kept in the
    // plugin's data folder as the wall-clock time it ends.
    std::wstring pauseFile() const { return winshun::wide(dataDir()) + L"\\pause.txt"; }

    void loadPause()
    {
        if (dataDir().empty())
            return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, pauseFile().c_str(), L"rb") != 0 || !f)
            return;
        long long until = 0;
        const int got = fscanf_s(f, "%lld", &until);
        std::fclose(f);
        if (got == 1 && until > brk::Core::wall()) {
            const std::lock_guard lock(m_core.mutex);
            m_core.schedule.pause(until);
        }
    }

    void savePause()
    {
        if (dataDir().empty())
            return;
        std::int64_t until = 0;
        {
            const std::lock_guard lock(m_core.mutex);
            const brk::State st = m_core.schedule.state(brk::Core::now(), brk::Core::wall());
            until = st.phase == brk::Phase::Paused ? st.pausedUntil : 0;
        }
        ::CreateDirectoryW(winshun::wide(dataDir()).c_str(), nullptr);
        FILE* f = nullptr;
        if (_wfopen_s(&f, pauseFile().c_str(), L"wb") == 0 && f) {
            std::fprintf(f, "%lld\n", static_cast<long long>(until));
            std::fclose(f);
        }
    }

    brk::Core m_core;
    brk::Ui m_ui;
};

} // namespace

int main()
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Break plugin;
    return plugin.serve();
}
