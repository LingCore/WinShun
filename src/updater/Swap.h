#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ws::update {

// A portable copy's new files put in place (WinShunUpdate.exe --portable).
// `stage` holds the new version as the zip had it (Updater unpacked and
// checked it, beside the program on the same volume), `app` the program
// folder. Only the names the new version has are replaced: what else the
// folder holds (the user's "data" above all, or files of their own) is
// not touched. Every move is a rename on one volume, so rolling back is
// moving the same names back.
struct SwapPlan {
    std::wstring app;
    std::wstring stage;
    std::wstring backup; // made here; the old version's replaced entries
};

struct SwapMoves {
    std::vector<std::wstring> backedUp; // names moved app -> backup
    std::vector<std::wstring> placed; // names moved stage -> app
};

using Log = std::function<void(const std::wstring&)>;

// The top-level names in `stage` to put in place: everything but "data".
std::vector<std::wstring> stagedNames(const std::wstring& stage);

// Each name's old entry to the backup, then the new one in. False when a
// move failed: what was done is rolled back first. `failAfter` (tests):
// pretend the move after that many fails.
bool swapIn(const SwapPlan& plan, SwapMoves& moves, const Log& log, int failAfter = -1);
// The new entries back to the stage, the old ones back from the backup, in
// reverse. False when something could not be moved back (logged).
bool rollBack(const SwapPlan& plan, const SwapMoves& moves, const Log& log);

// Deletes a folder with everything in it; read-only files too. False when
// something was left.
bool removeTree(const std::wstring& folder);

} // namespace ws::update
