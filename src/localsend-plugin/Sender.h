#pragma once

// Sending to a device: one job at a time, on a thread of its own. Files
// (folders sent with what is in them, their names relative: "相册/IMG_1.jpg")
// or a text message; prepare-upload, then the files one after another.
// The device's person may take a while to say yes (LocalSend's own sender
// waits for ever; we wait kAnswerTime). A device that wants a PIN is
// asked again with the one the user then types.

#include "Client.h"
#include "Protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace localsend {

struct Outgoing {
    enum class Kind { Files, Text };
    Kind kind = Kind::Files;
    std::vector<std::wstring> paths; // files and folders
    std::string text;
};

// What a set of chosen files and folders comes to (for the rows: bounded,
// `budget` for all of it, however many were chosen; never on the thread
// that answers Win顺). One on a network share is not looked at (a NAS that
// sleeps answers in seconds): it counts as a file, its size unknown.
struct Summary {
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    bool partial = false; // the count stopped at the limit or the time, or a share was not looked at
    std::size_t remote = 0; // of the chosen, on a network share
    std::vector<std::string> names; // the chosen ones' names, the first few
    bool operator==(const Summary&) const = default;
};
Summary summarize(const std::vector<std::wstring>& paths, std::chrono::milliseconds budget);
bool onNetwork(const std::wstring& path); // a share (\\server\share) or a drive mapped to one

struct OutgoingFile {
    std::wstring path;
    std::string name; // as sent: "相册/IMG_1.jpg"
    std::uint64_t size = 0;
};
// The files to send for these paths: at most kMaxFiles. Links to folders
// inside a folder are not followed (a junction back up would never end);
// linked files are sent, with the size of what they point to. False when
// there are none or too many (`why`, in the user's words).
bool collectFiles(const std::vector<std::wstring>& paths, std::vector<OutgoingFile>& out, std::string* why);
std::string mimeType(const std::wstring& path);

struct SendOutcome {
    enum class Kind {
        Sent, // the files went
        TextSent,
        Declined, // 403
        Busy, // 409
        NoAnswer, // nobody said yes or no in kAnswerTime
        PinRefused, // a PIN was wanted and none came
        TooManyPins, // 429
        Unreachable,
        NotTheDevice, // another certificate at its address: some other device is there now
        Interrupted, // the files began to go and did not all arrive
        Cancelled, // by the user
        Nothing, // nothing to send (`detail` says why)
        Paused, // 429 from Win顺: a no just now (it waits a while before asking again), or a text a moment ago
        NoRoom, // 403 from Win顺: not enough space, or where it saves is not there
        TooMany, // 413 from Win顺: more files than it takes at once
    };
    Kind kind = Kind::Sent;
    bool text = false; // a text message, not files
    std::string device;
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    std::string detail; // for the log (never a file name)
};

struct SendView {
    enum class State { Preparing, Waiting, NeedsPin, Sending };
    State state = State::Preparing;
    std::string device;
    bool text = false;
    bool wrongPin = false; // NeedsPin after one was tried
    std::size_t files = 0;
    std::uint64_t total = 0;
    std::uint64_t sent = 0;
    double bytesPerSecond = 0;
};

class SendJob {
public:
    static constexpr auto kAnswerTime = std::chrono::minutes(3);
    static constexpr auto kPinTime = std::chrono::minutes(5);

    // `done` is called once, on the job's thread, when it ends.
    SendJob(Client& client, Target target, std::string deviceName, DeviceInfo self, Outgoing what,
        std::function<void(const SendOutcome&)> done);
    ~SendJob(); // cancelled, its thread waited for
    SendJob(const SendJob&) = delete;
    SendJob& operator=(const SendJob&) = delete;

    std::optional<SendView> view() const; // nothing once it has ended
    void cancel();
    // Before the process ends: cancelled, and up to `within` for the job to
    // end and the receiver to hear it (its partial files go then); after
    // that, telling it is cut short. Never longer than `within`.
    void shutDown(std::chrono::milliseconds within);
    bool givePin(const std::string& pin); // false unless it waits for one
    bool finished() const noexcept { return m_finished; }

private:
    void run();
    SendOutcome::Kind work(SendOutcome& outcome);
    SendOutcome::Kind prepare(const PrepareUpload& request, std::string& sessionId, std::map<std::string, std::string>& tokens,
        std::string& detail);
    void sayCancel(const std::string& sessionId);
    void sample(); // the speed

    Client& m_client;
    const Target m_target;
    const std::string m_deviceName;
    const DeviceInfo m_self;
    const Outgoing m_what;
    std::function<void(const SendOutcome&)> m_done;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    SendView m_view;
    std::optional<std::string> m_pin; // given, not yet tried
    std::atomic<bool> m_cancelled {false};
    std::atomic<bool> m_finished {false};
    std::atomic<std::uint64_t> m_sent {0};
    std::vector<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> m_samples;
    std::chrono::milliseconds m_sayWithin {2000}; // how long telling the receiver "cancelled" may take
    Cutter m_cutter;
    Cutter m_sayCutter; // that telling, cut when the process ends first
    std::thread m_thread;
};

} // namespace localsend
