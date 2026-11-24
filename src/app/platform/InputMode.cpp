#include "InputMode.h"

#include "WindowEffects.h"

#include <commctrl.h>
#include <msctf.h>
#include <wrl/client.h>

#include <optional>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace ws::win {

namespace {

constexpr UINT_PTR kWatchSubclassId = 0x5749; // "WI": InputModeWatch
constexpr DWORD kLettersBits = IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE;

struct TsfMode {
    std::optional<bool> open;
    std::optional<DWORD> conversion;
};

std::optional<InputMode> readImmMode(HIMC context)
{
    InputMode mode;
    if (!::ImmGetConversionStatus(context, &mode.conversion, &mode.sentence))
        return std::nullopt;
    mode.open = ::ImmGetOpenStatus(context) != FALSE;
    return mode;
}

TsfMode readTsfMode()
{
    TsfMode out;
    ::ITfThreadMgr* mgr = nullptr;
    DWORD client = TF_CLIENTID_NULL;
    if (!textInputThread(&mgr, &client) || !mgr)
        return out;
    ComPtr<ITfCompartmentMgr> compMgr;
    if (FAILED(mgr->QueryInterface(IID_PPV_ARGS(&compMgr))) || !compMgr)
        return out;
    const auto readI4 = [&](const GUID& guid) -> std::optional<LONG> {
        ComPtr<ITfCompartment> comp;
        if (FAILED(compMgr->GetCompartment(guid, &comp)) || !comp)
            return std::nullopt;
        VARIANT var;
        ::VariantInit(&var);
        std::optional<LONG> value;
        if (SUCCEEDED(comp->GetValue(&var)) && var.vt == VT_I4)
            value = var.lVal;
        ::VariantClear(&var);
        return value;
    };
    if (const auto open = readI4(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE))
        out.open = *open != 0;
    if (const auto conv = readI4(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION))
        out.conversion = static_cast<DWORD>(*conv);
    return out;
}

void writeTsfMode(bool open, std::optional<DWORD> conversion, bool openFirst)
{
    ::ITfThreadMgr* mgr = nullptr;
    DWORD client = TF_CLIENTID_NULL;
    if (!textInputThread(&mgr, &client) || !mgr || client == TF_CLIENTID_NULL)
        return;
    ComPtr<ITfCompartmentMgr> compMgr;
    if (FAILED(mgr->QueryInterface(IID_PPV_ARGS(&compMgr))) || !compMgr)
        return;
    const auto writeI4 = [&](const GUID& guid, LONG value) {
        ComPtr<ITfCompartment> comp;
        if (FAILED(compMgr->GetCompartment(guid, &comp)) || !comp)
            return;
        VARIANT var;
        ::VariantInit(&var);
        var.vt = VT_I4;
        var.lVal = value;
        comp->SetValue(client, &var);
        ::VariantClear(&var);
    };
    const auto setConv = [&] {
        if (conversion)
            writeI4(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, static_cast<LONG>(*conversion));
    };
    const auto setOpen = [&] {
        writeI4(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, open ? 1 : 0);
    };
    if (openFirst) {
        setOpen();
        setConv();
    } else {
        setConv();
        setOpen();
    }
}

} // namespace

