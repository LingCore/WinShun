#include "InputMode.h"

#include <optional>

namespace ws::win {

namespace {

std::optional<InputMode> readMode(HIMC context)
{
    InputMode mode;
    if (!::ImmGetConversionStatus(context, &mode.conversion, &mode.sentence))
        return std::nullopt;
    mode.open = ::ImmGetOpenStatus(context) != FALSE;
    return mode;
}

} // namespace

LettersSwitch switchToLetters(HWND window)
{
    LettersSwitch done;
    const HIMC context = window ? ::ImmGetContext(window) : nullptr;
    if (!context)
        return done; // none: Qt has not given it one (or the field takes no input method)
    if (const std::optional<InputMode> before = readMode(context)) {
        done.context = context;
        done.before = *before;
        // Always (lettersMode): the conversion mode first, as an input
        // method that keeps the two in step closes itself as the native bit
        // goes.
        const InputMode letters = lettersMode(*before);
        ::ImmSetConversionStatus(context, letters.conversion, letters.sentence);
        ::ImmSetOpenStatus(context, letters.open);
        done.after = readMode(context).value_or(letters);
        done.changed = !sameLetters(done.after, done.before);
    }
    ::ImmReleaseContext(window, context);
    return done;
}

void restoreInputMode(const LettersSwitch& done)
{
    if (!done.changed || !done.context)
        return;
    // The thread's default context, there as long as the thread is: also
    // when Qt has taken it from the window meanwhile (the focus elsewhere).
    const std::optional<InputMode> now = readMode(done.context);
    if (!now || !sameLetters(*now, done.after))
        return; // the user switched it meanwhile: theirs
    ::ImmSetOpenStatus(done.context, done.before.open); // open first, then the native bit
    ::ImmSetConversionStatus(done.context, done.before.conversion, done.before.sentence);
}

} // namespace ws::win
