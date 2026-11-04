#include "ClipScrollBench.h"

#include "Clipboard.h"
#include "ClipModel.h"
#include "ClipStore.h"
#include "ClipThumbnailProvider.h"
#include "FileIconProvider.h"
#include "Placement.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/ClipboardWatcher.h"
#include "platform/WindowEffects.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QMutex>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlDebuggingEnabler>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QScreen>
#include <QSet>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::preview {

namespace {

void report(const QString& line)
{
    std::fprintf(stderr, "%s\n", qUtf8Printable(line));
    std::fflush(stderr);
}

// The event loop for a while, the GUI thread asleep when it has nothing to
// do (as in the app): its CPU time per frame is then the frame's own work.
void pumpFor(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, Qt::PreciseTimer, &loop, &QEventLoop::quit);
    loop.exec();
}

QQuickItem* findNamed(QQuickItem* item, const QString& name)
{
    if (!item)
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (QQuickItem* child : item->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto at = static_cast<std::size_t>(std::clamp(p * static_cast<double>(values.size() - 1), 0.0, double(values.size() - 1)));
    return values[at];
}

qint64 privateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return 0;
    return static_cast<qint64>(counters.PrivateWorkingSetSize);
}

// Picture sizes of a real history (the author's: 48 pictures, 2026-11-03).
constexpr std::pair<int, int> kPictureSizes[] = {
    {543, 88}, {929, 89}, {1263, 66}, {579, 164}, {511, 199}, {942, 111}, {401, 267}, {1353, 95},
    {908, 158}, {394, 391}, {340, 474}, {971, 183}, {967, 191}, {454, 414}, {701, 307}, {429, 505},
    {1333, 188}, {436, 594}, {655, 474}, {797, 401}, {912, 399}, {822, 518}, {419, 1041}, {999, 472},
    {924, 555}, {1591, 398}, {884, 728}, {1075, 616}, {1317, 571}, {841, 926}, {1140, 713}, {1157, 803},
    {1414, 674}, {1385, 693}, {1140, 893}, {1080, 960}, {1439, 779}, {1177, 988}, {1243, 1033}, {1566, 868},
    {1387, 1001}, {1066, 1305}, {2020, 701}, {1380, 1080}, {1562, 959}, {1820, 870}, {2560, 1600}, {1920, 1080},
};

// A screen-like picture: panels, lines of text, a few colours; compresses as
// screenshots do.
QByteArray makePicture(int n, QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor::fromHsl((n * 47) % 360, 30, 245));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QRandomGenerator random(static_cast<quint32>(n + 1));
    for (int i = 0; i < 6; ++i) {
        const QRect panel(random.bounded(size.width()), random.bounded(size.height()),
            random.bounded(40, std::max(41, size.width() / 2)), random.bounded(20, std::max(21, size.height() / 2)));
        painter.fillRect(panel, QColor::fromHsl(random.bounded(360), 60, random.bounded(150, 235)));
    }
    painter.setPen(QColor(30, 30, 30));
    painter.setFont(QFont(u"Microsoft YaHei UI"_s, 11));
    for (int y = 24; y < size.height(); y += 22)
        painter.drawText(12 + random.bounded(40), y, u"第 %1 行 clipboard row text %2 — Win顺 滚动测试 %3"_s.arg(y).arg(n).arg(random.generate()));
    painter.end();
    QByteArray bytes;
    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
    }
    return bytes;
}

// The fixture is kept between runs (pictures take a while to make).
QStringList pictureFiles(const QDir& folder, int count)
{
    QDir().mkpath(folder.path());
    QStringList files;
    for (int i = 0; i < count; ++i) {
        const QString path = folder.filePath(u"pic%1.png"_s.arg(i, 4, 10, QLatin1Char('0')));
        if (!QFile::exists(path)) {
            const auto [w, h] = kPictureSizes[i % std::size(kPictureSizes)];
            QFile file(path);
            if (file.open(QIODevice::WriteOnly))
                file.write(makePicture(i, QSize(w, h)));
        }
        files.append(path);
    }
    return files;
}

struct Frame {
    qint64 swapNs = 0;
};

struct Sample {
    qint64 ns = 0;
    double contentY = 0;
    int placeholders = 0; // picture rows in view without their thumbnail
    int blankIcons = 0; // file rows in view without their icon
    int anchor = -1; // the first row in view: its index and where it is drawn
    double anchorY = 0;
    quint64 guiCycles = 0; // the GUI thread's CPU cycles so far (QueryThreadCycleTime): not counting time other programs had the core
};

quint64 threadCycles()
{
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

} // namespace

