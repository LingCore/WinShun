// The part of winshun.explorer-tabs that watches File Explorer and moves new
// folder windows into tabs (the rules are Rules.h). Two threads of its own:
//
//  - the watch thread holds an out-of-context WinEvent hook for SHOW and
//    never makes a COM call, so it sees a new window within a millisecond
//    or two and can hide it (layered, alpha 0) before its folder is known;
//  - the shell thread (an STA) holds IShellWindows, hears WindowRegistered
//    and WindowRevoked, keeps a cache of every Explorer tab (so a new one is
//    found by looking at the end of the list, not by asking about all of
//    them), decides, opens the tab (0xA21B), navigates it, closes B.
//
// Each window it hides is written to a journal in the data folder before it
// is hidden; whatever is in the journal at the next start is shown again
// (the process can be ended without a word: the job, a crash).
#pragma once

#include "Rules.h"

#include <functional>
#include <memory>
#include <string>

namespace tabs {

struct Options {
    Settings settings;
    Restriction restriction; // dev only (WINSHUN_TABS_TEST_ROOT)
    std::wstring testMark; // dev only: a window whose title has it may be hidden at SHOW (WINSHUN_TABS_TEST_MARK)
    bool forceBypass = false; // dev only: as if Ctrl+Shift were held (WINSHUN_TABS_BYPASS)
    bool failNavigation = false; // dev only: every navigation of the new tab fails (WINSHUN_TABS_FAIL)
    std::wstring journal; // <dataDir>\hidden.txt
    std::wstring trace; // dev only: timings, one line per window (WINSHUN_TABS_TRACE)
    std::function<void(const std::string&)> log;
};

class Engine {
public:
    explicit Engine(Options options);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start();
    void stop(); // shows again whatever it hid
    void configure(const Settings& settings);
    void setPaused(bool paused);
    bool paused() const;
    int merged() const; // windows turned into tabs since the start

    // Shows the windows a journal lists (left hidden by a process that ended).
    static int restoreJournal(const std::wstring& journal);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace tabs
