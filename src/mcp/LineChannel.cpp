#include "LineChannel.h"

#include "Pipe.h"

#include <vector>

namespace ws::pipe {

LineChannel::LineChannel(HANDLE handle, Callbacks callbacks)
    : m_handle(handle)
    , m_stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_queueEvent(::CreateEventW(nullptr, FALSE, FALSE, nullptr))
    , m_callbacks(std::move(callbacks))
{
}

LineChannel::~LineChannel()
{
    if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr)
        ::CloseHandle(m_handle);
    ::CloseHandle(m_stopEvent);
    ::CloseHandle(m_queueEvent);
}

void LineChannel::run()
{
    OVERLAPPED readOverlapped {};
    readOverlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED writeOverlapped {};
    writeOverlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<char> chunk(64 * 1024);
    QByteArray pending; // read, not yet a whole line
    QByteArray writing; // the line being written
    bool readPending = false;
    bool writePending = false;
    bool broken = false;

    bool first = true;
    const auto handleRead = [&](DWORD got) {
        pending.append(chunk.data(), static_cast<qsizetype>(got));
        if (first) {
            first = false;
            if (m_callbacks.firstData)
                m_callbacks.firstData();
        }
        qsizetype at = 0;
        while ((at = pending.indexOf('\n')) >= 0) {
            QByteArray line = pending.left(at);
            if (line.endsWith('\r'))
                line.chop(1);
            pending.remove(0, at + 1);
            if (m_callbacks.line && !line.isEmpty())
                m_callbacks.line(std::move(line));
        }
        if (static_cast<std::size_t>(pending.size()) > kMaxLineBytes)
            broken = true; // a line that never ends
    };

    while (!broken) {
        if (!readPending) {
            ::ResetEvent(readOverlapped.hEvent);
            DWORD got = 0;
            if (::ReadFile(m_handle, chunk.data(), static_cast<DWORD>(chunk.size()), &got, &readOverlapped)) {
                if (got == 0) {
                    broken = true;
                    break;
                }
                handleRead(got);
                continue;
            }
            if (::GetLastError() != ERROR_IO_PENDING) {
                broken = true;
                break; // closed or broken
            }
            readPending = true;
        }
        if (!writePending) {
            if (writing.isEmpty()) {
                const std::lock_guard lock(m_mutex);
                if (!m_queue.empty()) {
                    writing = std::move(m_queue.front());
                    m_queue.pop_front();
                }
            }
            if (!writing.isEmpty()) {
                ::ResetEvent(writeOverlapped.hEvent);
                DWORD put = 0;
                if (::WriteFile(m_handle, writing.constData(), static_cast<DWORD>(writing.size()), &put, &writeOverlapped)) {
                    writing.clear();
                    continue; // the next line, if any
                }
                if (::GetLastError() != ERROR_IO_PENDING) {
                    broken = true;
                    break;
                }
                writePending = true;
            }
        }
        // Stop, the read, and either the write in flight or a line queued.
        const HANDLE events[] = {m_stopEvent, readOverlapped.hEvent, writePending ? writeOverlapped.hEvent : m_queueEvent};
        const DWORD which = ::WaitForMultipleObjects(3, events, FALSE, INFINITE);
        if (which == WAIT_OBJECT_0 + 1) {
            DWORD got = 0;
            readPending = false;
            if (!::GetOverlappedResult(m_handle, &readOverlapped, &got, FALSE) || got == 0) {
                broken = true;
                break;
            }
            handleRead(got);
        } else if (which == WAIT_OBJECT_0 + 2) {
            if (writePending) {
                DWORD put = 0;
                writePending = false;
                if (!::GetOverlappedResult(m_handle, &writeOverlapped, &put, FALSE)) {
                    broken = true;
                    break;
                }
                writing.clear();
            }
            // else the queue has a line: the loop picks it up
        } else {
            break; // stop(), or a wait failure
        }
    }

    ::CancelIoEx(m_handle, nullptr);
    DWORD unused = 0;
    if (readPending)
        ::GetOverlappedResult(m_handle, &readOverlapped, &unused, TRUE);
    if (writePending)
        ::GetOverlappedResult(m_handle, &writeOverlapped, &unused, TRUE);
    // A named-pipe flush waits for the peer to read every buffered byte.
    // stop() must also return when that peer is hung or no longer reading.
    ::CloseHandle(readOverlapped.hEvent);
    ::CloseHandle(writeOverlapped.hEvent);
    const std::lock_guard lock(m_mutex);
    m_ended = true;
    m_broken = broken;
    m_queue.clear();
}

void LineChannel::send(QByteArray line)
{
    {
        const std::lock_guard lock(m_mutex);
        if (m_ended)
            return;
        m_queue.push_back(std::move(line));
    }
    ::SetEvent(m_queueEvent);
}

void LineChannel::stop()
{
    ::SetEvent(m_stopEvent);
}

} // namespace ws::pipe
