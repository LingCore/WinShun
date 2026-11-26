// The pin window's text pick (GitHub #258): the pill that says what to do
// stays while the words are being read, then fades off the picture so it
// does not sit on the bottom line of what one wants to pick; a pick brings
// it back for a moment. And its key card (GitHub #343, PinKeyCard): the
// rows, when it shows, where, its picture, and what the pin tells about it.
#include "PinKeyCard.h"
#include "PinPill.h"
#include "PinWindow.h"

#include <QGuiApplication>
#include <QImage>
#include <QScopeGuard>
#include <QTest>

#include <cmath>
#include <functional>

using namespace ws::capture;
using namespace Qt::StringLiterals;

namespace {

QStringList texts(const std::vector<PinKeyRow>& rows)
{
    QStringList list;
    for (const PinKeyRow& row : rows)
        list.append(row.text);
    return list;
}

} // namespace

class TestPinWindow : public QObject {
    Q_OBJECT

private slots:
    void textPillFadesOffTheWords()
    {
        QImage image(480, 240, QImage::Format_RGB32);
        image.fill(Qt::white);
        std::function<void(QList<RedactLine>)> wordsRead; // the reading, answered when the test says
        PinWindow::Host host;
        host.readLines = [&](const QImage&, double, std::function<void(QList<RedactLine>)> done,
                             std::shared_ptr<const std::atomic<bool>>) { wordsRead = std::move(done); };
        PinWindow pin(image, QPoint(40, 40), std::move(host));
        QVERIFY(pin.hwnd());
        QCOMPARE(pin.textHintAlpha(), 0); // nothing said while no text is picked
        pin.beginTextPick();
        QVERIFY(pin.textMode());
        QCOMPARE(pin.textHintAlpha(), 255); // 识别文字中…
        QTest::qWait(2800); // past the moment a ready pill would get: still reading, still there
        QCOMPARE(pin.textHintAlpha(), 255);
        QVERIFY(wordsRead);
        wordsRead({}); // the words came: 拖选文字 · Esc 退出, for a moment
        QCOMPARE(pin.textHintAlpha(), 255);
        QTest::qWait(1200);
        QCOMPARE(pin.textHintAlpha(), 255); // long enough to read
        QTRY_COMPARE_WITH_TIMEOUT(pin.textHintAlpha(), 0, 4000); // then off the words
        pin.endTextPick();
        QVERIFY(!pin.textMode());
        QCOMPARE(pin.textHintAlpha(), 0);
    }

    // Auto-OCR on pin: quiet background scan, brief ready pill when text exists,
    // direct I-beam cursor (Alt overrides to move), Ctrl+A / Ctrl+C / T / Esc /
    // Shift+C instant cached copy, and cache lifecycle across rotate/collapse/Tab.
    void autoOcrDirectSelectionAndCache()
    {
        const DPI_AWARENESS_CONTEXT was = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto restoreDpi = qScopeGuard([was] { ::SetThreadDpiAwarenessContext(was); });
        auto makeLine = [](const QString& text, QRectF box) {
            RedactLine l;
            l.text = text;
            l.quad = QPolygonF(box);
            l.quad.removeLast();
            for (int i = 0; i < text.size(); ++i)
                l.chars.append({i, static_cast<float>((i + 0.5) / text.size())});
            return l;
        };
        const QList<RedactLine> sample {
            makeLine(u"第一行"_s, QRectF(40, 40, 120, 24)),
            makeLine(u"第二行"_s, QRectF(40, 80, 120, 24)),
        };

        // 1. Quiet background scan; empty result shows no pill.
        {
            QImage img(320, 200, QImage::Format_RGB32);
            img.fill(Qt::white);
            std::function<void(QList<RedactLine>)> reply;
            PinWindow::Host host;
            host.readLines = [&](const QImage&, double, std::function<void(QList<RedactLine>)> done,
                                 std::shared_ptr<const std::atomic<bool>>) { reply = std::move(done); };
            PinWindow pin(img, QPoint(40, 40), std::move(host));
            pin.setAutoOcr(true);
            QVERIFY(reply);
            QCOMPARE(pin.textHintAlpha(), 0); // silent while scanning in background
            reply({});
            QVERIFY(pin.hasRecognizedLines());
            QCOMPARE(pin.recognizedLineCount(), 0);
            QCOMPARE(pin.textHintAlpha(), 0); // no pill when picture has no text
        }

        // 2. Auto-OCR with text: ready pill, cursor I-beam / Alt override, Ctrl+A, Ctrl+C, T, Esc, Shift+C, cache.
        {
            POINT at {};
            const bool hasCursor = ::GetCursorPos(&at) != FALSE;
            const QPoint cursor(at.x, at.y);
            QImage img(400, 260, QImage::Format_RGB32);
            img.fill(Qt::white);
            int scans = 0;
            std::function<void(QList<RedactLine>)> reply;
            std::shared_ptr<const std::atomic<bool>> lastCancel;
            QString copiedText;
            bool copiedImage = false;
            bool closed = false;
            PinWindow::Host host;
            host.readLines = [&](const QImage&, double, std::function<void(QList<RedactLine>)> done,
                                 std::shared_ptr<const std::atomic<bool>> cancel) {
                ++scans;
                reply = std::move(done);
                lastCancel = std::move(cancel);
            };
            host.copyText = [&](const QString& text) { copiedText = text; };
            host.copy = [&](PinWindow*) { copiedImage = true; };
            host.closed = [&](PinWindow*) { closed = true; };
            // Place pin so real cursor sits inside line 0 (40..160, 40..64) -> offset (80, 52).
            const QPoint origin = hasCursor ? cursor - QPoint(80, 52) : QPoint(40, 40);
            PinWindow pin(img, origin, std::move(host));
            pin.setAutoOcr(true);
            QCOMPARE(scans, 1);
            QVERIFY(reply);
            reply(sample);
            QVERIFY(pin.hasRecognizedLines());
            QCOMPARE(pin.recognizedLineCount(), 2);
            QCOMPARE(pin.textHintAlpha(), 255);
            QCOMPARE(pin.textPillText(), u"已识别文字，可拖选"_s);

            // Cursor hit-test: I-beam on text, Move when Alt held (only when desktop cursor is readable).
            BYTE keys[256] {};
            QVERIFY(::GetKeyboardState(keys));
            BYTE held[256] {};
            std::copy(std::begin(keys), std::end(keys), std::begin(held));
            const auto restoreKeys = qScopeGuard([&keys] { ::SetKeyboardState(keys); });
            if (hasCursor && pin.pictureRect().topLeft() == cursor - QPoint(80, 52)) {
                held[VK_MENU] = 0;
                QVERIFY(::SetKeyboardState(held));
                QCOMPARE(::SendMessageW(pin.hwnd(), WM_SETCURSOR, reinterpret_cast<WPARAM>(pin.hwnd()),
                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)),
                    LRESULT(TRUE)); // I-beam over recognized text
                held[VK_MENU] = 0x80; // Alt held: fall through to IDC_SIZEALL move cursor
                QVERIFY(::SetKeyboardState(held));
                QCOMPARE(::SendMessageW(pin.hwnd(), WM_SETCURSOR, reinterpret_cast<WPARAM>(pin.hwnd()),
                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)),
                    LRESULT(FALSE));
                held[VK_MENU] = 0;
                QVERIFY(::SetKeyboardState(held));
            }

