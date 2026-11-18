#pragma once

#include <windows.h>

#include <QString>
#include <QStringList>

#include <optional>

namespace ws {

class FileIndex;

// The folder a File Explorer view shows that another program hosts:
// 360文件夹 (folderview::Hit::hosted). Windows' Explorer browser control in
// its own process, so the shell's windows do not have it, and the program
// tells nobody which folder its tab is on: no automation of its own, an
// address bar it draws itself (measured on 1.0.0.1220). What it shows does
// tell: its title is the folder's name, and its items view (UI Automation)
// lists what is in it; the index has the folders of that name and what is
// in each (shownfolder::pick).
namespace hostedexplorer {

struct Shown {
    QString title; // the window's: the name of the folder its tab shows
    QStringList names; // the first items listed there, as shown (an extension may be hidden)
};

// What the window `frame` shows in its tab `control` (the browser control).
// UI Automation, which waits on that program: on a COM thread of its own,
// never the GUI's.
Shown shown(HWND frame, HWND control);

// The folder of `shown`: nullopt when it cannot be told; empty for 此电脑
// (no folder, searched as the whole PC as File Explorer's This PC is). A
// drive by its name ("Windows (C:)"), Windows' own folders by theirs (桌面,
// 文档…), any other through `index` (may be null: those are not told).
std::optional<QString> place(const Shown& shown, const FileIndex* index);

} // namespace hostedexplorer
} // namespace ws
