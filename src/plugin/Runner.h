#pragma once

// Win顺's end of the runner (WinShunPluginHost.exe, src/plugin/runner):
// starts it once with the user's normal rights, serves the pipe it
// connects to, and multiplexes plugin processes over it by instance
// number. Everything here runs on the thread that owns the object (the
// GUI thread); the pipe's thread hands lines over through the queue. The
// protocol is described in runner/main.cpp.

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <windows.h>

#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace ws::pipe {
class LineChannel;
}

namespace ws::plugin {

class Runner : public QObject {
    Q_OBJECT

public:
    // Whose token the runner got (the plugin design, §3.2; stage 0):
    // Explorer's (the user's session: network logons and all); our own
    // token restricted to medium integrity (no usable Explorer: the user
    // may not see network places the plugins should); our own as it is
    // (not elevated: we are the user); our own elevated one (the second
    // runner, for the plugins allowed to follow an elevated Win顺: Elevation.h).
    enum class TokenSource { None, Explorer, Restricted, Plain, Elevated };

    struct Spec {
        QString exe;
        QStringList args;
        QString cwd;
        QList<std::pair<QString, QString>> env; // added to the user's environment
        int memoryMB = 512;
        bool processes = false;
        bool clipboardRead = false;
        bool clipboardWrite = false;
        bool ui = false;
        bool strict = false; // low integrity, restricted token
        bool oem = false; // its stdout is in the OEM code page
        bool stdinOpen = true; // false: its stdin is NUL (once plugins)
        bool elevated = false; // Host's: start it on the elevated runner (not sent)
    };

    // `elevated`: the second runner, started with Win顺's own token as it is
    // (administrator, when Win顺 is), its pipe open to administrators and
    // SYSTEM alone. Its instance numbers begin at `firstInstance`, apart from
    // the first runner's, so the host can tell whose an instance is.
    explicit Runner(QString runnerExe, QObject* parent = nullptr);
    Runner(QString runnerExe, bool elevated, std::uint64_t firstInstance, QObject* parent = nullptr);
    ~Runner() override; // ends the runner and with it every plugin

    // Makes the pipe and starts the runner. False with a reason when the
    // process cannot be started; else ready() follows when it has said
    // hello, or exited() when it never does.
    bool start(QString* error = nullptr);
    void shutdown(); // asks it to end everything and exit; exited() follows
    bool running() const noexcept { return m_process != nullptr; }
    bool isReady() const noexcept { return m_ready; } // hello came
    TokenSource tokenSource() const noexcept { return m_tokenSource; }
    const QJsonObject& facts() const noexcept { return m_facts; } // what hello reported
    DWORD pid() const noexcept { return m_pid; }

    // A plugin process: its instance number, at once; started() or
    // failed() for it follow. Before ready(), the request waits.
    std::uint64_t launch(const Spec& spec);
    void send(std::uint64_t instance, const QByteArray& line); // to its stdin, without the newline
    void stop(std::uint64_t instance); // TerminateJobObject: exited() follows

signals:
    void ready();
    void exited(); // the runner is gone (closed, crashed, or the pipe broke); every instance with it
    void started(std::uint64_t instance, quint32 pid);
    void failed(std::uint64_t instance, const QString& stage, quint32 error);
    void line(std::uint64_t instance, const QByteArray& line); // a line of its stdout
    void stderrLine(std::uint64_t instance, const QString& text);
    void instanceExited(std::uint64_t instance, quint32 code, const QStringList& stderrTail, bool overlong);
    void logged(const QString& text); // the runner's own log

private:
    void handleLine(const QByteArray& line);
    void handleControl(const QJsonObject& message);
    void sendRaw(const QByteArray& line);
    void closeAll();

    QString m_exe;
    std::wstring m_pipeName;
    QString m_key;
    std::unique_ptr<pipe::LineChannel> m_channel;
    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_process = nullptr;
    HANDLE m_job = nullptr;
    DWORD m_pid = 0;
    bool m_ready = false;
    bool m_stopping = false;
    TokenSource m_tokenSource = TokenSource::None;
    QJsonObject m_facts;
    bool m_elevated = false;
    std::uint64_t m_nextInstance = 1;
    std::vector<QByteArray> m_waiting; // lines to send once ready
};

} // namespace ws::plugin
