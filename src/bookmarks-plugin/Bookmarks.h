// The logic of WinShunBookmarks.exe (the winshun.bookmarks plugin): finding
// the Bookmarks files of Chromium browsers, reading them, and picking the
// bookmarks that have every word typed. Plain C++ on the plugin SDK, no Qt,
// so tests/tst_bookmarks.cpp builds it in again; main.cpp only reads the
// environment and prints the answer.

#pragma once

#include "winshun.h"

#include <string>
#include <vector>

namespace bm {

// One bookmark with the folder it sits in ("Bookmarks bar › Work") and the
// browser it came from.
struct Entry {
    std::string title;
    std::string url;
    std::string folder;
    std::string browser;
};

// Unicode lower case of a UTF-8 text (not ASCII only: "ÄÖ", Cyrillic ...).
std::string lower(const std::string& s);

// The URL with its %XX escapes decoded as UTF-8, like Python's
// unquote(errors="replace"): a bad sequence becomes U+FFFD, "+" stays "+".
// The list shows and matches this form ("…#安装"); Enter opens the URL as it is.
std::string unquote(const std::string& url);

// The typed text split at white space (ASCII, U+3000 and the other Unicode
// spaces) and lower-cased.
std::vector<std::string> words(const std::string& text);

// "Chrome", "Edge", "Brave" ... from the file's path; else a general name.
std::string browserOf(const std::string& path, bool chinese);

// Every bookmark in the text of a Bookmarks file (a UTF-8 BOM is skipped).
// Not JSON, or not a Bookmarks file: nothing.
std::vector<Entry> parseFile(const std::string& content, const std::string& browser);

// The file's whole content, opened with full sharing (the browser has it
// open); empty when it cannot be read. Never writes.
std::string readFile(const std::string& path);

// Chrome's and Edge's Bookmarks of every profile under `localAppData`, then
// `extraFile` when it is an existing file.
std::vector<std::string> bookmarkFiles(const std::string& localAppData, const std::string& extraFile);

// The rows for the typed `text`: at most 50, title matches first. When
// nothing is found, one invalid row says why (no files at all, or no match).
winshun::Items search(const std::vector<Entry>& entries, const std::string& text, bool chinese);

} // namespace bm
