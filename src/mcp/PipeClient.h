#pragma once

// The client end of Win顺's named pipe, without Qt: the name, and lines in
// and out. ws.exe (src/cli, static C runtime) builds this file in; so does
// wsmcp_proto (Qt's C runtime) for the relay WinShunMcp.exe, whose
// pipe::Client (Pipe.h) adds QByteArray lines on top. One source file in
// both, never one library: the two C runtimes do not link together (LNK2038).
//
// The pipe is the user's own: its name carries the user's SID (pipe names are
// machine-wide, so two users each get their own); Pipe.h has its security.

#include <windows.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>

namespace ws::pipe {

// "S-1-5-21-…" of the user this process runs as; empty if it cannot be read.
std::wstring userSid();
// "WinShun.Mcp.S-1-5-21-…": the name Win顺 serves on and the clients open.
std::wstring defaultName();
std::wstring fullPath(std::wstring_view name); // "\\.\pipe\" + name

inline constexpr std::size_t kMaxLineBytes = 16u << 20; // longer lines end the connection

// One end of the pipe, from the client side: connects to the server, writes
// lines, reads lines. Reads block (one thread reads); writes may come from
// any thread, while a read waits; close() ends a blocked read.
class LineClient {
public:
    LineClient();
    ~LineClient();
    LineClient(const LineClient&) = delete;
    LineClient& operator=(const LineClient&) = delete;

    // Opens `name` (without the \\.\pipe\ prefix), waiting at most `timeoutMs`
    // for a free instance when the server is busy. False when there is no
    // server (ERROR_FILE_NOT_FOUND) or it cannot be opened; `error` has the
    // Win32 code.
    bool connect(std::wstring_view name, DWORD timeoutMs, DWORD* error = nullptr);
    bool connected() const noexcept;
    void close();
    // Ends a read (or write) waiting on another thread, which returns false;
    // the pipe stays open. For Ctrl+C in ws.
    void interrupt() noexcept;

    // A line with its newline; false once the pipe is closed or broken.
    bool write(std::string_view line);
    // The next line (without its newline); false at the end. `idleMs`: how
    // long the pipe may stay silent while a line is awaited (the time since the
    // last bytes came, not a total); then the read is given up, false is
    // returned and timedOut() says why. INFINITE: as long as it takes.
    bool readLine(std::string& line, DWORD idleMs = INFINITE);
    // The last readLine() ended because the pipe stayed silent for idleMs.
    bool timedOut() const noexcept { return m_timedOut; }

    // The process that serves the other end of the pipe; 0 when it cannot be told.
    DWORD serverProcessId() const noexcept;

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE; // overlapped
    bool m_timedOut = false;
    HANDLE m_readEvent; // one read at a time, one write at a time
    HANDLE m_writeEvent;
    std::mutex m_writeMutex;
    std::string m_buffer; // read, not yet returned
    std::size_t m_scanned = 0; // the bytes of m_buffer already searched for a newline
};

} // namespace ws::pipe
