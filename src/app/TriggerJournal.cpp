#include "TriggerJournal.h"

#include "DiagnosticLog.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

#include <string_view>
#include <utility>

#include <windows.h>
#include <psapi.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

std::uint32_t pageFaults()
{
    PROCESS_MEMORY_COUNTERS counters {};
    counters.cb = sizeof counters;
    return ::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof counters) ? counters.PageFaultCount : 0;
}

const char* sourceName(TriggerJournal::Source source)
{
    switch (source) {
    case TriggerJournal::Source::DoubleTap:
        return "double-tap";
    case TriggerJournal::Source::Hotkey:
        return "hotkey";
    case TriggerJournal::Source::WinV:
        return "win-v";
    case TriggerJournal::Source::ClipboardHotkey:
        return "clipboard-hotkey";
    case TriggerJournal::Source::Tray:
        return "tray";
    case TriggerJournal::Source::Command:
        return "command";
    }
    return "?";
}

const char* frontName(win::Front front)
{
    switch (front) {
    case win::Front::Already:
        return "already";
    case win::Front::Direct:
        return "direct";
    case win::Front::Input:
        return "input";
    case win::Front::Refused:
        return "refused";
    }
    return "?";
}

// Past these it is what the issue is about.
constexpr std::int64_t kSlowQueueMs = 100;
constexpr std::int64_t kSlowSeenMs = 150;
constexpr double kSlowFrontMs = 50;

} // namespace

void TriggerJournal::begin(Source source, std::uint64_t pressedAt, const Context& context)
{
    if (m_entry && m_entry->coming) {
        // Its window may still come: a press queued behind it (GitHub #111)
        // is decided before that window is up, and must not count as it
        // never having come.
        if (m_waiting)
            write(*std::exchange(m_waiting, std::nullopt));
        m_waiting = std::exchange(m_entry, std::nullopt);
    } else if (m_entry) {
        write(*std::exchange(m_entry, std::nullopt));
    }
    Entry entry;
    entry.source = source;
    entry.pressedAt = pressedAt;
    entry.receivedAt = ::GetTickCount64();
    entry.context = context;
    entry.faults = pageFaults();
    m_entry = entry;
}

void TriggerJournal::decided(const char* action, std::optional<Window> coming)
{
    if (!m_entry)
        return;
    m_entry->action = action;
    m_entry->coming = coming;
    if (!coming)
        write(*std::exchange(m_entry, std::nullopt));
}

void TriggerJournal::ignored(const char* reason, std::uint8_t facts)
{
    if (!m_entry)
        return;
    m_entry->action = "ignored";
    m_entry->ignored = reason;
    m_entry->facts = facts;
    write(*std::exchange(m_entry, std::nullopt));
}

void TriggerJournal::front(win::Front how, double ms)
{
    if (!m_entry || m_entry->front) // the first: the one that brought it
        return;
    m_entry->front = how;
    m_entry->frontMs = ms;
}

void TriggerJournal::seen(Window window)
{
    const std::uint64_t now = ::GetTickCount64();
    for (std::optional<Entry>* waiting : {&m_waiting, &m_entry}) { // the older first
        if (*waiting && (*waiting)->coming == window) {
            (*waiting)->seenAt = now;
            write(*std::exchange(*waiting, std::nullopt));
        }
    }
}

void TriggerJournal::end()
{
    if (m_waiting)
        write(*std::exchange(m_waiting, std::nullopt));
    if (m_entry)
        write(*std::exchange(m_entry, std::nullopt));
}

void TriggerJournal::write(const Entry& entry)
{
    const auto queueMs = static_cast<std::int64_t>(entry.receivedAt - entry.pressedAt);
    const std::optional<std::int64_t> seenMs = entry.seenAt
        ? std::optional(static_cast<std::int64_t>(*entry.seenAt - entry.pressedAt))
        : std::nullopt;
    // Raw Input "off" (no double tap set) is how it should be, not a fault.
    // The helper's word late, this process's own stood in for it.
    const std::string_view rawInput(entry.context.rawInput);
    const bool notable = queueMs > kSlowQueueMs || (seenMs && *seenMs > kSlowSeenMs)
        || (entry.coming && !seenMs) || entry.ignored || entry.front == win::Front::Refused
        || entry.frontMs > kSlowFrontMs || (rawInput != "ok" && rawInput != "off")
        || (entry.context.told
            && (std::string_view(entry.context.told->from) == "own-backup" || entry.context.told->queuedMs > 0));
    if (!m_budget.take(notable, ::GetTickCount64()))
        return;
    QJsonObject details {
        {u"source"_s, QString::fromLatin1(sourceName(entry.source))},
        {u"queue_ms"_s, static_cast<qint64>(queueMs)},
        {u"action"_s, QString::fromLatin1(entry.action ? entry.action : "none")},
        {u"trimmed"_s, entry.context.trimmed},
        {u"faults"_s, static_cast<qint64>(pageFaults() - entry.faults)},
        {u"helper"_s, entry.context.helper},
        {u"raw_input"_s, QString::fromLatin1(entry.context.rawInput)},
    };
    if (const std::optional<Told>& told = entry.context.told) {
        details.insert(u"tap_from"_s, QString::fromLatin1(told->from));
        details.insert(u"listener_ms"_s, static_cast<qint64>(told->listenerMs));
        if (told->queuedMs > 0)
            details.insert(u"queued_ms"_s, static_cast<qint64>(told->queuedMs));
    }
    if (entry.ignored) {
        details.insert(u"ignored"_s, QString::fromLatin1(entry.ignored));
        QStringList facts;
        for (const auto& [bit, name] : {std::pair {kCovers, "covers"}, std::pair {kExclusive, "exclusive"},
                 std::pair {kHidden, "hidden"}, std::pair {kConfined, "confined"}, std::pair {kCaptured, "captured"},
                 std::pair {kTyping, "typing"}}) {
            if (entry.facts & bit)
                facts.append(QString::fromLatin1(name));
        }
        details.insert(u"facts"_s, facts.join(u','));
    }
    if (entry.front) {
        details.insert(u"front"_s, QString::fromLatin1(frontName(*entry.front)));
        details.insert(u"front_ms"_s, qRound(entry.frontMs * 10) / 10.0);
    }
    if (seenMs)
        details.insert(u"seen_ms"_s, static_cast<qint64>(*seenMs));
    else if (entry.coming)
        details.insert(u"seen_ms"_s, QJsonValue()); // never came: hidden first, or another press
    if (m_budget.skipped() > 0)
        details.insert(u"skipped"_s, m_budget.skipped());
    diagnostics::record(u"trigger"_s, details);
}

} // namespace ws
