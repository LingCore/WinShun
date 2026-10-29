#pragma once

#include <QImage>
#include <QList>
#include <QPair>
#include <QPolygonF>
#include <QRect>
#include <QString>
#include <QStringList>

#include <functional>

// Smart redaction (docs/design-capture-roadmap.md, 4.1): from the lines of
// text the reader found (their corners and each character's place along
// them) to the boxes that hide what should not be shown: phone numbers,
// identity card and bank card numbers, e-mail addresses, keys and passwords,
// the names of this computer's user. Plain C++ on plain values, so the tests
// run it without the reader; everything stays on this computer, and nothing
// here writes what it read anywhere.
namespace ws::capture {

struct RedactRules {
    bool phone = true;
    bool idCard = true;
    bool bankCard = true;
    bool email = true;
    bool secret = true;
    bool identity = true;
    bool ipMac = false;
    bool allText = false;
    QStringList words; // the user's own words, whole words
    QStringList names; // this computer's user name, computer name (the caller fills them)
};

// One line the reader found, in image px.
struct RedactLine {
    QPolygonF quad; // top left, top right, bottom right, bottom left
    QString text;
    // Each character: its index in `text` (UTF-16) and where its middle is
    // along the line, 0 (left end) to 1; empty when the reader gave none.
    QList<QPair<int, float>> chars;
};

struct RedactHit {
    QRect box; // image px, with the margin of 4.1 added
    QString category; // phone, id, card, email, secret, identity, ip, word, text: only for counting
};

struct RedactScan {
    QList<RedactLine> lines;
    QImage gray; // the scanned region's grayscale pixels, prepared by the reader thread
};

// The boxes to hide, top to bottom: matches in each line, widened and merged
// where they touch on one line; those that `alreadyCovered` (the boxes of the
// redactions already there) hides by 80 % or more are left out. The reader
// places a character only to about a quarter of an em, so with `gray` (the
// picture the lines came from, Format_Grayscale8, same pixels) the left and
// right edge of a match are moved to the nearest start and end of ink.
QList<RedactHit> findSensitive(const QList<RedactLine>& lines, const RedactRules& rules, const QList<QRect>& alreadyCovered,
    const QImage& gray = {});

// What the rules look at, for the tests.
bool validIdCard(QStringView digits); // 18 digits (last may be X) with its check digit and a real birth date, or 15 digits
bool validLuhn(QStringView digits);
// The box of characters [from, to) of the line's text, image px, without any
// margin; its edges at the ink when `gray` is given (see findSensitive).
QRect matchBox(const RedactLine& line, int from, int to, const QImage& gray = {});

// How a scan is asked for and delivered, so that the model that shows the
// result need not know the reader: the app gives these to the capture and to
// the editing window.
struct SmartHooks {
    // Crops `region` from the shared frozen image and reads it in the
    // background. `done` is called on the GUI thread with the region's
    // lines and grayscale pixels, unless
    // cancel() or a newer scan came first (then it is not called).
    std::function<void(const QImage& image, const QRect& region, double dpr, std::function<void(RedactScan)> done)> scan;
    std::function<void()> cancel;
    std::function<RedactRules()> rules;
    std::function<bool()> prescan; // [Capture] SmartPrescan
};

} // namespace ws::capture
