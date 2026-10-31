#pragma once

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QString>
#include <QStringList>

#include <optional>
#include <thread>
#include <vector>

namespace ws::capture {

// A capture taken, as , and . in the overlay bring it back (截图历史, as in
// PixPin and Snipaste): every monitor's frozen picture, where the selection
// was, and the annotations over it, editable again.
struct HistoryShot {
    struct Screen {
        QRect rect; // physical, virtual desktop
        qreal dpr = 1;
        QImage image; // rect.size()
    };
    QString id; // its folder's name: when it was taken, ms since the epoch
    std::vector<Screen> screens;
    int screen = -1; // the selection's, an index into screens
    QRect selection; // physical, that monitor's own
    QByteArray document; // the annotations' doc.json, on that monitor's whole picture; empty: none

    // Shown on monitors where `monitors` are now (physical, virtual desktop),
    // each by its own picture: the same ones, wherever they are listed.
    bool fits(const std::vector<QRect>& monitors) const;
};

// The last captures on the disk, a folder each (<id>\: meta.json, a PNG per
// monitor, doc.json), newest kept, up to the limit. Written off the GUI
// thread, one after another (a capture's frames take a few hundred ms to
// encode); a folder is renamed into place once whole, so one cut short
// (the program ended meanwhile) is never read, and goes with the next write.
class CaptureHistory {
public:
    CaptureHistory() = default;
    ~CaptureHistory(); // the writes under way finish first
    CaptureHistory(const CaptureHistory&) = delete;
    CaptureHistory& operator=(const CaptureHistory&) = delete;

    void setFolder(const QString& folder) { m_folder = folder; }
    QString folder() const { return m_folder; }
    // How many are kept: 0, none, and the ones there are deleted (after the
    // writes under way).
    void setLimit(int count);
    int limit() const { return m_limit; }
    bool enabled() const { return m_limit > 0 && !m_folder.isEmpty(); }

    // Their ids, newest first, as on the disk now.
    QStringList ids() const;
    // Kept, the oldest beyond the limit deleted; `replaces`, a capture brought
    // back and taken again (the same pictures), goes too.
    void add(HistoryShot shot, const QString& replaces = {});
    // Read back, its pictures decoded: blocking (the caller's own thread).
    static std::optional<HistoryShot> read(const QString& folder, const QString& id);
    // Waits for the writes under way (tests).
    void flush();

private:
    // On the writing thread: the shot's folder, then the ones over the limit.
    static void write(const QString& folder, const HistoryShot& shot);
    static void prune(const QString& folder, int limit, const QString& replaces);

    QString m_folder;
    int m_limit = 10;
    std::jthread m_writer; // each write joins the one before it
};

} // namespace ws::capture