int runClipScrollBench(const QDir& output, const QStringList& args)
{
    const auto valueOf = [&](QStringView flag, const QString& fallback) {
        const qsizetype at = args.indexOf(flag);
        return at >= 0 && at + 1 < args.size() ? args[at + 1] : fallback;
    };
    if (args.contains(u"--qml-debug"_s))
        QQmlDebuggingEnabler::enableDebugging(false);
    const int clipCount = valueOf(u"--clips", u"1000"_s).toInt();
    const bool large = args.contains(u"--large-pictures"_s);
    const QString label = valueOf(u"--label", u"run"_s);
    QDir().mkpath(output.path());
    const QDir fixture(valueOf(u"--fixture", output.filePath(u"fixture"_s)));
    QDir().mkpath(fixture.path());

    QElapsedTimer setup;
    setup.start();
    const QStringList pictures = pictureFiles(QDir(fixture.filePath(u"pictures"_s)), std::max(1, clipCount * 2 / 5));
    // Files of several kinds to point at (their icons: by type, or their own).
    QStringList files;
    const QStringList types {u"txt"_s, u"docx"_s, u"pdf"_s, u"xlsx"_s, u"zip"_s, u"exe"_s, u"lnk"_s, u"md"_s};
    for (int i = 0; i < 24; ++i) {
        const QString path = QDir::toNativeSeparators(fixture.filePath(u"files/文档%1.%2"_s.arg(i).arg(types[i % types.size()])));
        QDir().mkpath(fixture.filePath(u"files"_s));
        if (!QFile::exists(path)) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly))
                file.write("x");
        }
        files.append(path);
    }
    const QStringList folders {QDir::toNativeSeparators(fixture.path()), u"C:\\Windows"_s, u"C:\\Users"_s,
        QDir::toNativeSeparators(fixture.filePath(u"files"_s))};

    QTemporaryDir temporary;
    ClipStore store(temporary.filePath(u"clipboard"_s));
    store.setLimits({0, 0}, QDateTime::currentMSecsSinceEpoch());
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) {
        report(u"bench: the temporary history could not open"_s);
        return 4;
    }
    const qint64 work = store.addGroup(u"工作"_s);
    const qint64 start = QDateTime::currentMSecsSinceEpoch() - qint64(clipCount) * 60000;
    int pictureAt = 0;
    std::vector<qint64> pinned, grouped;
    for (int i = 0; i < clipCount; ++i) {
        ClipCapture capture;
        capture.source = i % 3 == 0 ? u"微信"_s : i % 3 == 1 ? u"Visual Studio Code"_s : u"Microsoft Edge"_s;
        switch (i % 20) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: {
            QFile file(pictures[pictureAt++ % pictures.size()]);
            if (!file.open(QIODevice::ReadOnly))
                return 4;
            capture.kind = ClipKind::Image;
            capture.imageData = file.readAll();
            const QImage size = QImage::fromData(capture.imageData);
            capture.width = size.width();
            capture.height = size.height();
            capture.hash = QCryptographicHash::hash(capture.imageData + QByteArray::number(i), QCryptographicHash::Sha1);
            capture.screenshot = i % 3 == 0;
            break;
        }
        case 7: // a picture copied as a file (WeChat)
            capture.kind = ClipKind::Files;
            capture.text = QDir::toNativeSeparators(pictures[(pictureAt + 7) % pictures.size()]);
            break;
        case 12: case 13:
            capture.kind = ClipKind::Link;
            capture.text = u"https://example.com/path/%1?q=滚动&page=%2"_s.arg(i).arg(i % 7);
            break;
        case 14:
            capture.kind = ClipKind::Files;
            capture.text = i % 3 == 0 ? files[i % files.size()] + u"\n"_s + files[(i + 1) % files.size()] : files[i % files.size()];
            break;
        case 15:
            capture.kind = ClipKind::Path;
            capture.text = folders[i % folders.size()];
            break;
        case 16: // gone
            capture.kind = ClipKind::Files;
            capture.text = u"C:\\wsbench-gone\\已删除的文件%1.docx"_s.arg(i);
            break;
        case 17: // a server that is not there
            capture.kind = ClipKind::Files;
            capture.text = u"\\\\wsbench-nohost\\share\\报告%1.xlsx"_s.arg(i);
            break;
        case 18:
            capture.kind = ClipKind::Text;
            capture.text = u"#%1"_s.arg(QString::number(0x203a7bd5 + i * 977, 16).right(6));
            break;
        default:
            capture.kind = ClipKind::Text;
            capture.text = i % 5 == 0
                ? u"第 %1 段很长的文字。\n"_s.arg(i).repeated(40)
                : u"Clipboard entry %1: some text that was copied while working, with 中文 too"_s.arg(i);
            break;
        }
        const qint64 id = store.add(capture, start + qint64(i) * 60000);
        if (i >= clipCount - 4)
            pinned.push_back(id);
        else if (i % 25 == 0)
            grouped.push_back(id);
    }
    store.setGroup(grouped, work);
    store.setGroup(pinned, ClipStore::kPinned);
    store.flush();
    report(u"bench: %1 entries (%2 pictures from %3 files) ready in %4 ms"_s
               .arg(store.clips().size()).arg(pictureAt).arg(pictures.size()).arg(setup.elapsed()));

    QJsonObject result {{u"label"_s, label}, {u"clips"_s, int(store.clips().size())}, {u"largePictures"_s, large}};

    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    clipboard.setRowOptions(false, large, valueOf(u"--size", u"standard"_s));
    clipboard.setNumberHints(valueOf(u"--hints", u"always"_s)); // the setting's default
    clipboard.setPreviewOpen(args.contains(u"--preview-open"_s));
    clipboard.setActive(true);
    clipboard.handleShown();

    // The model alone: what each row costs to read, on the GUI thread.
    {
        ClipModel* model = clipboard.items();
        std::map<int, std::vector<double>> byKind;
        std::vector<double> perRole(ClipModel::PastedRole + 1, 0);
        const QList<int> roles = model->roleNames().keys();
        for (int pass = 0; pass < 2; ++pass) {
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex index = model->index(row);
                QElapsedTimer timer;
                timer.start();
                int kind = 0;
                for (const int role : roles) {
                    QElapsedTimer one;
                    one.start();
                    const QVariant value = model->data(index, role);
                    if (role == ClipModel::KindRole)
                        kind = value.toInt();
                    if (pass == 1)
                        perRole[std::size_t(role - Qt::UserRole - 1)] += one.nsecsElapsed() / 1000.0;
                }
                const QString image = model->data(index, ClipModel::ImageRole).toString();
                if (pass == 1)
                    byKind[image.isEmpty() ? kind : 10 + kind].push_back(timer.nsecsElapsed() / 1000.0);
            }
        }
        QJsonObject kinds;
        for (const auto& [kind, values] : byKind) {
            double sum = 0;
            for (const double v : values)
                sum += v;
            kinds.insert(QString::number(kind), QJsonObject {{u"rows"_s, int(values.size())},
                {u"mean_us"_s, sum / values.size()}, {u"p95_us"_s, percentile(values, 0.95)}, {u"max_us"_s, percentile(values, 1)}});
        }
        QJsonObject rolesUs;
        const QHash<int, QByteArray> names = model->roleNames();
        for (const int role : roles)
            rolesUs.insert(QString::fromLatin1(names.value(role)), perRole[std::size_t(role - Qt::UserRole - 1)] / model->rowCount());
        result.insert(u"data_us_per_row_by_kind"_s, kinds);
        result.insert(u"data_us_per_row_by_role"_s, rolesUs);
        report(u"bench: model data() per row by kind (warm): %1"_s.arg(QString::fromUtf8(QJsonDocument(kinds).toJson(QJsonDocument::Compact))));
        report(u"bench: model data() per row by role (warm, us): %1"_s.arg(QString::fromUtf8(QJsonDocument(rolesUs).toJson(QJsonDocument::Compact))));
    }

    // As the running app: the Mica backdrop (an alpha swap chain), the Shell
    // icon handlers loaded once (the clipboard's prewarm does that there).
    SystemTheme::setBackdropAvailable(true);
    (void)FileIconProvider::fileIcon(files[5], 48);
    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    engine.addImageProvider(u"clipthumb"_s, new ClipThumbnailProvider);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(&engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) {
        report(u"bench: "_s + component.errorString());
        return 4;
    }
    if (args.contains(u"--flipped"_s)) { // opened above its box, turned over (Clipboard/FlipAbove)
        for (QObject* child : window->contentItem()->findChildren<QObject*>()) {
            if (child->metaObject()->indexOfProperty("flipped") >= 0 && child->metaObject()->indexOfProperty("above") >= 0)
                child->setProperty("above", true);
        }
    }
    window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowStaysOnTopHint);
    window->setProperty("_q_showWithoutActivating", true);
    QSurfaceFormat format = window->requestedFormat();
    format.setAlphaBufferSize(8);
    window->setFormat(format);
    window->create();
    win::styleFramelessWindow(window, true);
    frame.setWindow(window);
    placement.setWindow(window);
    clipboard.setWindow(window);
    QScreen* screen = QGuiApplication::primaryScreen();
    const QRect area = screen->availableGeometry();
    const QSize size(460, std::min(760, area.height() - 40));
    window->setGeometry(QRect(QPoint(area.right() - size.width() - 40, area.top() + 20), size));
    window->show();
    clipboard.handlePlaced();
    pumpFor(1500);

    auto* list = findNamed(window->contentItem(), u"clipboardEntries"_s);
    if (!list) {
        report(u"bench: no clipboard list"_s);
        return 4;
    }
    const double dpr = window->devicePixelRatio();
    result.insert(u"dpr"_s, dpr);
    result.insert(u"hints"_s, clipboard.numberHints());
    result.insert(u"preview"_s, clipboard.previewOpen());
    result.insert(u"refresh_hz"_s, screen->refreshRate());
    result.insert(u"window"_s, u"%1x%2"_s.arg(window->width()).arg(window->height()));
    report(u"bench: window %1x%2 at %3%, %4 Hz, list %5 px tall, %6 rows"_s.arg(window->width()).arg(window->height())
               .arg(dpr * 100).arg(screen->refreshRate()).arg(list->height()).arg(list->property("count").toInt()));
    {
        RECT r {};
        ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &r);
        report(u"bench: native rect %1 %2 %3 %4"_s.arg(r.left).arg(r.top).arg(r.right).arg(r.bottom));
    }

    QElapsedTimer clock;
    clock.start();
    QMutex mutex;
    std::vector<qint64> swaps;
    // Each frame's stages, as the threaded render loop runs them: the GUI
    // thread animates (afterAnimating), polishes (the list lays out, rows are
    // made or reused), then waits while the render thread synchronizes;
    // the render thread then renders and swaps.
    std::vector<qint64> syncStarts, syncEnds, renderEnds;
    std::vector<quint64> renderCycles; // the render thread's CPU cycles at each afterRendering
    // Cycles per ms of this thread's CPU time, from a busy wait.
    double cyclesPerMs = 0;
    {
        QElapsedTimer spin;
        const quint64 c0 = threadCycles();
        spin.start();
        while (spin.nsecsElapsed() < 100'000'000) {
        }
        cyclesPerMs = double(threadCycles() - c0) / (spin.nsecsElapsed() / 1e6);
    }
    std::vector<Sample> samples;
    std::map<QQuickItem*, qint64> waiting; // a picture row in view since, without its thumbnail
    std::vector<double> latencies; // ms from a picture row in view to its thumbnail shown
    bool recording = false;
    // Where the mouse rests (over the list, as when a wheel is turned): the
    // row under it changes as the rows move (a hover flip), and rows made
    // while scrolling (the reuse pool ran dry) are counted, each with when.
    const QPointF over = list->mapToScene(QPointF(list->width() * 0.6, list->height() / 2));
    QQuickItem* underMouse = nullptr;
    std::vector<qint64> flips, made, reused;
    QSet<QQuickItem*> seenRows;
    QHash<QQuickItem*, QString> rowNames; // a pooled row reused for another entry gets that one's name
    QObject::connect(window, &QQuickWindow::beforeSynchronizing, window, [&] {
        QMutexLocker lock(&mutex);
        if (recording)
            syncStarts.push_back(clock.nsecsElapsed());
    }, Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::afterSynchronizing, window, [&] {
        QMutexLocker lock(&mutex);
        if (recording)
            syncEnds.push_back(clock.nsecsElapsed());
    }, Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::afterRendering, window, [&] {
        QMutexLocker lock(&mutex);
        if (recording) {
            renderEnds.push_back(clock.nsecsElapsed());
            renderCycles.push_back(threadCycles());
        }
    }, Qt::DirectConnection);
    QObject::connect(window, &QQuickWindow::frameSwapped, window, [&] {
        QMutexLocker lock(&mutex);
        if (recording)
            swaps.push_back(clock.nsecsElapsed());
    }, Qt::DirectConnection);
    QQuickItem* content = list->property("contentItem").value<QQuickItem*>();
    QObject::connect(window, &QQuickWindow::afterAnimating, window, [&] {
        for (QQuickItem* row : content->childItems()) {
            if (!row->objectName().startsWith(u"clipboardRow-"))
                continue;
            if (!seenRows.contains(row)) {
                seenRows.insert(row);
                if (recording)
                    made.push_back(clock.nsecsElapsed());
            } else if (rowNames.value(row) != row->objectName() && recording) {
                reused.push_back(clock.nsecsElapsed());
            }
            rowNames.insert(row, row->objectName());
        }
        if (!recording)
            return;
        Sample sample {clock.nsecsElapsed(), list->property("contentY").toDouble()};
        sample.guiCycles = threadCycles();
        const QRectF view = list->mapRectToScene(QRectF(0, 0, list->width(), list->height()));
        QQuickItem* hit = nullptr;
        for (QQuickItem* row : content->childItems()) {
            if (!row->isVisible() || !row->objectName().startsWith(u"clipboardRow-"))
                continue;
            const QRectF where = row->mapRectToScene(QRectF(0, 0, row->width(), row->height()));
            if (where.contains(over))
                hit = row;
            if (!where.intersects(view))
                continue;
            const int index = row->objectName().mid(13).toInt();
            if (sample.anchor < 0 || index < sample.anchor) {
                sample.anchor = index;
                sample.anchorY = where.y();
            }
            if (row->property("picture").toBool()) {
                static QHash<QQuickItem*, QQuickItem*> thumbs;
                QQuickItem*& thumb = thumbs[row];
                if (!thumb)
                    thumb = findNamed(row, u"clipboardThumbnail"_s);
                const bool ready = thumb && thumb->property("ready").toBool();
                const auto it = waiting.find(row);
                if (!ready) {
                    ++sample.placeholders;
                    if (it == waiting.end())
                        waiting.emplace(row, sample.ns);
                } else if (it != waiting.end()) {
                    latencies.push_back((sample.ns - it->second) / 1e6);
                    waiting.erase(it);
                }
            } else {
                const int kind = row->property("kind").toInt();
                if (kind == 2 || kind == 3) {
                    QQuickItem* icon = findNamed(row, u"clipboardFileIcon"_s);
                    if (icon && icon->isVisible() && icon->property("status").toInt() != 1)
                        ++sample.blankIcons;
                }
            }
        }
        if (hit != underMouse) {
            underMouse = hit;
            flips.push_back(sample.ns);
        }
        samples.push_back(sample);
    }, Qt::DirectConnection);

    // --no-hover: the mouse never comes over the window (the rows' hover off).
    if (!args.contains(u"--no-hover"_s)) {
        QMouseEvent move(QEvent::MouseMove, over, window->mapToGlobal(over), Qt::NoButton, Qt::NoButton, {});
        QCoreApplication::sendEvent(window, &move);
    }
    pumpFor(300);

    const auto wheel = [&](int delta) {
        QWheelEvent event(over, window->mapToGlobal(over), {}, QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
            Qt::NoScrollPhase, false);
        event.setTimestamp(static_cast<quint64>(clock.elapsed()));
        QCoreApplication::sendEvent(window, &event);
    };

    // --trace: down a while unrecorded, then up notch by notch at
    // --trace-ms, each change of contentY (and who made it) and each frame
    // written down: the wheel's goal and glide, the list's range, the first
    // row in view as laid out and as drawn.
    if (args.contains(u"--trace"_s)) {
        QObject* glide = nullptr;
        for (QObject* child : list->findChildren<QObject*>()) {
            if (child->metaObject()->indexOfProperty("writeDepth") >= 0)
                glide = child;
        }
        report(u"bench: trace: wheel controller %1"_s.arg(glide ? u"found"_s : u"MISSING"_s));
        const int intervalMs = valueOf(u"--trace-ms", u"60"_s).toInt();
        const int direction = args.contains(u"--trace-down"_s) ? -120 : 120;
        if (direction > 0) { // as --motion before its notch60_up
            const QList<QPair<int, int>> before {{1, 0}, {40, 30}, {40, 60}, {30, 100}, {20, 150}, {20, 180}};
            for (const auto& [count, ms] : before) {
                for (int i = 0; i < count; ++i) {
                    wheel(-120);
                    pumpFor(ms);
                }
                pumpFor(700);
            }
        }
        QJsonArray events;
        bool tracing = true;
        const auto state = [&](const QString& what) {
            QJsonObject e {{u"t"_s, clock.nsecsElapsed() / 1e6}, {u"what"_s, what},
                {u"contentY"_s, list->property("contentY").toDouble()}, {u"originY"_s, list->property("originY").toDouble()},
                {u"contentHeight"_s, list->property("contentHeight").toDouble()}};
            if (glide) {
                e.insert(u"writing"_s, glide->property("writeDepth").toInt());
                e.insert(u"running"_s, glide->property("running").toBool());
                e.insert(u"position"_s, glide->property("position").toDouble());
                e.insert(u"requestedY"_s, glide->property("requestedY").toDouble());
                e.insert(u"minimumY"_s, glide->property("minimumY").toDouble());
                e.insert(u"unsnapped"_s, glide->property("unsnapped").toBool());
                e.insert(u"carry"_s, glide->property("carry").toDouble());
                if (what != u"frame" && what != u"contentY") {
                    // What the wheel reads its row edges from, at the goal and at contentY.
                    const double top = list->property("topMargin").toDouble();
                    const double requested = glide->property("requestedY").toDouble();
                    for (const auto& [name, y] : {std::pair {u"goal"_s, requested}, std::pair {u"here"_s, list->property("contentY").toDouble()}}) {
                        QVariant up, down;
                        QMetaObject::invokeMethod(glide, "rowEdge", Q_RETURN_ARG(QVariant, up), Q_ARG(QVariant, y), Q_ARG(QVariant, -1));
                        QMetaObject::invokeMethod(glide, "rowEdge", Q_RETURN_ARG(QVariant, down), Q_ARG(QVariant, y), Q_ARG(QVariant, 1));
                        int index = -2;
                        QMetaObject::invokeMethod(list, "indexAt", Q_RETURN_ARG(int, index),
                            Q_ARG(qreal, list->property("contentX").toDouble() + 1), Q_ARG(qreal, y + top - 0.5));
                        e.insert(name + u"EdgeUp"_s, up.toDouble());
                        e.insert(name + u"EdgeDown"_s, down.toDouble());
                        e.insert(name + u"Index"_s, index);
                    }
                }
            }
            QQuickItem* first = nullptr;
            int firstIndex = INT_MAX;
            const QRectF view = list->mapRectToScene(QRectF(0, 0, list->width(), list->height()));
            int madeTop = INT_MAX; // the first row made at all (in the cacheBuffer too)
            double madeTopY = 0;
            for (QQuickItem* row : content->childItems()) {
                if (!row->isVisible() || !row->objectName().startsWith(u"clipboardRow-"))
                    continue;
                if (const int index = row->objectName().mid(13).toInt(); index < madeTop) {
                    madeTop = index;
                    madeTopY = row->y();
                }
                if (!row->mapRectToScene(QRectF(0, 0, row->width(), row->height())).intersects(view))
                    continue;
                const int index = row->objectName().mid(13).toInt();
                if (index < firstIndex) {
                    firstIndex = index;
                    first = row;
                }
            }
            e.insert(u"madeTop"_s, madeTop == INT_MAX ? -1 : madeTop);
            e.insert(u"madeTopY"_s, madeTopY);
            e.insert(u"made"_s, content->childItems().size());
            if (first) {
                e.insert(u"row"_s, firstIndex);
                e.insert(u"rowY"_s, first->y()); // laid out, in the content
                e.insert(u"rowH"_s, first->height());
                e.insert(u"rowX"_s, first->x());
                e.insert(u"rowDrawn"_s, first->mapToScene(QPointF()).y() - view.y()); // with its Translate and contentItem.y
                e.insert(u"rowPlain"_s, first->y() + content->y()); // without its Translate
            }
            events.append(e);
        };
        // contentY changes as they happen (as QSignalSpy: a receiver's own
        // method index past QObject's).
        const int signal = list->metaObject()->indexOfSignal("contentYChanged()");
        struct Spy : QObject {
            std::function<void()> on;
            int qt_metacall(QMetaObject::Call c, int id, void** a) override
            {
                id = QObject::qt_metacall(c, id, a);
                if (id >= 0 && c == QMetaObject::InvokeMetaMethod) {
                    on();
                    --id;
                }
                return id;
            }
        } spy;
        spy.on = [&] { if (tracing) state(u"contentY"_s); };
        QMetaObject::connect(list, signal, &spy, QObject::staticMetaObject.methodCount(), Qt::DirectConnection);
        const QMetaObject::Connection frame = QObject::connect(window, &QQuickWindow::afterAnimating, window,
            [&] { if (tracing) state(u"frame"_s); }, Qt::DirectConnection);
        const int notches = valueOf(u"--trace-notches", u"30"_s).toInt();
        for (int i = 0; i < notches; ++i) {
            state(u"notch"_s);
            wheel(direction);
            state(u"notched"_s);
            pumpFor(intervalMs);
        }
        pumpFor(600);
        tracing = false;
        QObject::disconnect(frame);
        QFile file(output.filePath(u"trace-%1.json"_s.arg(label)));
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(events).toJson(QJsonDocument::Compact));
        report(u"bench: trace: %1 events"_s.arg(events.size()));
        return 0;
    }

    // --grab: pictures of the window for a pixel comparison, no timing.
    if (args.contains(u"--grab"_s)) {
        pumpFor(1500);
        window->grabWindow().save(output.filePath(u"grab-%1-top.png"_s.arg(label)));
        for (int i = 0; i < 9; ++i) {
            wheel(-120);
            pumpFor(90);
        }
        pumpFor(1500);
        window->grabWindow().save(output.filePath(u"grab-%1-down.png"_s.arg(label)));
        for (int i = 0; i < 5; ++i) {
            wheel(120);
            pumpFor(90);
        }
        pumpFor(1500);
        window->grabWindow().save(output.filePath(u"grab-%1-back.png"_s.arg(label)));
        // The mouse on the hovered row's first button: its tip shows.
        QQuickItem* button = nullptr;
        for (QQuickItem* row : content->childItems()) {
            if (!row->isVisible() || !row->objectName().startsWith(u"clipboardRow-")
                || !row->mapRectToScene(QRectF(0, 0, row->width(), row->height())).contains(over))
                continue;
            const std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* item) -> QQuickItem* {
                if (item->isVisible() && QByteArray(item->metaObject()->className()).startsWith("RowAction"))
                    return item;
                for (QQuickItem* child : item->childItems()) {
                    if (QQuickItem* found = find(child))
                        return found;
                }
                return nullptr;
            };
            button = find(row);
        }
        if (button) {
            const QPointF at = button->mapToScene(QPointF(button->width() / 2, button->height() / 2));
            QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, {});
            QCoreApplication::sendEvent(window, &move);
            pumpFor(1500);
            window->grabWindow().save(output.filePath(u"grab-%1-tip.png"_s.arg(label)));
        }
        report(u"bench: grabs saved"_s + (button ? u""_s : u" (no button for the tip)"_s));
        return 0;
    }

    struct Phase {
        QString name;
        int notches;
        int intervalMs;
        int delta;
    };
    // --external-ms: no events of its own; a real wheel turned meanwhile (SendInput).
    const int externalMs = valueOf(u"--external-ms", u"0"_s).toInt();
    // --motion: the wheel's glide alone, at several rates, a single notch and
    // a spin into the top.
    const QList<Phase> phases = externalMs > 0 ? QList<Phase> {{u"external"_s, 1, externalMs, 0}}
        : args.contains(u"--motion"_s) ? QList<Phase> {
            {u"single_down"_s, 1, 0, -120}, {u"notch30_down"_s, 40, 30, -120}, {u"notch60_down"_s, 40, 60, -120},
            {u"notch100_down"_s, 30, 100, -120}, {u"notch150_down"_s, 20, 150, -120}, {u"notch180_down"_s, 20, 180, -120}, {u"notch30_up"_s, 20, 30, 120}, {u"notch60_up"_s, 30, 60, 120}, {u"notch100_up"_s, 20, 100, 120}, {u"notch150_up"_s, 15, 150, 120}, {u"spin30_to_top"_s, 200, 30, 120},
        } : QList<Phase> {
            {u"notch80_down"_s, 40, 80, -120}, {u"notch80_up"_s, 40, 80, 120},
            {u"spin16_down"_s, 80, 16, -120}, {u"spin16_up"_s, 80, 16, 120},
        };
    if (externalMs > 0)
        report(u"bench: ready for input"_s);
    QJsonArray phaseResults;
    const double vsyncMs = 1000.0 / std::max(1.0, screen->refreshRate());
    for (const Phase& phase : phases) {
        {
            QMutexLocker lock(&mutex);
            swaps.clear();
            syncStarts.clear();
            syncEnds.clear();
            renderEnds.clear();
            renderCycles.clear();
        }
        samples.clear();
        latencies.clear();
        waiting.clear();
        flips.clear();
        made.clear();
        reused.clear();
        recording = true;
        const qint64 phaseStart = clock.nsecsElapsed();
        const qint64 privateBefore = privateBytes();
        int sent = 0;
        std::vector<qint64> notchAt; // when each notch was sent
        QElapsedTimer timer;
        timer.start();
        {
            // Each notch when its time comes, checked every millisecond
            // (not spinning: see pumpFor).
            QEventLoop loop;
            QTimer tick;
            tick.setTimerType(Qt::PreciseTimer);
            tick.setInterval(1);
            const auto due = [&] {
                while (sent < phase.notches && timer.elapsed() >= qint64(sent) * phase.intervalMs) {
                    if (phase.delta != 0) {
                        notchAt.push_back(clock.nsecsElapsed());
                        wheel(phase.delta);
                    }
                    ++sent;
                }
                if (sent >= phase.notches)
                    loop.quit();
            };
            QObject::connect(&tick, &QTimer::timeout, &loop, due);
            due();
            if (sent < phase.notches) {
                tick.start();
                loop.exec();
            }
        }
        if (phase.delta == 0)
            pumpFor(phase.intervalMs);
        pumpFor(700); // the glide ends, the last thumbnails come
        recording = false;
        const int never = static_cast<int>(waiting.size()); // in view, its thumbnail never shown
        std::vector<double> intervals;
        {
            QMutexLocker lock(&mutex);
            for (std::size_t i = 1; i < swaps.size(); ++i)
                intervals.push_back((swaps[i] - swaps[i - 1]) / 1e6);
        }
        // Frames while moving: from the first sample to the last one whose position changed.
        qint64 movingUntil = phaseStart;
        for (std::size_t i = 1; i < samples.size(); ++i) {
            if (samples[i].contentY != samples[i - 1].contentY)
                movingUntil = samples[i].ns;
        }
        // A missed frame "near" an event: the event came in it or the one before
        // (from a vsync before the previous swap to this swap; "near" is a Windows macro).
        const auto within = [&](const std::vector<qint64>& events, qint64 from, qint64 to) {
            const auto it = std::lower_bound(events.begin(), events.end(), from);
            return it != events.end() && *it <= to;
        };
        int missedNearFlip = 0, missedNearMade = 0, missedNearReuse = 0, framesNearFlip = 0;
        int missed = 0, long33 = 0;
        std::vector<double> moving;
        {
            QMutexLocker lock(&mutex);
            for (std::size_t i = 1; i < swaps.size(); ++i) {
                if (swaps[i] > movingUntil)
                    break;
                const double ms = (swaps[i] - swaps[i - 1]) / 1e6;
                moving.push_back(ms);
                const qint64 from = swaps[i - 1] - qint64(vsyncMs * 1e6);
                const bool flipped = within(flips, from, swaps[i]);
                framesNearFlip += flipped ? 1 : 0;
                if (ms > vsyncMs * 1.5) {
                    const int lost = static_cast<int>(std::lround(ms / vsyncMs)) - 1;
                    missed += lost;
                    missedNearFlip += flipped ? lost : 0;
                    missedNearMade += within(made, from, swaps[i]) ? lost : 0;
                    missedNearReuse += within(reused, from, swaps[i]) ? lost : 0;
                }
                if (ms > 33)
                    ++long33;
            }
        }
        // Where the rows are drawn, along contentY: the first row in view
        // followed from frame to frame (contentY alone also moves when the
        // list re-estimates the rows above it).
        std::vector<double> drawn(samples.size(), 0);
        for (std::size_t i = 1; i < samples.size(); ++i) {
            const bool same = samples[i].anchor >= 0 && samples[i].anchor == samples[i - 1].anchor;
            drawn[i] = drawn[i - 1] + (same ? samples[i - 1].anchorY - samples[i].anchorY : samples[i].contentY - samples[i - 1].contentY);
        }
        // The motion: speed per frame (px/ms) in the middle of the phase,
        // how even it is, how often it jumps up again (a new notch restarting
        // the glide), and frames standing still in between.
        std::vector<double> speeds;
        {
            const qint64 from = phaseStart + (movingUntil - phaseStart) / 5;
            const qint64 to = movingUntil - (movingUntil - phaseStart) / 5;
            for (std::size_t i = 1; i < samples.size(); ++i) {
                if (samples[i].ns < from || samples[i].ns > to)
                    continue;
                speeds.push_back(std::abs(drawn[i] - drawn[i - 1]) / ((samples[i].ns - samples[i - 1].ns) / 1e6));
            }
        }
        double mean = 0, deviation = 0;
        for (const double v : speeds)
            mean += v / std::max<std::size_t>(1, speeds.size());
        for (const double v : speeds)
            deviation += (v - mean) * (v - mean) / std::max<std::size_t>(1, speeds.size());
        int surges = 0, still = 0;
        for (std::size_t i = 1; i < speeds.size(); ++i) {
            surges += speeds[i] > speeds[i - 1] * 1.3 && speeds[i] > 0.05 ? 1 : 0;
            still += speeds[i] == 0 ? 1 : 0;
        }
        // The same over four frames (whole logical pixels make single frames
        // jitter): its evenness while the wheel turns, and how often it speeds
        // up again by a third or more; frames moving against the wheel.
        std::vector<double> windowed;
        {
            const qint64 from = phaseStart + (movingUntil - phaseStart) / 5;
            const qint64 to = movingUntil - (movingUntil - phaseStart) / 5;
            for (std::size_t i = 4; i < samples.size(); i += 4) {
                if (samples[i - 4].ns < from || samples[i].ns > to)
                    continue;
                windowed.push_back(std::abs(drawn[i] - drawn[i - 4]) / ((samples[i].ns - samples[i - 4].ns) / 1e6));
            }
        }
        double mean4 = 0, deviation4 = 0;
        for (const double v : windowed)
            mean4 += v / std::max<std::size_t>(1, windowed.size());
        for (const double v : windowed)
            deviation4 += (v - mean4) * (v - mean4) / std::max<std::size_t>(1, windowed.size());
        int surges4 = 0;
        for (std::size_t i = 1; i < windowed.size(); ++i)
            surges4 += windowed[i] > windowed[i - 1] * 1.3 && windowed[i] > 0.2 ? 1 : 0;
        // How far behind the wheel it runs: against a staircase that takes
        // the phase's mean step at each notch, the mean distance in ms of
        // the mean speed.
        double lagMs = 0;
        if (!notchAt.empty() && samples.size() > 1) {
            const double y0 = drawn.front();
            const double step = std::abs(drawn.back() - y0) / double(notchAt.size());
            double sum = 0, speedSum = 0;
            int count = 0;
            for (std::size_t i = 1; i < samples.size(); ++i) {
                const auto sentBy = std::upper_bound(notchAt.begin(), notchAt.end(), samples[i].ns) - notchAt.begin();
                if (sentBy == 0)
                    continue;
                sum += std::abs(double(sentBy) * step - std::abs(drawn[i] - y0));
                speedSum += std::abs(drawn[i] - drawn[i - 1]) / ((samples[i].ns - samples[i - 1].ns) / 1e6);
                ++count;
            }
            const double speed = speedSum / std::max(1, count);
            lagMs = speed > 0 ? sum / std::max(1, count) / speed : 0;
        }
        // Against the wheel: rows drawn moving back (one row followed), and
        // contentY moving back (also the list re-estimating rows above).
        int backward = 0, backwardContentY = 0;
        double backwardMost = 0;
        const int way = phase.delta < 0 ? 1 : -1; // contentY grows as the wheel turns down
        for (std::size_t i = 1; i < samples.size(); ++i) {
            const bool same = samples[i].anchor >= 0 && samples[i].anchor == samples[i - 1].anchor;
            const double back = same ? -(samples[i - 1].anchorY - samples[i].anchorY) * way : 0;
            backward += back > 0 ? 1 : 0;
            backwardMost = std::max(backwardMost, back);
            backwardContentY += (samples[i].contentY - samples[i - 1].contentY) * way < 0 ? 1 : 0;
        }
        QJsonArray trajectory; // a single notch: ms from the notch, contentY
        if (phase.notches == 1 && phase.delta != 0) {
            for (std::size_t i = 0; i < samples.size(); ++i)
                trajectory.append(QJsonArray {(samples[i].ns - phaseStart) / 1e6, drawn[i]});
        }
        int placeholderFrames = 0, blankIconFrames = 0;
        for (const Sample& s : samples) {
            placeholderFrames += s.placeholders;
            blankIconFrames += s.blankIcons;
        }
        int late = 0;
        for (const double l : latencies)
            late += l > vsyncMs * 1.5 ? 1 : 0;
        // The stages of each frame while moving, in ms: polish (GUI thread,
        // from its animation step to the render thread's sync), sync (GUI
        // blocked) and render (render thread, up to the present call).
        std::vector<double> polishMs, syncMs, renderMs, guiMs;
        {
            QMutexLocker lock(&mutex);
            for (const qint64 start : syncStarts) {
                if (start > movingUntil)
                    break;
                const auto anim = std::upper_bound(samples.begin(), samples.end(), start,
                    [](qint64 t, const Sample& s) { return t < s.ns; });
                const auto end = std::lower_bound(syncEnds.begin(), syncEnds.end(), start);
                if (anim == samples.begin() || end == syncEnds.end())
                    continue;
                const double polish = (start - std::prev(anim)->ns) / 1e6;
                const double sync = (*end - start) / 1e6;
                if (polish > 50)
                    continue; // not this frame's animation step (an idle gap)
                polishMs.push_back(polish);
                syncMs.push_back(sync);
                guiMs.push_back(polish + sync);
                const auto rendered = std::lower_bound(renderEnds.begin(), renderEnds.end(), *end);
                if (rendered != renderEnds.end())
                    renderMs.push_back((*rendered - *end) / 1e6);
            }
        }
        const auto meanOf = [](const std::vector<double>& v) {
            double sum = 0;
            for (const double x : v)
                sum += x;
            return v.empty() ? 0.0 : sum / double(v.size());
        };
        // CPU time per frame, ms: the GUI thread's from one animation step to
        // the next (input, hover, animation, polish), the render thread's
        // from one render to the next (sync, render). Robust to other load.
        std::vector<double> guiCpu, renderCpu;
        for (std::size_t i = 1; i < samples.size(); ++i) {
            if (samples[i].ns > movingUntil)
                break;
            guiCpu.push_back(double(samples[i].guiCycles - samples[i - 1].guiCycles) / cyclesPerMs);
        }
        {
            QMutexLocker lock(&mutex);
            for (std::size_t i = 1; i < renderCycles.size() && i < renderEnds.size(); ++i) {
                if (renderEnds[i] > movingUntil)
                    break;
                renderCpu.push_back(double(renderCycles[i] - renderCycles[i - 1]) / cyclesPerMs);
            }
        }
        const auto countOver = [](const std::vector<double>& v, double ms) {
            return int(std::count_if(v.begin(), v.end(), [ms](double x) { return x > ms; }));
        };
        const QJsonObject stages {
            {u"gui_cpu_ms_mean"_s, meanOf(guiCpu)}, {u"gui_cpu_ms_p95"_s, percentile(guiCpu, 0.95)},
            {u"gui_cpu_ms_p99"_s, percentile(guiCpu, 0.99)}, {u"gui_cpu_ms_max"_s, percentile(guiCpu, 1)},
            {u"gui_cpu_frames_over_1ms"_s, countOver(guiCpu, 1)}, {u"gui_cpu_frames_over_2ms"_s, countOver(guiCpu, 2)},
            {u"render_cpu_ms_mean"_s, meanOf(renderCpu)}, {u"render_cpu_ms_p95"_s, percentile(renderCpu, 0.95)},
            {u"render_cpu_ms_p99"_s, percentile(renderCpu, 0.99)}, {u"render_cpu_frames_over_2ms"_s, countOver(renderCpu, 2)},
            {u"cycles_per_ms"_s, cyclesPerMs},
            {u"polish_ms_mean"_s, meanOf(polishMs)}, {u"polish_ms_p95"_s, percentile(polishMs, 0.95)}, {u"polish_ms_p99"_s, percentile(polishMs, 0.99)},
            {u"sync_ms_mean"_s, meanOf(syncMs)}, {u"sync_ms_p95"_s, percentile(syncMs, 0.95)}, {u"sync_ms_p99"_s, percentile(syncMs, 0.99)},
            {u"gui_ms_mean"_s, meanOf(guiMs)}, {u"gui_ms_p95"_s, percentile(guiMs, 0.95)}, {u"gui_ms_p99"_s, percentile(guiMs, 0.99)},
            {u"render_ms_mean"_s, meanOf(renderMs)}, {u"render_ms_p95"_s, percentile(renderMs, 0.95)}, {u"render_ms_p99"_s, percentile(renderMs, 0.99)},
        };
        const QJsonObject phaseResult {
            {u"stages"_s, stages},
            {u"phase"_s, phase.name},
            {u"frames_moving"_s, int(moving.size())},
            {u"moving_ms"_s, (movingUntil - phaseStart) / 1e6},
            {u"frame_ms_p50"_s, percentile(moving, 0.5)},
            {u"frame_ms_p95"_s, percentile(moving, 0.95)},
            {u"frame_ms_p99"_s, percentile(moving, 0.99)},
            {u"frame_ms_max"_s, percentile(moving, 1)},
            {u"missed_vsyncs"_s, missed},
            {u"frames_over_33ms"_s, long33},
            {u"placeholder_row_frames"_s, placeholderFrames},
            {u"blank_icon_row_frames"_s, blankIconFrames},
            {u"thumbs_seen"_s, int(latencies.size())},
            {u"thumbs_late"_s, late},
            {u"thumbs_never"_s, never},
            {u"thumb_ms_p50"_s, percentile(latencies, 0.5)},
            {u"thumb_ms_p95"_s, percentile(latencies, 0.95)},
            {u"thumb_ms_max"_s, percentile(latencies, 1)},
            {u"private_ws_delta_mb"_s, (privateBytes() - privateBefore) / 1048576.0},
            {u"speed_px_ms_mean"_s, mean},
            {u"speed_cv"_s, mean > 0 ? std::sqrt(deviation) / mean : 0},
            {u"speed_surges"_s, surges},
            {u"still_frames_mid"_s, still},
            {u"end_contentY"_s, list->property("contentY").toDouble()},
            {u"speed4_cv"_s, mean4 > 0 ? std::sqrt(deviation4) / mean4 : 0},
            {u"speed4_surges"_s, surges4},
            {u"speed4_surges_per_notch"_s, double(surges4) / phase.notches},
            {u"backward_frames"_s, backward},
            {u"backward_contentY_frames"_s, backwardContentY},
            {u"backward_px_max"_s, backwardMost},
            {u"lag_ms"_s, lagMs},
            {u"hover_flips"_s, int(flips.size())},
            {u"frames_near_flip"_s, framesNearFlip},
            {u"missed_near_flip"_s, missedNearFlip},
            {u"rows_made"_s, int(made.size())},
            {u"missed_near_made"_s, missedNearMade},
            {u"rows_reused"_s, int(reused.size())},
            {u"missed_near_reuse"_s, missedNearReuse},
            {u"trajectory"_s, trajectory},
        };
        phaseResults.append(phaseResult);
        report(u"bench: %1"_s.arg(QString::fromUtf8(QJsonDocument(phaseResult).toJson(QJsonDocument::Compact))));
    }
    result.insert(u"phases"_s, phaseResults);
    result.insert(u"private_ws_mb"_s, privateBytes() / 1048576.0);
    report(u"bench: private working set %1 MB"_s.arg(privateBytes() / 1048576.0, 0, 'f', 1));
    window->hide();
    QFile out(output.filePath(u"clipscroll-%1.json"_s.arg(label)));
    if (out.open(QIODevice::WriteOnly))
        out.write(QJsonDocument(result).toJson());
    return 0;
}

} // namespace ws::preview
