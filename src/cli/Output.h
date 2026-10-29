#pragma once

// Where ws writes (docs/design-cli.md §4). stdout carries only data, stderr
// every hint. On a console: WriteConsoleW (right in any code page), colours
// through virtual-terminal sequences. Elsewhere: bytes in the code page
// Encoding.h picks, no colours unless asked, LF line ends, nothing more once
// the reader has gone (`ws … | head`).

#include "Encoding.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ws::cli {

enum class Style : std::uint8_t {
    Plain,
    Bold,
    Dim, // the folder part of a path, hints
    Name, // a file's name
    FolderName, // a folder's name
    Match, // the words of the query
    GrepPath,
    GrepPlace, // line, page, slide, row
    Error, // "ws:" before an error
};

enum class ColorMode { Auto, Always, Never };

class Stream {
public:
    // `handle`: a standard handle (none is fine: nothing is written).
    // `codePage`: for when it is not a console.
    Stream(HANDLE handle, UINT codePage);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    bool console() const noexcept { return m_console; }
    bool colored() const noexcept { return m_colored; }
    void setColored(bool on) noexcept { m_colored = on; }
    UINT codePage() const noexcept { return m_encoder.codePage(); }
    // Characters the code page lacks as \uXXXX, for JSON (nothing lost).
    void setJsonEscapes(bool on) noexcept { m_encoder.setJsonEscapes(on); }
    // The other stream, written out first whenever this one writes, so that
    // both keep their order where they meet (a console, `2>&1`).
    void setPartner(Stream* partner) noexcept { m_partner = partner; }

    void write(std::wstring_view text, Style style = Style::Plain);
    // UTF-8 text (JSON): as it is when the output is UTF-8.
    void writeUtf8(std::string_view text);
    void newline();
    void nul();
    void flush();

    // Writing stopped: the reader went away (a closed pipe) or the write failed
    // (a full disk); the caller may stop too.
    bool broken() const noexcept { return m_broken; }
    // Why a write failed (GetLastError), unless the reader merely went away
    // (`ws ... | head`: ERROR_NO_DATA, ERROR_BROKEN_PIPE, ERROR_PIPE_NOT_CONNECTED),
    // which is no error to report; 0 when there is none.
    DWORD writeError() const noexcept { return m_writeError; }
    // Lines with a character the code page could not hold ('?' instead), the
    // line still being written included.
    std::size_t lossyLines() const noexcept { return m_lossyLines + (m_lineLossy ? 1 : 0); }

private:
    void put(std::wstring_view text);
    void countLoss(std::wstring_view text, std::size_t lost);
    void fail(DWORD error) noexcept;
    void pending();
    void writeOut(const void* data, std::size_t bytes);

    HANDLE m_handle;
    bool m_console = false;
    bool m_colored = false;
    Encoder m_encoder;
    Stream* m_partner = nullptr;
    std::wstring m_wide; // console: waiting to be written
    std::string m_bytes; // otherwise
    bool m_lineLossy = false; // the line being written has one already
    std::size_t m_lossyLines = 0; // finished lines with one
    bool m_broken = false;
    DWORD m_writeError = 0;
};

// The console's output modes with virtual-terminal processing on (colours,
// erasing a progress line), put back as they were when ws ends.
class ConsoleModes {
public:
    ConsoleModes();
    ~ConsoleModes();
    ConsoleModes(const ConsoleModes&) = delete;
    ConsoleModes& operator=(const ConsoleModes&) = delete;

    bool vt(HANDLE handle) const noexcept;
    // Puts the modes back now (Ctrl+C's way out, which skips destructors).
    static void restoreNow() noexcept;
};

// Whether colours go to `stream`: --color, NO_COLOR, a console that takes
// them.
bool wantColor(ColorMode mode, bool console, bool vt);

} // namespace ws::cli
