#pragma once

// What AI clients may read and change of Win顺's settings (the MCP tools
// get_settings and change_settings, src/mcp). One table, default-deny: a
// setting that has no entry here is invisible to AI, and one that is not
// marked writable can be read and not changed. A field added to Settings
// later is therefore out of AI's reach until someone adds its entry.
//
// Nothing here knows the app: what only the app can tell (is Total
// Commander installed, who owns a keyword, is the shortcut one Windows
// accepts) comes in through Env, so the table and its validation run in the
// tests as they are. What a change does to the running app (hotkeys
// registered, Explorer restarted) is App's business (App::applyAiSettings).

#include "Settings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <functional>
#include <variant>
#include <vector>

namespace ws::aisettings {

enum class Type { Bool, Choice, Int, Number, Text, Hotkey, List, WebShortcuts };
enum class Access {
    Hidden, // not listed, not changeable; asking for it is as asking for a key that does not exist
    ReadOnly, // listed, not changeable
    Writable,
    AlwaysAsk, // changeable, but the user is asked even when AI may change settings by itself
};
// When a change shows more to AI than before (what it keeps out is let in, a
// recording is switched on), the user is asked even when AI may change
// settings by itself.
enum class Widens {
    No,
    Shrink, // a list of what is kept out: taking items out widens
    TurnOn, // a switch: turning it on widens
    Grow, // a list of what is let in: putting items in widens
};
// What the user still has to do before the change shows.
enum class Effect { Now, ExplorerRestart, AppRestart };
// What the items of a list are, and so how one is written and kept.
// LocalFolders: folders on this PC's disks, no network paths (网络共享 has those).
enum class ListKind { Programs, Extensions, ContentExtensions, Names, FileNames, Folders, LocalFolders };

// The settings as a change works on them: those of Settings, and the one
// that lives elsewhere (autostart, a task in the Task Scheduler).
struct State {
    Settings settings;
    bool autostart = false;

    bool operator==(const State&) const = default;
};

// What only the app knows. Any left empty: no check (the tests').
struct Env {
    std::function<bool(const QString& hotkey)> hotkeyValid; // a shortcut Win顺 can register
    std::function<bool()> isAdministrator;
    std::function<bool()> isElevated; // Win顺 itself runs with administrator rights
    std::function<bool(const QString& manager)> fileManagerAvailable; // installed (explorer, custom: always)
    std::function<QString(const QString& keyword)> keywordOwner; // the favorite that has the keyword; empty: none
};

struct Choice {
    QString value; // as kept in the file
    QString meaning; // English, for the model
    const char* label; // Chinese, translated for the user ("AiSettings")
};

struct Entry {
    QString key; // the file's own "Section/Key"; "Launcher/Autostart" is no key of the file
    const char* title = nullptr; // what the settings page calls it (Chinese, translated for the user: "AiSettings")
    const char* rowTitle = nullptr; // ... where it differs from the settings page's row, for finding it there
    QString section; // see sections()
    int tab = -1; // of its page, for opening the page there
    QString about; // English, for the model: what it does, when to change it, what to know
    Type type = Type::Bool;
    Access access = Access::Writable;
    Effect effect = Effect::Now;

    std::vector<Choice> choices; // Choice
    double min = 0; // Int and Number: the range load() takes as it is
    double max = 0;
    const char* zeroLabel = nullptr; // Int: 0 is allowed too, shown as this (translated)
    QString zeroMeaning; // ... and as this to the model ("no limit")
    ListKind list = ListKind::Names; // List
    Widens widens = Widens::No;

