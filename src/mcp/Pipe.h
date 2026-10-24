#pragma once

// The named pipe between Win顺's clients and Win顺: WinShunMcp.exe (the relay
// the AI clients start, with the user's normal rights) and ws.exe (the
// command line). Messages are lines of JSON-RPC (JsonRpc.h). The name and the
// client's core are Qt-free (PipeClient.h); this adds the server's security
// and QByteArray lines, for the relay, Win顺 and the tests.
//
// The pipe's DACL lets only its user, administrators and the system open it,
// and its label is medium integrity, so that a process of medium integrity
// can connect to an object made by an elevated one, while low-integrity
// processes (browser sandboxes) cannot.

#include "PipeClient.h"

#include <QByteArray>

namespace ws::pipe {

// The security descriptor described above; null if it cannot be made. Free
// with ::LocalFree.
PSECURITY_DESCRIPTOR createSecurityDescriptor();

// LineClient with QByteArray lines.
class Client : public LineClient {
public:
    using LineClient::readLine;
    using LineClient::write;

    // A line with its newline; false once the pipe is closed or broken.
    bool write(const QByteArray& line)
    {
        return LineClient::write(std::string_view(line.constData(), static_cast<std::size_t>(line.size())));
    }
    // The next line (without its newline); false at the end.
    bool readLine(QByteArray& line)
    {
        std::string read;
        if (!LineClient::readLine(read))
            return false;
        line = QByteArray(read.data(), static_cast<qsizetype>(read.size()));
        return true;
    }
};

} // namespace ws::pipe