LettersSwitch switchToLetters(HWND window)
{
    LettersSwitch done;
    const HIMC context = window ? ::ImmGetContext(window) : nullptr;
    if (!context)
        return done; // none: Qt has not given it one (or the field takes no input method)
    if (const std::optional<InputMode> immBefore = readImmMode(context)) {
        const TsfMode tsfBefore = readTsfMode();
        const InputMode before = combineInputMode(*immBefore, tsfBefore.open, tsfBefore.conversion);
        done.context = context;
        done.before = before;
        done.tsfBeforeConversion = tsfBefore.conversion;
        // Always (lettersMode): the conversion mode first, as an input
        // method that keeps the two in step closes itself as the native bit
        // goes. Write both IMM32 and TSF compartments so a freshly-activated
        // TSF text service cannot lag behind IMM32 on first boot.
        const InputMode letters = lettersMode(before);
        ::ImmSetConversionStatus(context, letters.conversion, letters.sentence);
        ::ImmSetOpenStatus(context, letters.open);
        std::optional<DWORD> tsfLettersConv;
        if (tsfBefore.conversion)
            tsfLettersConv = *tsfBefore.conversion & ~kLettersBits;
        writeTsfMode(false, tsfLettersConv, /*openFirst=*/false);
        const InputMode immAfter = readImmMode(context).value_or(letters);
        const TsfMode tsfAfter = readTsfMode();
        done.after = combineInputMode(immAfter, tsfAfter.open, tsfAfter.conversion);
        done.changed = !sameLetters(done.after, done.before);
    }
    ::ImmReleaseContext(window, context);
    return done;
}

bool inputModeNeedsLetters(HWND window)
{
    const HIMC context = window ? ::ImmGetContext(window) : nullptr;
    if (!context)
        return false;
    bool needs = false;
    if (const std::optional<InputMode> imm = readImmMode(context)) {
        const TsfMode tsf = readTsfMode();
        needs = needsLettersSwitch(combineInputMode(*imm, tsf.open, tsf.conversion));
    }
    ::ImmReleaseContext(window, context);
    return needs;
}

void restoreInputMode(const LettersSwitch& done)
{
    if (!done.changed || !done.context)
        return;
    // The thread's default context, there as long as the thread is: also
    // when Qt has taken it from the window meanwhile (the focus elsewhere).
    const std::optional<InputMode> immNow = readImmMode(done.context);
    if (!immNow)
        return;
    const TsfMode tsfNow = readTsfMode();
    const InputMode now = combineInputMode(*immNow, tsfNow.open, tsfNow.conversion);
    if (!sameLetters(now, done.after))
        return; // the user switched it meanwhile: theirs
    ::ImmSetOpenStatus(done.context, done.before.open); // open first, then the native bit
    ::ImmSetConversionStatus(done.context, done.before.conversion, done.before.sentence);
    writeTsfMode(done.before.open, done.tsfBeforeConversion, /*openFirst=*/true);
}

InputModeWatch::InputModeWatch(HWND window, std::function<void()> onImeEvent, std::function<void()> onUserInput)
    : m_window(window)
    , m_onImeEvent(std::move(onImeEvent))
    , m_onUserInput(std::move(onUserInput))
{
    if (m_window && !::SetWindowSubclass(m_window, &InputModeWatch::proc, kWatchSubclassId, reinterpret_cast<DWORD_PTR>(this)))
        m_window = nullptr;
}

InputModeWatch::~InputModeWatch()
{
    if (m_window && ::IsWindow(m_window))
        ::RemoveWindowSubclass(m_window, &InputModeWatch::proc, kWatchSubclassId);
    m_window = nullptr;
}

LRESULT CALLBACK InputModeWatch::proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    auto* self = reinterpret_cast<InputModeWatch*>(data);
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Any fresh key press (including Shift, Ctrl+Space, or typing) ends
        // the post-open settle guard before the IME acts on it, so a manual
        // switch back to Chinese inside the search box is never undone.
        if (self && self->m_onUserInput && (lParam & (1 << 30)) == 0)
            self->m_onUserInput();
        break;
    case WM_INPUTLANGCHANGE:
        if (self && self->m_onUserInput)
            self->m_onUserInput();
        break;
    case WM_IME_SETCONTEXT:
        if (self && self->m_onImeEvent && wParam != FALSE)
            self->m_onImeEvent();
        break;
    case WM_IME_NOTIFY:
        if (self && self->m_onImeEvent && (wParam == IMN_SETOPENSTATUS || wParam == IMN_SETCONVERSIONMODE))
            self->m_onImeEvent();
        break;
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(hwnd, &InputModeWatch::proc, kWatchSubclassId);
        if (self)
            self->m_window = nullptr;
        break;
    default:
        break;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace ws::win