            // Ctrl+A selects all text; Ctrl+C copies selected text (not image).
            held[VK_CONTROL] = 0x80;
            QVERIFY(::SetKeyboardState(held));
            ::SendMessageW(pin.hwnd(), WM_KEYDOWN, 'A', 0);
            QVERIFY(pin.hasPickedText());
            QCOMPARE(pin.pickedText(), u"第一行\n第二行"_s);
            QCOMPARE(pin.textPillText(), u"Ctrl+C 复制文字"_s);
            ::SendMessageW(pin.hwnd(), WM_KEYDOWN, 'C', 0);
            QCOMPARE(copiedText, u"第一行\n第二行"_s);
            QVERIFY(!copiedImage);
            QCOMPARE(pin.textPillText(), u"已复制选中文字"_s);

            // Pressing T while text is directly selected also copies selected text without entering box mode.
            copiedText.clear();
            held[VK_CONTROL] = 0;
            QVERIFY(::SetKeyboardState(held));
            ::SendMessageW(pin.hwnd(), WM_KEYDOWN, 'T', 0);
            QCOMPARE(copiedText, u"第一行\n第二行"_s);
            QVERIFY(!pin.textMode());

            // First Esc clears direct selection without closing the pin.
            ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
            QVERIFY(!pin.hasPickedText());
            QVERIFY(!closed && pin.hwnd());

            // Pressing T with nothing selected enters explicit box mode immediately (no re-OCR!).
            ::SendMessageW(pin.hwnd(), WM_KEYDOWN, 'T', 0);
            QVERIFY(pin.textMode());
            QCOMPARE(scans, 1);
            QCOMPARE(pin.textPillText(), u"拖选文字 · Esc 退出"_s);
            ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
            QVERIFY(!pin.textMode());
            QVERIFY(pin.hasRecognizedLines()); // cache survives endTextPick()

            // copyAllRecognizedText() copies instantly from cache without re-running OCR.
            copiedText.clear();
            QVERIFY(pin.copyAllRecognizedText());
            QCOMPARE(scans, 1);
            QCOMPARE(copiedText, u"第一行\n第二行"_s);
            QCOMPARE(pin.textPillText(), u"已复制全部文字"_s);

            // Collapse + expand preserves cached lines (no extra OCR).
            pin.collapse(PinForm::Tile);
            pin.expand();
            QVERIFY(pin.hasRecognizedLines());
            QCOMPARE(scans, 1);

            // Translation preserves original side's lines when flipping back with Tab.
            QImage translated(400, 260, QImage::Format_RGB32);
            translated.fill(Qt::yellow);
            QVERIFY(pin.setTranslation(translated, u"Line 1\nLine 2"_s, pin.revision()));
            QCOMPARE(scans, 2); // auto-OCR started for the translated side
            QVERIFY(lastCancel && !lastCancel->load());
            pin.showOtherSide(); // Tab back to original: cancels translated scan & restores original cache!
            QVERIFY(lastCancel->load());
            QVERIFY(pin.hasRecognizedLines());
            QCOMPARE(pin.recognizedLineCount(), 2);
            QCOMPARE(scans, 2);

            // Rotate invalidates cache and triggers a fresh scan; copyAll joins in-flight scan.
            pin.rotate(true);
            QVERIFY(!pin.hasRecognizedLines());
            QCOMPARE(scans, 3);
            copiedText.clear();
            QVERIFY(pin.copyAllRecognizedText());
            QCOMPARE(pin.textPillText(), u"识别文字中…"_s);
            reply({makeLine(u"旋转后"_s, QRectF(10, 10, 80, 20))});
            QCOMPARE(copiedText, u"旋转后"_s);
            QCOMPARE(pin.textPillText(), u"已复制全部文字"_s);

