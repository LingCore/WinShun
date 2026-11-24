#include "CaptureHistory.h"

#include "AnnotationFormat.h"
#include "AnnotationRender.h"

#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>

#include <algorithm>
#include <future>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

constexpr int kVersion = 1;
// QImageWriter's PNG quality is zlib's level backwards: 80 is level 1, the
// fastest that still packs a screen into a few MB.
constexpr int kPngQuality = 80;
const QString kPart = u".part"_s;

QJsonArray rectJson(const QRect& r)
{
    return {r.x(), r.y(), r.width(), r.height()};
}

QRect rectFrom(const QJsonValue& value)
{
    const QJsonArray a = value.toArray();
    return a.size() == 4 ? QRect(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt()) : QRect();
}

bool newerFirst(const QString& a, const QString& b)
{
    return a.size() != b.size() ? a.size() > b.size() : a > b;
}

} // namespace

bool HistoryShot::fits(const std::vector<QRect>& monitors) const
{
    return monitors.size() == screens.size() && std::ranges::all_of(monitors, [&](const QRect& m) {
        return std::ranges::any_of(screens, [&](const Screen& s) { return s.rect == m; });
    });
}

CaptureHistory::~CaptureHistory()
{
    flush();
}

void CaptureHistory::flush()
{
    if (m_writer.joinable())
        m_writer.join();
}

void CaptureHistory::setLimit(int count)
{
    count = std::max(0, count);
    if (count == m_limit)
        return;
    const bool fewer = count < m_limit;
    m_limit = count;
    if (fewer && !m_folder.isEmpty()) {
        m_writer = std::jthread([before = std::move(m_writer), folder = m_folder, count]() mutable {
            if (before.joinable())
                before.join();
            prune(folder, count, {});
        });
    }
}

QStringList CaptureHistory::ids() const
{
    if (!enabled())
        return {};
    QStringList names = QDir(m_folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    names.removeIf([](const QString& n) { return n.endsWith(kPart); });
    std::ranges::sort(names, newerFirst);
    return names;
}

void CaptureHistory::add(HistoryShot shot, const QString& replaces)
{
    if (!enabled() || shot.screens.empty())
        return;
    m_writer = std::jthread([before = std::move(m_writer), folder = m_folder, limit = m_limit, shot = std::move(shot),
                                replaces]() mutable {
        if (before.joinable())
            before.join();
        write(folder, shot);
        prune(folder, limit, replaces);
    });
}

void CaptureHistory::write(const QString& folder, const HistoryShot& shot)
{
    QDir dir(folder);
    const QString part = shot.id + kPart;
    if (!dir.mkpath(part)) {
        qWarning().noquote() << "Capture history: cannot make" << dir.filePath(part);
        return;
    }
    const QDir into(dir.filePath(part));
    QJsonArray screens;
    for (std::size_t i = 0; i < shot.screens.size(); ++i) {
        const HistoryShot::Screen& s = shot.screens[i];
        const QString file = QString::number(i) + u".png"_s;
        // Opaque: written as RGB (a third smaller than RGBA), read back in its own format.
        QImageWriter writer(into.filePath(file), "png");
        writer.setQuality(kPngQuality);
        const QImage opaque = s.image.hasAlphaChannel() ? s.image.convertToFormat(QImage::Format_RGB32) : s.image;
        if (!writer.write(opaque)) {
            qWarning().noquote() << "Capture history: cannot write" << into.filePath(file) << writer.errorString();
            QDir(into).removeRecursively();
            return;
        }
        screens.append(QJsonObject {{u"rect"_s, rectJson(s.rect)}, {u"dpr"_s, s.dpr}, {u"file"_s, file},
            {u"format"_s, static_cast<int>(s.image.format())}});
    }
    if (!shot.document.isEmpty()) {
        QSaveFile doc(into.filePath(u"doc.json"_s));
        if (!doc.open(QIODevice::WriteOnly) || doc.write(shot.document) != shot.document.size() || !doc.commit())
            qWarning().noquote() << "Capture history: cannot write the annotations of" << shot.id;
    }
    QJsonObject meta {{u"version"_s, kVersion}, {u"screens"_s, screens}, {u"screen"_s, shot.screen},
        {u"selection"_s, rectJson(shot.selection)}};
    if (shot.spans)
        meta.insert(u"spans"_s, true);
    QSaveFile file(into.filePath(u"meta.json"_s));
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact)) < 0
        || !file.commit() || !dir.rename(part, shot.id)) {
        qWarning().noquote() << "Capture history: cannot finish" << into.path();
        QDir(into).removeRecursively();
    }
}

