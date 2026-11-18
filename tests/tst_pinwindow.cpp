// The pin window's text pick (GitHub #258): the pill that says what to do
// stays while the words are being read, then fades off the picture so it
// does not sit on the bottom line of what one wants to pick; a pick brings
// it back for a moment. And its key card (GitHub #343, PinKeyCard): the
// rows, when it shows, where, its picture, and what the pin tells about it.
#include "PinKeyCard.h"
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
        host.readLines = [&](const QImage&, double, std::function<void(QList<RedactLine>)> done) { wordsRead = std::move(done); };
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
        QCOMPARE(texts(rows), QStringList({u"编辑"_s, u"复制"_s, u"保存到“屏幕截图”"_s, u"旋转与翻转"_s, u"缩放 / 透明度"_s,
                                  u"收成小块"_s, u"关闭"_s}));
        QCOMPARE(rows[0].keys, QStringList {keys.text(KeyAction::PinEdit)});
        QCOMPARE(rows[1].keys, QStringList {keys.text(KeyAction::PinCopy)});
        QCOMPARE(rows[3].keys, QStringList({keys.text(KeyAction::PinRotateRight), keys.text(KeyAction::PinFlipHorizontal),
                                   keys.text(KeyAction::PinFlipVertical)}));
        QCOMPARE(rows[4].keys, QStringList({u"滚轮"_s, u"Ctrl+滚轮"_s}));
        QCOMPARE(rows[6].keys, QStringList {keys.text(KeyAction::PinClose)});

        rows = pinKeyRows(keys, true, true, true); // text recognition there, a translated pin showing its translation
        QCOMPARE(texts(rows), QStringList({u"编辑"_s, u"复制"_s, u"保存到“屏幕截图”"_s, u"识别文字"_s, u"选择文字"_s, u"显示原文"_s,
                                  u"旋转与翻转"_s, u"缩放 / 透明度"_s, u"收成小块"_s, u"关闭"_s}));
        QCOMPARE(rows[5].keys, QStringList {u"Tab"_s});
        QCOMPARE(texts(pinKeyRows(keys, false, true, false))[3], u"显示译文"_s);

        keys.set(KeyAction::PinCopy, QKeyCombination()); // cleared in the settings
        keys.set(KeyAction::PinFlipHorizontal, QKeyCombination());
        keys.set(KeyAction::PinFlipVertical, QKeyCombination());
        keys.set(KeyAction::PinEdit, QKeyCombination(Qt::Key_F2));
        rows = pinKeyRows(keys, false, false, false);
        QCOMPARE(texts(rows), QStringList({u"编辑"_s, u"保存到“屏幕截图”"_s, u"旋转与翻转"_s, u"缩放 / 透明度"_s, u"收成小块"_s,
                                  u"关闭"_s}));
        QCOMPARE(rows[0].keys, QStringList {u"F2"_s});
        QCOMPARE(rows[2].keys, QStringList {keys.text(KeyAction::PinRotateRight)});
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
                const QSize card = PinKeyCard::cardSize(rows, dpr);
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
        const QSize size = PinKeyCard::cardSize(rows, 1.0);
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
        QCOMPARE(card.rect().size(), PinKeyCard::cardSize(rows, 1.5));
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
};

QTEST_MAIN(TestPinWindow)
#include "tst_pinwindow.moc"
