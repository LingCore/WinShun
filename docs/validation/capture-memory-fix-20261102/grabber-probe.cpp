#include "Grabber.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <algorithm>

void memory(const char* phase, qint64 elapsed = 0)
{
    PROCESS_MEMORY_COUNTERS_EX2 m {};
    m.cb = sizeof m;
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof m))
        std::exit(2);
    std::printf("%s, private_commit=%.2f, private_working=%.2f, working=%.2f, elapsed_ms=%lld\n", phase,
        m.PrivateUsage / 1048576.0, m.PrivateWorkingSetSize / 1048576.0, m.WorkingSetSize / 1048576.0, elapsed);
    std::fflush(stdout);
}
int main(int argc, char** argv)
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QCoreApplication app(argc, argv);
    memory("before");
    ws::capture::Grabber grabber;
    QThread::msleep(1000);
    memory("prepared");
    QRect region;
    const bool hdrTest = qEnvironmentVariableIsSet("WINSHUN_TEST_HDR_CAPTURE");
    for (int i = 0; i < 6; ++i) {
        QElapsedTimer clock; clock.start();
        QString report;
        auto shots = grabber.grab(&report, hdrTest);
        const qint64 elapsed = clock.elapsed();
        bool valid = !shots.empty();
        for (const auto& shot : shots)
            valid = valid && !shot.image.isNull() && shot.image.size() == shot.rect.size()
                && (!hdrTest || (shot.wgc && shot.hdrPixels && !shot.hdrPixels->isNull()));
        if (region.isEmpty() && valid) region = QRect(shots.front().rect.topLeft(), QSize(200, 150));
        std::printf("round=%d valid=%d %s\n", i, valid, qPrintable(report));
        if (!valid) return 3;
        memory("holding_frame", elapsed);
        shots.clear();
        QThread::msleep(500);
        memory("released_frame");
    }
    grabber.refresh();
    for (int run = 0; run < 2; ++run) {
        auto stream = grabber.stream(region);
        if (!stream) return 4;
        const auto frame = stream->next(0, 2000);
        if (frame.image.isNull() || frame.image.size() != region.size()) return 5;
        memory("stream_active");
        stream.reset();
        QThread::msleep(500);
        memory("stream_ended");
    }
}