void CaptureHistory::prune(const QString& folder, int limit, const QString& replaces)
{
    const QDir dir(folder);
    QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : names) {
        if (name.endsWith(kPart) || (!replaces.isEmpty() && name == replaces)) // a write cut short, or taken again
            QDir(dir.filePath(name)).removeRecursively();
    }
    names.removeIf([&](const QString& n) { return n.endsWith(kPart) || n == replaces; });
    std::ranges::sort(names, newerFirst);
    for (qsizetype i = limit; i < names.size(); ++i)
        QDir(dir.filePath(names[i])).removeRecursively();
}

std::optional<HistoryShot> CaptureHistory::read(const QString& folder, const QString& id)
{
    const QDir dir(QDir(folder).filePath(id));
    QFile file(dir.filePath(u"meta.json"_s));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject meta = QJsonDocument::fromJson(file.readAll()).object();
    if (meta.value(u"version"_s).toInt() != kVersion)
        return std::nullopt;
    HistoryShot shot;
    shot.id = id;
    shot.screen = meta.value(u"screen"_s).toInt(-1);
    shot.selection = rectFrom(meta.value(u"selection"_s));
    shot.spans = meta.value(u"spans"_s).toBool();
    const QJsonArray screens = meta.value(u"screens"_s).toArray();
    std::vector<std::future<QImage>> decoded;
    for (const QJsonValue& value : screens) {
        const QJsonObject s = value.toObject();
        shot.screens.push_back({rectFrom(s.value(u"rect"_s)), s.value(u"dpr"_s).toDouble(1), {}});
        const auto format = static_cast<QImage::Format>(s.value(u"format"_s).toInt(QImage::Format_RGB32));
        // Each monitor's picture on a thread of its own: a 4K PNG takes ~100 ms.
        decoded.push_back(std::async(std::launch::async, [path = dir.filePath(s.value(u"file"_s).toString()), format] {
            QImageReader reader(path, "png");
            reader.setAllocationLimit(0); // a 8K monitor is over Qt's 256 MB default
            QImage image = reader.read();
            return image.isNull() || image.format() == format ? image : image.convertToFormat(format);
        }));
    }
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        shot.screens[i].image = decoded[i].get();
        if (shot.screens[i].image.size() != shot.screens[i].rect.size())
            return std::nullopt;
    }
    if (shot.spans ? shot.screens.size() < 2 : shot.screens.empty() || shot.screen < 0 || shot.screen >= std::ssize(shot.screens))
        return std::nullopt;
    if (QFile doc(dir.filePath(u"doc.json"_s)); doc.open(QIODevice::ReadOnly))
        shot.document = doc.readAll();
    return shot;
}

std::optional<RenderedHistoryShot> renderHistoryShot(const HistoryShot& shot)
{
    if (shot.screens.empty() || shot.selection.isEmpty())
        return std::nullopt;
    QImage surface;
    QPoint surfaceOrigin;
    if (shot.spans) {
        QRect bounds;
        for (const HistoryShot::Screen& s : shot.screens)
            bounds = bounds.united(s.rect);
        if (bounds.isEmpty())
            return std::nullopt;
        const QImage::Format format = shot.screens.front().image.isNull() ? QImage::Format_RGB32 : shot.screens.front().image.format();
        surface = QImage(bounds.size(), format);
        surface.fill(Qt::black);
        QPainter painter(&surface);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        for (const HistoryShot::Screen& s : shot.screens)
            painter.drawImage(s.rect.topLeft() - bounds.topLeft(), s.image);
        painter.end();
        surfaceOrigin = bounds.topLeft();
    } else {
        if (shot.screen < 0 || shot.screen >= std::ssize(shot.screens))
            return std::nullopt;
        const HistoryShot::Screen& s = shot.screens[static_cast<std::size_t>(shot.screen)];
        surface = s.image;
        surfaceOrigin = s.rect.topLeft();
    }
    const QRect area = shot.selection.intersected(QRect(QPoint(0, 0), surface.size()));
    if (area.isEmpty())
        return std::nullopt;
    RenderedHistoryShot out;
    out.origin = surfaceOrigin + area.topLeft();
    if (!shot.document.isEmpty()) {
        if (const auto doc = annot::fromJson(QJsonDocument::fromJson(shot.document).object(), nullptr);
            doc && !doc->items.isEmpty()) {
            const annot::Project packed = annot::projectFromOverlay(*doc, surface, area);
            if (!packed.base.isNull()) {
                annot::Compositor compositor;
                out.image = compositor.exportImage(packed.doc, packed.base, QRect(QPoint(0, 0), packed.base.size()));
                out.project = annot::writeProject(packed.doc, packed.base);
            }
        }
    }
    if (out.image.isNull())
        out.image = surface.copy(area);
    if (out.image.isNull())
        return std::nullopt;
    return out;
}

} // namespace ws::capture
