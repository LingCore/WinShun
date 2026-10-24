#pragma once

// The text of one file, for whoever asks to read it whole (an AI client
// through MCP, a plugin with content:read): a document's (Word, Excel,
// PowerPoint, PDF) as the content index keeps it, or read out now by the
// extractor when the index has none; a text file's, decoded from the disk.
//
// Reading goes to the disk and may wait for an extractor: call it from a
// worker thread, never the GUI's.

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ws {

class ContentIndex;
class DocExtractor;
class FileIndex;

namespace filetext {

enum class Status {
    Ok,
    Missing, // no such file
    Folder,
    NotText, // a binary file
    TooLarge, // more bytes than `maxBytes`
    CloudOnly, // a cloud file not on this computer: reading it would download it
    Unreadable, // in use, no access, or the extractor failed on it
};

struct Result {
    Status status = Status::Unreadable;
    // The whole text. A document's has "[page 3]", "[slide 2]" and
    // "[sheet Name]" lines where those begin.
    QString text;
    bool document = false;
    bool fromIndex = false; // the content index had it: no file was opened
    bool cut = false; // the file has more than was read (the text limit)
};

struct Sources {
    std::shared_ptr<const FileIndex> index; // may be null
    std::shared_ptr<const ContentIndex> content; // may be null
    DocExtractor* extractor = nullptr; // null: documents the index has no text of are Unreadable
};

// `path`: full, native ("C:\dir\file.docx"). `maxBytes`: the largest file to
// read (a document's extraction; a text file is read up to its first 16 MB
// regardless).
Result read(const std::wstring& path, const Sources& sources, std::int64_t maxBytes, const std::function<bool()>& cancelled);
// The same for a file opened already, with any access (FILE_FLAG_BACKUP_
// SEMANTICS lets a folder be one), whose final path is `path` (for the
// content index and the kind): read through that handle, so what a caller
// checked is what is read, whatever the path is made to lead to meanwhile.
Result read(void* file, const std::wstring& path, const Sources& sources, std::int64_t maxBytes, const std::function<bool()>& cancelled);

// The start of a text file, for looking at it (the launcher's preview): its
// first `maxBytes` decoded as the content search decodes it (BOM, else valid
// UTF-8, else the ANSI code page: GBK on Chinese Windows), line ends made
// "\n". Cut at the last whole line that fits (or the last whole character,
// when one line is longer than that). Status Ok, NotText (a NUL near the
// start: a binary file), Missing, Folder, CloudOnly or Unreadable; never
// reads a cloud file that is not on this computer.
enum class HeadEncoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Ansi };
struct Head {
    Status status = Status::Unreadable;
    QString text;
    HeadEncoding encoding = HeadEncoding::Utf8;
    unsigned codePage = 0; // Ansi: which (936 for GBK)
    bool cut = false; // the file goes on past `text`
    std::uint64_t size = 0; // the file's, in bytes
};
Head readHead(const std::wstring& path, std::size_t maxBytes);
// The same on bytes in memory (`total`: how long the file is; more than
// bytes.size() means they are only its start).
Head decodeHead(std::string_view bytes, std::uint64_t total);

} // namespace filetext
} // namespace ws