    // Where its value is kept: a member of Settings (or of State, for
    // autostart), the one kind it is.
    struct SizeLimit {
        std::size_t kind; // ContentSizeLimits::Kind
    };
    using Field = std::variant<std::monostate, bool Settings::*, bool State::*, QString Settings::*, int Settings::*, SizeLimit,
        QStringList Settings::*, WebShortcuts Settings::*>;
    Field field;
    bool bound() const noexcept { return !std::holds_alternative<std::monostate>(field); }
    bool flag(const State& s) const;
    const QString& text(const State& s) const;
    int integer(const State& s) const;
    double number(const State& s) const;
    const QStringList& items(const State& s) const;
    const WebShortcuts& shortcuts(const State& s) const;
    void setFlag(State& s, bool value) const;
    void setText(State& s, const QString& value) const;
    void setInteger(State& s, int value) const;
    void setNumber(State& s, double value) const;
    void setItems(State& s, const QStringList& value) const;
    void setShortcuts(State& s, const WebShortcuts& value) const;
    // What else the settings window's own setter does with the value, once it
    // really changes.
    std::function<void(State&)> after;
    // More than the type says: an error for `value` (a Choice's or a Text's
    // or a Hotkey's, already normalised), or empty.
    std::function<QString(const State&, const QString& value, const Env&)> check;

    bool writable() const noexcept { return access == Access::Writable || access == Access::AlwaysAsk; }
    QString titleText() const; // translated
    QString rowTitleText() const; // as the settings page has it, translated; empty (rowTitle ""): not findable there
};

struct Section {
    QString id;
    const char* title; // Chinese, translated
    int page; // of the settings window (SettingsWindow.qml's pages)

    QString titleText() const; // translated
};

const std::vector<Section>& sections();
const Section* sectionOf(const QString& id);
const std::vector<Entry>& catalog();
// The entry for a key (any case), hidden ones too; null for a key that is none.
const Entry* find(const QString& key);
// Whether AI may see the entry at all.
inline bool visible(const Entry& e) noexcept { return e.access != Access::Hidden; }

// The value as AI reads it: true/false, a choice's value, a number, a text,
// an array of strings; for web shortcuts an array of {keyword, name, url,
// home, suggest, icon}. `state` is read, not changed.
QJsonValue valueOf(const Entry& entry, const State& state);

// ---- reading --------------------------------------------------------------

struct Query {
    QStringList sections; // ids (or titles); empty: all
    QStringList keys; // empty: all
    QString words; // all of them in the key, title, description or choices; Chinese or English
    bool changedOnly = false; // only what differs from the defaults
};

// {settings: [...], sections: [...]} for what is visible and fits the query.
QJsonObject describe(const State& current, const Query& query);

// ---- changing ---------------------------------------------------------------

// What one change comes to, as the user is shown it and AI is told.
struct Item {
    const Entry* entry = nullptr;
    QJsonValue oldValue;
    QJsonValue newValue;
    QString oldText; // translated, for the user: 开, 关, a choice's label, a list's items
    QString newText;
    // Lists and web shortcuts, what went and what came, each line short
    // ("还有 3 项" for the rest); else empty. diffText is the two lines, what went first.
    QString removedText; // "− c"
    QString addedText; // "+ a, b"
    QString diffText;
    bool changed = false; // false: it is so already
    bool askAlways = false; // the user is asked even when AI may change settings by itself
};

struct Plan {
    QStringList problems; // any: none of it is to be applied
    std::vector<Item> items; // in the order asked
    State result; // the state with every change made

    bool changed() const;
    bool askAlways() const;
};

// Checks `changes` (a JSON array of {key, value | add | remove}) against
// `current`, every one, and works out what they come to. Nothing is
// rejected by being clamped: a value out of range is a problem naming the
// range.
Plan makePlan(const State& current, const QJsonArray& changes, const Env& env);

// Puts `entry` back to `value` as valueOf gave it, no questions asked: for
// undoing a change that was made. `state` is changed.
void restore(const Entry& entry, State& state, const QJsonValue& value);

// The item as the host has it for the card and the answer (QML-friendly
// too): {key, title, section, old, new, old_text, new_text, removed_text,
// added_text, diff_text, changed, ask_always, effect}.
QJsonObject itemJson(const Item& item);

const char* effectName(Effect effect); // now | explorer_restart | app_restart

} // namespace ws::aisettings
