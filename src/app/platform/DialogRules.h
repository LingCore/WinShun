#pragma once

#include "FileDialog.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <windows.h>

#include <string>
#include <vector>

// File windows that programs draw themselves (ArcGIS's "Add Data", GitHub
// #272): neither a Windows file dialog nor one of the few Win顺 knows by name.
// The user describes such a window in dialogs.json in the data folder -- which
// program, which window, which box takes a path, what confirms it -- and it
// is then driven like the Windows XP style dialog: a folder (or a file's
// whole path) goes into that box and is confirmed there.
//
// Everything is done through UI Automation, so one description fits Win32,
// WPF, WinForms and Qt windows alike; nothing is loaded into the other
// program and no keys are synthesised. Enter, where a rule asks for it, is a
// window message to the box that has just been given the focus and the path.
//
// A rule never applies to a window FileDialog.cpp knows already.
namespace ws::dialogrules {

// One control of the window, by what UI Automation tells about it. Every
// field given must match; `index` picks among several that do.
struct Selector {
    std::wstring type; // "edit", "combobox", "button", "" any
    std::wstring id; // AutomationId (a Win32 control's ID as a number)
    std::wstring name;
    std::wstring className;
    int index = 0;

    bool given() const { return !type.empty() || !id.empty() || !name.empty() || !className.empty(); }
    bool operator==(const Selector&) const = default;
};

struct Rule {
    std::wstring name; // the user's own, for the log
    std::wstring program; // "arcmap.exe": the file name, lower case
    std::wstring windowClass; // "#32770", "HwndWrapper[ArcGISPro*": '*' for any run of characters; "" any
    std::wstring title; // likewise
    filedialog::Kind kind = filedialog::Kind::Open; // Open, Save or Folder: what the bar offers
    Selector box; // takes the path
    Selector confirm; // a button; not given: Enter in the box

    bool operator==(const Rule&) const = default;
};

// {"dialogs": [{"program": "ArcMap.exe", "class": "#32770", "title": "添加数据",
//   "box": {"type": "edit", "name": "名称:"}, "confirm": "enter"}]}
// Entries that cannot be used (no program, no box, an unknown kind) are left
// out and told in `problems`, one line each.
std::vector<Rule> parse(const QByteArray& json, QStringList* problems = nullptr);
QJsonObject toJson(const Rule& rule);

// "a*b" against text, case-insensitive; an empty pattern matches anything.
bool matches(std::wstring_view pattern, std::wstring_view text);

// The rules in use (any thread).
void setRules(std::vector<Rule> rules);
bool any();

// Whether one of the rules names this window: its program, class and title.
// No call into the program; any thread.
bool isOne(HWND window);
filedialog::Kind kind(HWND window);

// These wait on the program (UI Automation): on a worker thread with COM.
bool goTo(HWND window, const std::wstring& folder); // an existing folder, typed with a trailing backslash
bool setFileName(HWND window, const std::wstring& path); // into the box, which gets the focus
bool accept(HWND window);

// A rule for `window` as it is now: its program, class and title, and the
// box most likely meant (the one with the keyboard focus if it takes text,
// else the lowest one that does). `seen` lists every box and button there,
// for a rule that guessed wrong. An empty program: nothing to describe.
struct Seen {
    Rule rule;
    QStringList boxes; // "edit  id=1152  name=名称:  class=Edit"
    QStringList buttons;
};
Seen describe(HWND window);

} // namespace ws::dialogrules
