// The other device in the clipboard sync poster (tools/screenshots/release042.py,
// not shipped): the ShunSync engine alone, named 办公室电脑, in the group the
// poster's plugin is seeded with (identity.json written by the script). Once the
// plugin is online it copies two files and a folder (82 MB; files are only
// offered until they are asked for, as on Mac顺) and then a line of text;
// it keeps running until <stop file> appears. What the plugin sends here (the
// clipboard of the PC it runs on) is counted, never shown or kept.
//   sync_peer.exe <dataDir> <port> <work dir> <stop file>
// Built against wsshunsync.lib of a build folder (release042.py builds it).

#include "Engine.h"
#include "Util.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace shunsync;
namespace fs = std::filesystem;

namespace {

void say(const std::string& line)
{
    std::printf("[%lld] %s\n", static_cast<long long>(nowMs() % 1'000'000), line.c_str());
    std::fflush(stdout);
}

class Sink : public ClipboardSink {
public:
    std::atomic<int> count {0};
    bool putText(const std::string&, const std::string&) override { return ++count, true; }
    bool putImage(const Bytes&, const std::string&) override { return ++count, true; }
    bool putFiles(const std::vector<std::wstring>&, const std::string&) override { return ++count, true; }
};

bool sized(const fs::path& path, std::uintmax_t bytes)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    { std::ofstream(path, std::ios::binary); }
    fs::resize_file(path, bytes, error); // zeros, written lazily: nothing reads them
    return !error;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 5) {
        std::printf("sync_peer.exe <dataDir> <port> <work dir> <stop file>\n");
        return 2;
    }
    const std::wstring dataDir = argv[1];
    const auto port = static_cast<std::uint16_t>(std::wcstoul(argv[2], nullptr, 10));
    const fs::path work = argv[3];
    const fs::path stop = argv[4];

    const fs::path deck = work / L"第四季度方案.pptx";
    const fs::path sheet = work / L"预算明细.xlsx";
    const fs::path photos = work / L"展会照片";
    constexpr std::uintmax_t MB = 1024 * 1024;
    if (!sized(deck, 46 * MB) || !sized(sheet, 2 * MB) || !sized(photos / L"IMG_0412.jpg", 12 * MB)
        || !sized(photos / L"IMG_0413.jpg", 11 * MB) || !sized(photos / L"IMG_0415.jpg", 11 * MB)) {
        say("FAIL: the files to copy could not be made");
        return 1;
    }

    Sink sink;
    Options options;
    options.deviceName = "办公室电脑";
    options.platform = "windows";
    options.dataDir = dataDir;
    options.preferredPort = port;
    options.discovery = false;
    options.prefetchBytes = 0; // nothing of the plugin's PC pulled here
    Events events;
    events.log = [](const std::string& line) { say("log: " + line); };
    Engine engine(options, sink, events);
    std::string problem;
    if (!engine.start(&problem)) {
        say("FAIL: start: " + problem);
        return 1;
    }
    say("listening on port " + std::to_string(engine.port()) + ", id " + engine.deviceId());

    bool sent = false;
    std::error_code error;
    for (const std::int64_t until = nowMs() + 15 * 60 * 1000; nowMs() < until && !fs::exists(stop, error);) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (sent)
            continue;
        const Snapshot s = engine.snapshot();
        const bool online = std::ranges::any_of(s.members, [](const MemberView& m) { return m.state == PeerState::Online; });
        if (!online)
            continue;
        say("the plugin is online");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        engine.localFiles({deck.wstring(), sheet.wstring(), photos.wstring()});
        say("files copied");
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        engine.localText("周五的评审会改到下午 3 点，地点在三楼大会议室");
        say("text copied");
        sent = true;
    }
    say("received from the plugin: " + std::to_string(sink.count.load()) + " (not kept)");
    engine.stop();
    return sent ? 0 : 1;
}
