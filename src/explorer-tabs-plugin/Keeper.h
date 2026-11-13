// Keeps the open File Explorer windows and their tabs on disk and opens them
// again (GitHub #273; what is kept and what gets opened: Session.h). One
// thread of its own, an STA:
//
//  - every few seconds it compares what the windows look like (handles,
//    titles, places on screen: no call into Explorer) with the last look,
//    and only after a change that has settled asks Explorer where each tab
//    is (and, for a window with several, in which order its tab strip has
//    them: UI Automation) and writes the file. Never while Windows is
//    shutting down or the shell is gone: the windows closing then are not
//    the user's doing;
//  - at the first start after a logon, when asked to (the setting), and
//    whenever the user asks (the keyword's second row), it opens what the
//    logon before left and is not open by now: a window for the first tab,
//    Explorer's own new-tab command for the others.
//
// While it opens windows the engine is held, so they stay windows.
#pragma once

#include "Rules.h"
#include "Session.h"

#include <functional>
#include <memory>
#include <string>

namespace tabs {

struct KeeperOptions {
    std::wstring file; // <dataDir>\session.txt
    bool restoreAtLogon = false; // the setting
    Restriction restriction; // dev only: windows with a tab outside the root are not there
    bool asIfLogon = false; // dev only: this start counts as the first after a logon (WINSHUN_TABS_LOGON)
    std::function<void(bool)> hold; // the engine: new folder windows stay windows meanwhile
    std::function<void(const std::string&)> log;
};

class Keeper {
public:
    explicit Keeper(KeeperOptions options);
    ~Keeper();
    Keeper(const Keeper&) = delete;
    Keeper& operator=(const Keeper&) = delete;

    void start();
    void stop();

    // What the logon before left (for the keyword's row).
    struct Left {
        int windows = 0;
        int tabs = 0;
    };
    Left left() const;
    bool restoring() const;
    // Opens what the logon before left and is not open; returns at once.
    void restore();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace tabs