            // Second Esc (no selection, no text mode) closes the pin.
            ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
            QVERIFY(closed);
        }
    }

    // The cursor comes onto a pin: a close button in the corner it came in
    // by; a click there closes the pin. The pin is put under the cursor where
    // it is, and the window only told of the moves: the mouse is not moved.
    void closeButtonInTheCornerTheCursorCameInBy()
    {
        // Physical cursor points, as in the program (per-monitor v2); the offscreen test app is not.
        const DPI_AWARENESS_CONTEXT was = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto restore = qScopeGuard([was] { ::SetThreadDpiAwarenessContext(was); });
        POINT at {};
        QVERIFY(::GetCursorPos(&at));
        const QPoint cursor(at.x, at.y);
        QImage image(400, 300, QImage::Format_RGB32);
        image.fill(Qt::white);
        bool closed = false;
        PinWindow::Host host;
        host.closed = [&](PinWindow*) { closed = true; };
        // In by the right edge, low down: the lower right corner.
        {
            PinWindow pin(image, cursor - QPoint(395, 250), host);
            if (pin.pictureRect().topLeft() != cursor - QPoint(395, 250))
                QSKIP("the pin was kept in the work area: the cursor is near a screen edge");
            QVERIFY(pin.closeButton().isEmpty()); // none before the cursor comes
            ::SendMessageW(pin.hwnd(), WM_MOUSEMOVE, 0, 0);
            const QRect button = pin.closeButton();
            QVERIFY(!button.isEmpty());
            QVERIFY(button.right() > pin.pictureRect().center().x());
            QVERIFY(button.bottom() > pin.pictureRect().center().y());
            ::SendMessageW(pin.hwnd(), WM_MOUSELEAVE, 0, 0);
            QVERIFY(pin.closeButton().isEmpty()); // gone with the cursor
        }
        closed = false;
        // Under the cursor in the upper left: the button is there, and a click on it closes the pin.
        {
            const int in = 20; // inside the button (8 to 32 px in, times the scale) at any scale
            PinWindow pin(image, cursor - QPoint(in, in), host);
            const QRect picture = pin.pictureRect();
            if (picture.topLeft() != cursor - QPoint(in, in))
                QSKIP("the pin was kept in the work area: the cursor is near a screen edge");
            ::SendMessageW(pin.hwnd(), WM_MOUSEMOVE, 0, 0);
            QVERIFY2(pin.closeButton().contains(cursor),
                qPrintable(u"button %1,%2 %3x%4, picture %5,%6, cursor %7,%8, dpr %9"_s.arg(pin.closeButton().x())
                        .arg(pin.closeButton().y()).arg(pin.closeButton().width()).arg(pin.closeButton().height())
                        .arg(picture.x()).arg(picture.y()).arg(cursor.x()).arg(cursor.y()).arg(pin.dpr())));
            const int inset = static_cast<int>(std::lround(8 * pin.dpr()));
            QCOMPARE(pin.closeButton().topLeft(), picture.topLeft() + QPoint(inset, inset));
            ::SendMessageW(pin.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, 0);
            QVERIFY(!closed); // on the button up
            ::SendMessageW(pin.hwnd(), WM_LBUTTONUP, 0, 0);
            QVERIFY(closed);
            QVERIFY(!pin.hwnd());
        }
    }

    // 截图翻译 (GitHub #191): the translation over the original, the original its other side.
    void translationAndItsOtherSide()
    {
        QImage original(320, 200, QImage::Format_RGB32);
        original.fill(Qt::white);
        QImage translated(320, 200, QImage::Format_RGB32);
        translated.fill(Qt::yellow);
        PinWindow pin(original, QPoint(40, 40), PinWindow::Host {});
        QVERIFY(pin.hwnd());
        QVERIFY(!pin.hasOtherSide() && !pin.translationShown());
        pin.showOtherSide(); // none: nothing
        QCOMPARE(pin.image(), original);
        pin.note(QStringLiteral("…"), true);

        const int revision = pin.revision();
        QVERIFY(!pin.setTranslation(translated.copy(0, 0, 100, 100), QStringLiteral("x"), revision)); // not this picture's size
        QVERIFY(!pin.setTranslation(translated, QStringLiteral("x"), revision + 1)); // not this picture any more
        QVERIFY(!pin.hasOtherSide());
        QVERIFY(pin.setTranslation(translated, QStringLiteral("words"), revision));
        QVERIFY(pin.hasOtherSide() && pin.translationShown());
        QCOMPARE(pin.image(), translated);
        QCOMPARE(pin.translatedWords(), QStringLiteral("words"));
        const QRect place = pin.pictureRect();
        pin.showOtherSide();
        QVERIFY(pin.hasOtherSide() && !pin.translationShown());
        QCOMPARE(pin.image(), original);
        QCOMPARE(pin.pictureRect(), place);
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_TAB, 0); // Tab turns it over
        QVERIFY(pin.translationShown());
        QCOMPARE(pin.image(), translated);
        QCOMPARE(pin.revision(), revision); // turning it over is not another picture

        pin.flip(true); // another picture now: one side
        QVERIFY(!pin.hasOtherSide() && !pin.translationShown() && pin.translatedWords().isEmpty());
        QVERIFY(pin.revision() != revision);
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_TAB, 0);
        QVERIFY(!pin.translationShown());
        QVERIFY(pin.setTranslation(translated, QStringLiteral("again"), pin.revision()));
        pin.rotate(true);
        QVERIFY(!pin.hasOtherSide());
        QVERIFY(pin.setTranslation(translated.transformed(QTransform().rotate(90)), QStringLiteral("again"), pin.revision()));
        pin.setPicture(original, {});
        QVERIFY(!pin.hasOtherSide());
    }

    // Copied words pinned (GitHub #388) are copied as words until edited: a picture then.
    void copiedWordsGoWithAnEdit()
    {
        QImage card(200, 60, QImage::Format_RGB32);
        card.fill(Qt::white);
        PinWindow pin(card, QPoint(40, 40), PinWindow::Host {});
        QVERIFY(pin.copiedText().isEmpty());
        pin.setCopiedText(QStringLiteral("原文"));
        pin.rotate(true); // still the words, turned
        QCOMPARE(pin.copiedText(), QStringLiteral("原文"));
        pin.setPicture(card, {});
        QVERIFY(pin.copiedText().isEmpty());
    }

    // The card's rows: the user's keys in the menu's words; a row with no key left out.
    void keyCardRows()
    {
        CaptureKeys keys;
        std::vector<PinKeyRow> rows = pinKeyRows(keys, false, false, false);
        QCOMPARE(texts(rows), QStringList({u"编辑"_s, u"复制"_s, u"保存到“屏幕截图”"_s, u"识别公式"_s, u"旋转与翻转"_s,
                                  u"缩放 / 透明度"_s, u"收成小块"_s, u"关闭"_s}));
        QCOMPARE(rows[0].keys, QStringList {keys.text(KeyAction::PinEdit)});
        QCOMPARE(rows[1].keys, QStringList {keys.text(KeyAction::PinCopy)});
        QCOMPARE(rows[3].keys, QStringList {keys.text(KeyAction::PinFormula)});
        QCOMPARE(rows[4].keys, QStringList({keys.text(KeyAction::PinRotateRight), keys.text(KeyAction::PinFlipHorizontal),
                                   keys.text(KeyAction::PinFlipVertical)}));
        QCOMPARE(rows[5].keys, QStringList({u"滚轮"_s, u"Ctrl+滚轮"_s}));
        QCOMPARE(rows[7].keys, QStringList {keys.text(KeyAction::PinClose)});

        rows = pinKeyRows(keys, true, true, true); // text recognition there, a translated pin showing its translation
        QCOMPARE(texts(rows), QStringList({u"编辑"_s, u"复制"_s, u"保存到“屏幕截图”"_s, u"识别文字"_s, u"复制全部文字"_s, u"选择文字"_s,
                                  u"识别公式"_s, u"显示原文"_s, u"旋转与翻转"_s, u"缩放 / 透明度"_s, u"收成小块"_s, u"关闭"_s}));
        QCOMPARE(rows[4].keys, QStringList {keys.text(KeyAction::PinOcrCopy)});
        QCOMPARE(rows[7].keys, QStringList {u"Tab"_s});
        QCOMPARE(texts(pinKeyRows(keys, false, true, false))[4], u"显示译文"_s);

        keys.set(KeyAction::PinCopy, QKeyCombination()); // cleared in the settings
        keys.set(KeyAction::PinFlipHorizontal, QKeyCombination());
        keys.set(KeyAction::PinFlipVertical, QKeyCombination());
        keys.set(KeyAction::PinEdit, QKeyCombination(Qt::Key_F2));
        rows = pinKeyRows(keys, false, false, false);
        QCOMPARE(texts(rows), QStringList({u"编辑"_s, u"保存到“屏幕截图”"_s, u"识别公式"_s, u"旋转与翻转"_s, u"缩放 / 透明度"_s,
                                  u"收成小块"_s, u"关闭"_s}));
        QCOMPARE(rows[0].keys, QStringList {u"F2"_s});
        QCOMPARE(rows[3].keys, QStringList {keys.text(KeyAction::PinRotateRight)});
        keys.set(KeyAction::PinRotateRight, QKeyCombination());
        QVERIFY(!texts(pinKeyRows(keys, false, false, false)).contains(u"旋转与翻转"_s));
    }

    // When it shows by itself (PinHintPolicy's rules), on a clock of the test's.
    void keyCardTiming()
    {
        PinHintPolicy p;
        QCOMPARE(p.nextDeadline(), -1); // no pin with the focus
        p.focus(0, false);
        QCOMPARE(p.nextDeadline(), 400);
        p.tick(399);
        QVERIFY(!p.shown());
        p.press(100); // a button held (a drag): not while it is
        QCOMPARE(p.nextDeadline(), -1);
        p.tick(1000);
        QVERIFY(!p.shown());
        p.release(1000);
        QCOMPARE(p.nextDeadline(), 1400);
        p.input(1200); // a key: the rest starts again
        QCOMPARE(p.nextDeadline(), 1600);
        p.tick(1600);
        QVERIFY(p.shown());
        QCOMPARE(p.nextDeadline(), 1600 + PinHintPolicy::kShownMs);
        p.input(2000); // a key takes it away
        QVERIFY(!p.shown());
        p.tick(20'000);
        QVERIFY(!p.shown()); // once a round
        QCOMPARE(p.nextDeadline(), -1);

        p.focus(30'000, false); // a new round; left alone, it goes after kShownMs
        p.tick(30'400);
        QVERIFY(p.shown());
        p.tick(38'399);
        QVERIFY(p.shown());
        p.tick(38'400);
        QVERIFY(!p.shown());

        p.focus(40'000, false); // a button takes it away
        p.tick(40'400);
        QVERIFY(p.shown());
        p.press(40'500);
        QVERIFY(!p.shown());
        p.release(40'600);
        p.tick(50'000);
        QVERIFY(!p.shown());

        p.focus(60'000, false); // the focus gone before it showed, back from the pin's own menu: the same round
        p.blur();
        p.tick(61'000);
        QVERIFY(!p.shown());
        QCOMPARE(p.nextDeadline(), -1);
        p.focus(62'000, true);
        p.tick(62'400);
        QVERIFY(p.shown());
        p.input(62'500); // a key: over
        p.blur();
        QVERIFY(!p.shown());
        p.focus(63'000, true); // shown this round already
        p.tick(64'000);
        QVERIFY(!p.shown());
        p.focus(65'000, false); // another window's focus: a new round
        p.tick(65'400);
        QVERIFY(p.shown());

        p.focus(70'000, false); // ?: until ? again, no timer, keys leave it
        p.toggle();
        QVERIFY(p.shown());
        QCOMPARE(p.nextDeadline(), -1);
        p.input(70'100);
        p.tick(90'000);
        QVERIFY(p.shown());
        p.toggle();
        QVERIFY(!p.shown());
        p.tick(100'000);
        QVERIFY(!p.shown()); // hidden so: nothing more by itself this round
        p.focus(110'000, false);
        p.toggle();
        p.press(110'100); // a button
        QVERIFY(!p.shown());
        p.focus(120'000, false); // ? on the one that showed by itself
        p.tick(120'400);
        QVERIFY(p.shown());
        p.toggle();
        QVERIFY(!p.shown());

        p.setEnabled(false); // off in the settings: only ?
        p.focus(130'000, false);
        p.tick(140'000);
        QVERIFY(!p.shown());
        QCOMPARE(p.nextDeadline(), -1);
        p.toggle();
        QVERIFY(p.shown());
        p.setEnabled(true);
        p.focus(150'000, false); // collapsed, hidden: gone for the round
        p.stop();
        p.tick(151'000);
        QVERIFY(!p.shown());
    }

    // The focus coming and going by itself, nothing pressed (GitHub #343: at
    // a fresh process's first pin the card went 0.3 s after it showed, the
    // focus still on the pin). A card that shows stays when the pin is told
    // again that it has the focus, hides while the focus is with one of the
    // app's own windows and is back with it for the rest of its time; only
    // another program's window begins a new round.
    void keyCardFocusComesAndGoes()
    {
        PinHintPolicy p;
        p.focus(0, false);
        p.tick(400);
        QVERIFY(p.shown());
        p.focus(700, true); // told again
        QVERIFY(p.shown());
        p.tick(1000);
        QVERIFY(p.shown());
        QCOMPARE(p.nextDeadline(), 400 + PinHintPolicy::kShownMs);
        p.blur(); // to one of the app's own windows and back
        QVERIFY(!p.shown());
        p.tick(1200);
        p.focus(1300, true);
        QVERIFY(p.shown());
        QCOMPARE(p.nextDeadline(), 400 + PinHintPolicy::kShownMs); // its time, not a new one
        p.blur(); // away past its time: not back
        p.tick(400 + PinHintPolicy::kShownMs);
        p.focus(9000, true);
        p.tick(9500);
        QVERIFY(!p.shown());
        QCOMPARE(p.nextDeadline(), -1);
        p.focus(10'000, false); // another program's window: a new round, after the rest
        QVERIFY(!p.shown());
        p.tick(10'400);
        QVERIFY(p.shown());
        p.press(10'500); // a button ends it: not back after a trip through the app's own window
        p.release(10'600);
        p.blur();
        p.focus(11'000, true);
        p.tick(12'000);
        QVERIFY(!p.shown());

        p.focus(20'000, false); // ? likewise: hidden while the focus is away, back with it
        p.toggle();
        QVERIFY(p.shown());
        p.blur();
        QVERIFY(!p.shown());
        p.focus(21'000, true);
        QVERIFY(p.shown());
        p.focus(22'000, false); // a new round: gone, then by itself after the rest
        QVERIFY(!p.shown());
        p.tick(22'400);
        QVERIFY(p.shown());

        p.focus(30'000, false); // stop() (collapsed, edited) while the focus is away: not back
        p.tick(30'400);
        QVERIFY(p.shown());
        p.blur();
        p.stop();
        p.focus(31'000, true);
        p.tick(32'000);
        QVERIFY(!p.shown());
    }

    // The bottom-left corner of the pin's monitor, the bottom-right one when the pin is there.
    void keyCardPlace()
    {
        const QRect work(0, 0, 1920, 1040);
        const QSize card(300, 200);
        QCOMPARE(pinKeyCardPlace(card, work, QRect(800, 300, 300, 200), 16, 12), QRect(16, 824, 300, 200));
        QCOMPARE(pinKeyCardPlace(card, work, QRect(0, 700, 320, 200), 16, 12), QRect(1604, 824, 300, 200));
        QCOMPARE(pinKeyCardPlace(card, work, QRect(328, 700, 300, 200), 16, 12).left(), 16); // clear of the gap
        QCOMPARE(pinKeyCardPlace(card, work, QRect(327, 700, 300, 200), 16, 12).left(), 1604); // within it
        QVERIFY(pinKeyCardPlace(card, work, QRect(0, 900, 1920, 140), 16, 12).isEmpty()); // across the bottom: none
        QCOMPARE(pinKeyCardPlace(card, QRect(-1920, 40, 1920, 1000), QRect(), 16, 12), QRect(-1904, 824, 300, 200));
    }

    // Its picture at Windows' scales: whole device pixels, the edge as thick as a line of the style.
    void keyCardPicture()
    {
        const std::vector<PinKeyRow> rows = pinKeyRows(CaptureKeys(), true, false, false);
        for (const bool dark : {false, true}) {
            const PinKeyCard::Look look = PinKeyCard::Look::fallback(dark);
            for (const double dpr : {1.0, 1.25, 1.5, 2.0}) {
                const QSize card = PinKeyCard::cardSize(rows, dpr, look);
                const int s = PinKeyCard::shadowMargin(dpr);
                const QImage picture = PinKeyCard::picture(rows, dpr, look);
                QCOMPARE(picture.size(), card + QSize(2 * s, 2 * s));
                const int edge = std::max(1, static_cast<int>(std::lround(dpr)));
                const int y = s + card.height() / 2;
                QCOMPARE(picture.pixelColor(s + edge, y).rgba(), look.background.rgba()); // the body right inside the edge
                const QColor line = picture.pixelColor(s + edge - 1, y);
                QCOMPARE(line.alpha(), 255);
                QVERIFY(line.rgba() != look.background.rgba());
                QVERIFY(picture.pixelColor(s - 1, y).alpha() < 255); // the shadow
                QCOMPARE(picture.pixelColor(0, 0).alpha(), 0);
                QCOMPARE(picture.pixelColor(s + card.width() / 2, s + card.height() - edge).alpha(), 255);
            }
        }
    }

    // Its type is a menu's at the 界面大小, as the capture's card: a row as
    // tall as its keycap or its words and 6 px, the footer as a keycap.
    void keyCardType()
    {
        const std::vector<PinKeyRow> rows = pinKeyRows(CaptureKeys(), true, false, false);
        const int n = static_cast<int>(rows.size());
        const auto height = [n](int row, int key) { return n * row + (n - 1) * 6 + 2 * 8 + 1 + key + 2 * 10; };
        PinKeyCard::Look look = PinKeyCard::Look::fallback(false);
        QCOMPARE(PinKeyCard::cardSize(rows, 1.0, look).height(), height(20, 20)); // 紧凑: 14 / 12 px, keycaps 20
        const int compactWidth = PinKeyCard::cardSize(rows, 1.0, look).width();
        look.keyHeight = 18; // 密集: the words' row is still 20
        QCOMPARE(PinKeyCard::cardSize(rows, 1.0, look).height(), height(20, 18));
        look.textSize = 18; // 宽松
        look.captionSize = 16;
        look.keyHeight = 24;
        const QSize roomy = PinKeyCard::cardSize(rows, 1.0, look);
        QCOMPARE(roomy.height(), height(24, 24));
        QVERIFY(roomy.width() > compactWidth);
        QCOMPARE(PinKeyCard::cardSize(rows, 2.0, look).height(), 2 * height(24, 24)); // whole device pixels at 200 %
    }

    // The window: in a corner, through to what is under it, never the focus.
    void keyCardWindow()
    {
        const std::vector<PinKeyRow> rows = pinKeyRows(CaptureKeys(), false, false, false);
        const PinKeyCard::Look look = PinKeyCard::Look::fallback(true);
        const HWND before = ::GetForegroundWindow();
        const QRect work(100, 100, 1200, 800);
        PinKeyCard card;
        QVERIFY(!card.visible());
        QVERIFY(card.show(rows, work, QRect(), 1.0, look));
        QVERIFY(card.visible());
        const QSize size = PinKeyCard::cardSize(rows, 1.0, look);
        QCOMPARE(card.rect(), QRect(116, 900 - 16 - size.height(), size.width(), size.height()));
        const LONG_PTR style = ::GetWindowLongPtrW(card.hwnd(), GWL_EXSTYLE);
        for (const LONG_PTR bit : std::initializer_list<LONG_PTR> {WS_EX_LAYERED, WS_EX_TRANSPARENT, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST})
            QVERIFY(style & bit);
        QCOMPARE(::GetForegroundWindow(), before);
        QVERIFY(::AreDpiAwarenessContextsEqual(::GetWindowDpiAwarenessContext(card.hwnd()),
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
        QVERIFY(card.show(rows, work, QRect(100, 700, 400, 200), 1.0, look)); // the pin at the bottom left
        QCOMPARE(card.rect().right(), work.right() - 16);
        QVERIFY(card.show(rows, work, QRect(), 1.5, look)); // another scale: drawn again at its size
        QCOMPARE(card.rect().size(), PinKeyCard::cardSize(rows, 1.5, look));
        QVERIFY(!card.show(rows, work, QRect(100, 850, 1200, 50), 1.0, look)); // no corner free
        QVERIFY(!card.visible());
    }

    // What the pin tells its host about the card: the focus, buttons, keys, and ? or F1.
    void keyCardInput()
    {
        QImage image(200, 120, QImage::Format_RGB32);
        image.fill(Qt::white);
        QStringList told;
        PinWindow::Host host;
        host.focus = [&](PinWindow*, bool focused, bool own) { told.append((focused ? u"focus"_s : u"blur"_s) + (own ? u" own"_s : u""_s)); };
        host.input = [&](PinWindow*, PinInput input) {
            told.append(input == PinInput::Press ? u"press"_s : input == PinInput::Release ? u"release"_s : u"key"_s);
        };
        host.toggleHints = [&](PinWindow*) { told.append(u"toggle"_s); };
        PinWindow pin(image, QPoint(40, 40), std::move(host));
        QVERIFY(pin.hwnd());
        told.clear(); // its own activation when made
        ::SendMessageW(pin.hwnd(), WM_ACTIVATE, WA_INACTIVE, 0);
        ::SendMessageW(pin.hwnd(), WM_ACTIVATE, WA_CLICKACTIVE, reinterpret_cast<LPARAM>(pin.hwnd())); // from a window of the app
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_F1, 0);
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_SHIFT, 0); // alone: nothing (Shift+/ is ?)
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_LEFT, 0);
        ::SendMessageW(pin.hwnd(), WM_RBUTTONDOWN, 0, MAKELPARAM(20, 20));
        ::SendMessageW(pin.hwnd(), WM_RBUTTONUP, 0, MAKELPARAM(20, 20));
        ::SendMessageW(pin.hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(100, 100));
        QCOMPARE(told, QStringList({u"blur"_s, u"focus own"_s, u"toggle"_s, u"key"_s, u"press"_s, u"release"_s, u"key"_s}));

        CaptureKeys keys; // F1 given to a pin action: that, not the card
        keys.set(KeyAction::PinRotateRight, QKeyCombination(Qt::Key_F1));
        pin.setKeys(keys);
        told.clear();
        const QSize before = pin.image().size();
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_F1, 0);
        QCOMPARE(told, QStringList {u"key"_s});
        QCOMPARE(pin.image().size(), before.transposed());
    }

    // PinWindow tracks historyId/sourcePath, preserves fullOrigin while collapsed,
    // and fires Host::closing before releasing pixels so PinHistory can snapshot it (#446, #427).
    void closingHookAndSourceMetadata()
    {
        QImage image(120, 80, QImage::Format_RGB32);
        image.fill(QColor(0x33, 0x66, 0x99));
        QImage imageAtClosing;
        QPoint originAtClosing;
        QString sourceAtClosing;
        quint64 historyIdAtClosing = 0;
        QStringList events;

        PinWindow::Host host;
        host.closing = [&](PinWindow* p) {
            events.append(u"closing"_s);
            imageAtClosing = p->image();
            originAtClosing = p->fullOrigin();
            sourceAtClosing = p->sourcePath();
            historyIdAtClosing = p->historyId();
        };
        host.closed = [&](PinWindow*) { events.append(u"closed"_s); };

        PinWindow pin(image, QPoint(140, 160), std::move(host));
        QVERIFY(pin.hwnd());
        pin.setHistoryId(77);
        pin.setSourcePath(u"C:\\pics\\a.png"_s);
        QCOMPARE(pin.historyId(), 77ULL);
        QCOMPARE(pin.sourcePath(), u"C:\\pics\\a.png"_s);
        QCOMPARE(pin.fullOrigin(), QPoint(140, 160));

        // Collapse to tile and verify fullOrigin still reports the uncollapsed location.
        pin.collapse(PinForm::Tile);
        QCOMPARE(pin.form(), PinForm::Tile);
        QCOMPARE(pin.fullOrigin(), QPoint(140, 160));

        pin.close();
        QCOMPARE(events, QStringList({u"closing"_s, u"closed"_s}));
        QCOMPARE(imageAtClosing.size(), QSize(120, 80));
        QCOMPARE(originAtClosing, QPoint(140, 160));
        QCOMPARE(sourceAtClosing, u"C:\\pics\\a.png"_s);
        QCOMPARE(historyIdAtClosing, 77ULL);
    }

    // A tab on the top edge is dragged along that edge (GitHub #456): never off
    // it or past the screen's ends, beside another pin's tab when let go of over
    // one; a click still opens it, where the picture was.
    void edgeTabSlidesAlongTheTopEdge()
    {
        const DPI_AWARENESS_CONTEXT was = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto restore = qScopeGuard([was] { ::SetThreadDpiAwarenessContext(was); });
        QImage image(400, 300, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPoint cursor; // the test's own pointer: no mouse is moved
        std::vector<QRect> others;
        PinWindow::Host host;
        host.cursor = [&] { return cursor; };
        host.takenTabs = [&](const PinWindow*) { return others; };
        PinWindow pin(image, QPoint(300, 200), std::move(host));
        QVERIFY(pin.hwnd());
        const QPoint full = pin.pictureRect().topLeft();
        pin.collapse(PinForm::Edge);
        const QRect tab = *pin.edgeTab();
        MONITORINFO info {sizeof info};
        const POINT on {tab.center().x(), tab.center().y()};
        QVERIFY(::GetMonitorInfoW(::MonitorFromPoint(on, MONITOR_DEFAULTTONEAREST), &info));
        const QRect work(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top);
        QCOMPARE(tab.top(), work.top());
        const auto drag = [&](QPoint to) {
            ::SendMessageW(pin.hwnd(), WM_MOUSELEAVE, 0, 0); // no close button under the press
            cursor = pin.edgeTab()->center();
            ::SendMessageW(pin.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, 0);
            cursor = to;
            ::SendMessageW(pin.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, 0);
        };
        const auto drop = [&] { ::SendMessageW(pin.hwnd(), WM_LBUTTONUP, 0, 0); };

        // Right and down: along the edge only, and still a tab when let go of.
        drag(tab.center() + QPoint(200, 150));
        QCOMPARE(pin.edgeTab()->topLeft(), tab.topLeft() + QPoint(200, 0));
        drop();
        QCOMPARE(pin.form(), PinForm::Edge);
        QCOMPARE(pin.edgeTab()->topLeft(), tab.topLeft() + QPoint(200, 0));

        // Past the screen's ends: stops at them.
        drag(QPoint(work.left() - 500, work.top()));
        QCOMPARE(pin.edgeTab()->left(), work.left());
        drop();
        drag(QPoint(work.right() + 500, work.top()));
        QCOMPARE(pin.edgeTab()->right(), work.right());
        drop();

        // Over another pin's tab while dragged; let go of there, it goes beside it.
        const QRect other(work.left() + 600, work.top(), tab.width(), tab.height());
        others = {other};
        drag(other.center());
        QCOMPARE(*pin.edgeTab(), other);
        drop();
        QCOMPARE(pin.edgeTab()->left(), other.right() + 1 + 4);
        QCOMPARE(pin.form(), PinForm::Edge);

        // A click opens it where the picture was.
        ::SendMessageW(pin.hwnd(), WM_MOUSELEAVE, 0, 0);
        cursor = pin.edgeTab()->center();
        ::SendMessageW(pin.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, 0);
        drop();
        QCOMPARE(pin.form(), PinForm::Full);
        QCOMPARE(pin.pictureRect().topLeft(), full);
    }

    // Floating status/text pill geometry: outside the pin whenever there is
    // room on any edge, never overlapping the picture or another pill, falling
    // back inside only when the pin spans the whole work area.
    void pillPlace()
    {
        const QRect work(0, 0, 1920, 1040);
        const QSize pill(100, 26);
        constexpr int border = 1;
        constexpr int gap = 6;
        constexpr int inset = 8;

        // Small strip pin in the middle of the screen: top pill sits above, bottom sits below.
        const QRect strip(300, 300, 480, 64);
        const PinPillPlace top = placePinPill(strip, border, pill, work, PinPillEdge::Top, gap, inset);
        QVERIFY(!top.inside);
        QCOMPARE(top.rect, QRect(299, 267, 100, 26));
        QVERIFY(!top.rect.intersects(strip));

        const PinPillPlace bottom = placePinPill(strip, border, pill, work, PinPillEdge::Bottom, gap, inset, top.rect);
        QVERIFY(!bottom.inside);
        QCOMPARE(bottom.rect, QRect(299, 371, 100, 26));
        QVERIFY(!bottom.rect.intersects(strip));
        QVERIFY(!bottom.rect.intersects(top.rect));

        // Tiny pin smaller than the pill itself: still floats cleanly above without clipping or dropping.
        const QRect tiny(500, 400, 48, 24);
        const PinPillPlace tinyTop = placePinPill(tiny, border, pill, work, PinPillEdge::Top, gap, inset);
        QVERIFY(!tinyTop.inside);
        QCOMPARE(tinyTop.rect, QRect(499, 367, 100, 26));
        QVERIFY(!tinyTop.rect.intersects(tiny));

        // Flush against the top edge: top pill flips below, and a bottom pill slides right beside it.
        const QRect atTop(300, 0, 480, 64);
        const PinPillPlace topFlipped = placePinPill(atTop, border, pill, work, PinPillEdge::Top, gap, inset);
        QVERIFY(!topFlipped.inside);
        QCOMPARE(topFlipped.rect, QRect(299, 71, 100, 26));
        const PinPillPlace bottomBeside = placePinPill(atTop, border, QSize(140, 26), work, PinPillEdge::Bottom, gap, inset, topFlipped.rect);
        QVERIFY(!bottomBeside.inside);
        QCOMPARE(bottomBeside.rect.top(), topFlipped.rect.top());
        QCOMPARE(bottomBeside.rect.left(), topFlipped.rect.left() + topFlipped.rect.width() + gap);
        QVERIFY(!bottomBeside.rect.intersects(atTop));
        QVERIFY(!bottomBeside.rect.intersects(topFlipped.rect));

        // Flush against the bottom edge: bottom pill flips above.
        const QRect atBottom(300, 976, 480, 64);
        const PinPillPlace bottomFlipped = placePinPill(atBottom, border, pill, work, PinPillEdge::Bottom, gap, inset);
        QVERIFY(!bottomFlipped.inside);
        QCOMPARE(bottomFlipped.rect, QRect(299, 943, 100, 26));

        // Near the right edge: clamped horizontally inside the work area.
        const QRect nearRight(1870, 300, 40, 40);
        const PinPillPlace clamped = placePinPill(nearRight, border, pill, work, PinPillEdge::Top, gap, inset);
        QVERIFY(!clamped.inside);
        QCOMPARE(clamped.rect.right(), work.right());
        QVERIFY(!clamped.rect.intersects(nearRight));

        // Tall full-height sidebar pin: no room above or below, so it floats beside on the right.
        const QRect tall(400, 0, 300, 1040);
        const PinPillPlace tallRight = placePinPill(tall, border, pill, work, PinPillEdge::Top, gap, inset);
        QVERIFY(!tallRight.inside);
        QCOMPARE(tallRight.rect, QRect(707, 0, 100, 26));
        QVERIFY(!tallRight.rect.intersects(tall));

        // Tall full-height pin flush with right edge: floats beside on the left.
        const QRect tallAtRight(1620, 0, 300, 1040);
        const PinPillPlace tallLeft = placePinPill(tallAtRight, border, pill, work, PinPillEdge::Top, gap, inset);
        QVERIFY(!tallLeft.inside);
        QCOMPARE(tallLeft.rect, QRect(1513, 0, 100, 26));
        QVERIFY(!tallLeft.rect.intersects(tallAtRight));

        // Fullscreen pin: falls back inside and clears the top-left close button.
        const QRect close(8, 8, 24, 24);
        const PinPillPlace inside = placePinPill(work, border, pill, work, PinPillEdge::Top, gap, inset, {}, close);
        QVERIFY(inside.inside);
        QCOMPARE(inside.rect, QRect(40, 8, 100, 26));
        QVERIFY(!inside.rect.intersects(close));

        // Oversized zoomed pin extending off-screen: inside fallback stays within the visible work area.
        const QRect huge(-400, -300, 2800, 1800);
        const PinPillPlace hugePlace = placePinPill(huge, border, pill, work, PinPillEdge::Top, gap, inset);
        QVERIFY(hugePlace.inside);
        QVERIFY(work.contains(hugePlace.rect));
        QCOMPARE(hugePlace.rect.topLeft(), QPoint(8, 8));

        // Negative-coordinate secondary monitor.
        const QRect leftMonitor(-1920, 40, 1920, 1000);
        const PinPillPlace neg = placePinPill(QRect(-1900, 44, 300, 60), border, pill, leftMonitor, PinPillEdge::Top, gap, inset);
        QVERIFY(!neg.inside);
        QVERIFY(leftMonitor.contains(neg.rect));
        QCOMPARE(neg.rect.top(), 111); // top edge blocked: flipped below
    }

    // Offscreen pill picture at Windows' DPI scales: whole device pixels, opaque body, soft shadow.
    void pillPicture()
    {
        for (const qreal radius : {0.0, 4.0}) {
            const PinPill::Look look {QColor(0x25, 0x29, 0x32), radius};
            for (const double dpr : {1.0, 1.25, 1.5, 2.0}) {
                const QSize body = PinPill::pillSize(u"Tab 看原文"_s, dpr);
                const int s = PinPill::shadowMargin(dpr);
                const QImage picture = PinPill::picture(u"Tab 看原文"_s, dpr, look);
                QCOMPARE(picture.size(), body + QSize(2 * s, 2 * s));
                const int edge = std::max(1, static_cast<int>(std::lround(dpr)));
                const int midY = s + body.height() / 2;
                QCOMPARE(picture.pixelColor(s + edge + 1, midY).rgba(), look.fill.rgba());
                QCOMPARE(picture.pixelColor(s, midY).alpha(), 255); // subtle rim on opaque fill
                QVERIFY(picture.pixelColor(s - 1, midY).alpha() < 255); // soft shadow outside body
                QCOMPARE(picture.pixelColor(0, 0).alpha(), 0); // clean transparent outer corner
            }
        }
    }

    // Real Win32 PinWindow + PinPill: translation/status, zoom/opacity, and
    // text/auto-OCR pills float outside small pins without covering content,
    // track movement/visibility/topmost, clear on Tab or edit/rotate, fade out,
    // and clean up owned HWNDs on close.
    void floatingPillsOutsideSmallPin()
    {
        QImage original(320, 48, QImage::Format_RGB32);
        original.fill(Qt::white);
        QImage translated(320, 48, QImage::Format_RGB32);
        translated.fill(Qt::yellow);

        std::function<void(QList<RedactLine>)> wordsRead;
        PinWindow::Host host;
        host.accent = [] { return QColor(0x25, 0x29, 0x32); };
        host.pillRadius = [] { return 4.0; };
        host.readLines = [&](const QImage&, double, std::function<void(QList<RedactLine>)> done,
                             std::shared_ptr<const std::atomic<bool>>) { wordsRead = std::move(done); };

        PinWindow pin(original, QPoint(240, 240), std::move(host));
        QVERIFY(pin.hwnd());
        QVERIFY(pin.hintPillRect().isEmpty());
        QVERIFY(pin.textPillRect().isEmpty());

        // "正在翻译…" floats outside the small strip pin and never steals focus or mouse input.
        const HWND foregroundBefore = ::GetForegroundWindow();
        pin.note(u"正在翻译…"_s, true);
        QCOMPARE(pin.hintAlpha(), 255);
        const QRect translatingRect = pin.hintPillRect();
        QVERIFY(!translatingRect.isEmpty());
        QVERIFY(!translatingRect.intersects(pin.pictureRect()));
        QCOMPARE(::GetForegroundWindow(), foregroundBefore);

        const HWND hintHwnd = pin.hintPillHwnd();
        QVERIFY(hintHwnd && ::IsWindowVisible(hintHwnd));
        QCOMPARE(::GetWindow(hintHwnd, GW_OWNER), pin.hwnd());
        const LONG_PTR exStyle = ::GetWindowLongPtrW(hintHwnd, GWL_EXSTYLE);
        for (const LONG_PTR bit : std::initializer_list<LONG_PTR> {
                 WS_EX_LAYERED, WS_EX_TRANSPARENT, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST})
            QVERIFY(exStyle & bit);
        QVERIFY(::AreDpiAwarenessContextsEqual(::GetWindowDpiAwarenessContext(hintHwnd),
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));

        // Nudging the pin moves the floating pill along with it.
        const QPoint originBefore = pin.pictureRect().topLeft();
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_RIGHT, 0);
        QCOMPARE(pin.pictureRect().topLeft(), originBefore + QPoint(1, 0));
        QCOMPARE(pin.hintPillRect().topLeft(), translatingRect.topLeft() + QPoint(1, 0));

        // Topmost flag propagates cleanly to owned pills.
        pin.setTopmost(false);
        QVERIFY((::GetWindowLongPtrW(hintHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0);
        pin.setTopmost(true);
        QVERIFY((::GetWindowLongPtrW(hintHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

        // Hiding or collapsing the pin hides its pills; restoring brings a staying pill back.
        pin.setShown(false);
        QVERIFY(pin.hintPillRect().isEmpty());
        pin.setShown(true);
        QVERIFY(!pin.hintPillRect().isEmpty());
        pin.setAllHidden(true);
        QVERIFY(pin.hintPillRect().isEmpty());
        pin.setAllHidden(false);
        QVERIFY(!pin.hintPillRect().isEmpty());
        pin.collapse(PinForm::Tile);
        QVERIFY(pin.hintPillRect().isEmpty());
        pin.expand();
        QVERIFY(!pin.hintPillRect().isEmpty());

        // Translation finishes with auto-OCR ready: "Tab 看原文" floats above and
        // "已识别文字，可拖选" floats below — neither covers the translated strip.
        QVERIFY(pin.setTranslation(translated, u"译文"_s, pin.revision()));
        pin.note(u"Tab 看原文"_s);
        RedactLine line;
        line.text = u"译文"_s;
        line.quad = QPolygonF({QPointF(10, 10), QPointF(120, 10), QPointF(120, 34), QPointF(10, 34)});
        pin.setRecognizedLines({line}, true);
        QCOMPARE(pin.hintAlpha(), 255);
        QCOMPARE(pin.textHintAlpha(), 255);
        QVERIFY(!pin.hintPillRect().isEmpty());
        QVERIFY(!pin.textPillRect().isEmpty());
        QVERIFY(!pin.hintPillRect().intersects(pin.pictureRect()));
        QVERIFY(!pin.textPillRect().intersects(pin.pictureRect()));
        QVERIFY(!pin.hintPillRect().intersects(pin.textPillRect()));

        // Pressing Tab flips side and dismisses the stale "Tab 看原文" hint immediately.
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_TAB, 0);
        QVERIFY(!pin.translationShown());
        QCOMPARE(pin.hintAlpha(), 0);
        QVERIFY(pin.hintPillRect().isEmpty());

        // Zoom and opacity pills also float outside the pin and fade out cleanly.
        ::SendMessageW(pin.hwnd(), WM_KEYDOWN, VK_ADD, 0);
        QCOMPARE(pin.hintAlpha(), 255);
        QVERIFY(!pin.hintPillRect().isEmpty());
        QVERIFY(!pin.hintPillRect().intersects(pin.pictureRect()));
        ::SendMessageW(pin.hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, -WHEEL_DELTA), MAKELPARAM(260, 260));
        QCOMPARE(pin.hintAlpha(), 255);
        QVERIFY(!pin.hintPillRect().isEmpty());
        QVERIFY(!pin.hintPillRect().intersects(pin.pictureRect()));
        QTRY_COMPARE_WITH_TIMEOUT(pin.hintAlpha(), 0, 2500);
        QVERIFY(pin.hintPillRect().isEmpty());

        // Rotating while a staying translation note is active clears it so it never gets stuck.
        pin.note(u"正在翻译…"_s, true);
        QCOMPARE(pin.hintAlpha(), 255);
        pin.rotate(true);
        QCOMPARE(pin.hintAlpha(), 0);
        QVERIFY(pin.hintPillRect().isEmpty());

        // Tiny pin with BOTH status pill and text-pick pill: neither is dropped, neither covers the pin or each other.
        QImage tiny(50, 24, QImage::Format_RGB32);
        tiny.fill(Qt::white);
        pin.setPicture(tiny, {});
        pin.note(u"正在翻译…"_s, true);
        pin.beginTextPick();
        const QRect tinyHint = pin.hintPillRect();
        const QRect tinyText = pin.textPillRect();
        QVERIFY(!tinyHint.isEmpty());
        QVERIFY(!tinyText.isEmpty());
        QVERIFY(!tinyHint.intersects(pin.pictureRect()));
        QVERIFY(!tinyText.intersects(pin.pictureRect()));
        QVERIFY(!tinyHint.intersects(tinyText));

        const HWND textHwnd = pin.textPillHwnd();
        QVERIFY(textHwnd && ::IsWindowVisible(textHwnd));
        pin.close();
        QVERIFY(!::IsWindow(hintHwnd));
        QVERIFY(!::IsWindow(textHwnd));
    }
};

QTEST_MAIN(TestPinWindow)
#include "tst_pinwindow.moc"
